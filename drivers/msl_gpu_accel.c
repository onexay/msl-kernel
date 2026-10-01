// SPDX-License-Identifier: GPL-2.0
/*
 * msl-accel: guest driver for MSL's accelerator device (virtio ID 63).
 * Design: onexay/msl docs/internals/accel.md. Userspace interface: msl_gpu_accel.h.
 *
 * The device has one shared-memory window and two virtqueues:
 *   0 control: CREATE, DESTROY, KICK, CANCEL, answered at once
 *   1 waits:   WAIT, held by msld until the context's guest seq moves
 * Each message is struct wire_msg out and struct wire_reply in.
 *
 * msld backs only the window ranges of live contexts; a guest access anywhere
 * else stops the VM. So mmap is bounded to the fd's own context, and DESTROY
 * is sent only from release, after the last mapping of the fd is gone.
 */
#include <linux/cgroup.h>
#include <linux/cgroup_namespace.h>
#include <linux/completion.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>

#include "msl_gpu_accel.h"

#define VIRTIO_ID_MSL_GPU_ACCEL 63
#define CTL_TIMEOUT (5 * HZ)

enum { MSG_CREATE = 1, MSG_DESTROY, MSG_KICK, MSG_WAIT, MSG_CANCEL };

struct wire_msg {
	__le32 type;
	__le32 ctx;
	__le64 arg;       /* CREATE: data bytes; WAIT: seq */
	__le32 pid;       /* CREATE */
	__le32 pad;
	char engine[MSL_GPU_ACCEL_ENGINE_MAX];  /* CREATE */
	char distro[64];  /* CREATE: /msl/<id> cgroup of the caller, or "" */
};

struct wire_reply {
	__le32 status;    /* 0, or a positive errno */
	__le32 ctx;
	__le64 offset;    /* CREATE: window offset of the context */
	__le64 len;       /* CREATE: header + data */
};

struct accel_ctx;

struct req {
	struct wire_msg msg;
	struct wire_reply reply;
	struct completion done;
	struct accel_ctx *ctx;  /* holds a reference while the request is out */
	bool abandoned;         /* the caller gave up: the completion frees it */
};

struct accel_dev {
	struct virtio_device *vdev;
	struct virtqueue *ctl, *waits;
	spinlock_t lock;        /* both virtqueues, and req hand-off */
	struct miscdevice misc;
	u64 phys, len;
};

struct accel_ctx {
	struct kref ref;
	struct accel_dev *dev;
	struct mutex lock;      /* CREATE vs mmap */
	u32 id;
	u64 offset, len;        /* 0 until CREATE */
	u8 *hdr;                /* kernel mapping of the header */
	atomic_t waits;         /* outstanding WAITs, abandoned ones included */
};

static struct accel_dev *accel;  /* one device */

static inline u64 *hdr_word(struct accel_ctx *c, unsigned int off)
{
	return (u64 *)(c->hdr + off);
}

/* May run in the completion path (atomic): release unmapped the header. */
static void ctx_free(struct kref *ref)
{
	kfree(container_of(ref, struct accel_ctx, ref));
}

static void ctx_put(struct accel_ctx *c)
{
	kref_put(&c->ref, ctx_free);
}

/* Called under dev->lock. */
static void req_complete(struct req *r)
{
	if (r->abandoned) {
		if (le32_to_cpu(r->msg.type) == MSG_WAIT)
			atomic_dec(&r->ctx->waits);
		if (r->ctx)
			ctx_put(r->ctx);
		kfree(r);
	} else {
		complete(&r->done);
	}
}

static void vq_done(struct virtqueue *vq)
{
	struct accel_dev *d = vq->vdev->priv;
	unsigned long flags;
	unsigned int len;
	struct req *r;

	spin_lock_irqsave(&d->lock, flags);
	do {
		virtqueue_disable_cb(vq);
		while ((r = virtqueue_get_buf(vq, &len))) {
			if (len != sizeof(r->reply))
				r->reply.status = cpu_to_le32(EPROTO);
			req_complete(r);
		}
	} while (!virtqueue_enable_cb(vq));
	spin_unlock_irqrestore(&d->lock, flags);
}

static struct req *req_new(struct accel_ctx *c, u32 type, u64 arg)
{
	struct req *r = kzalloc(sizeof(*r), GFP_KERNEL);

	if (!r)
		return NULL;
	r->msg.type = cpu_to_le32(type);
	r->msg.ctx = cpu_to_le32(c->id);
	r->msg.arg = cpu_to_le64(arg);
	init_completion(&r->done);
	kref_get(&c->ref);
	r->ctx = c;
	return r;
}

/* Frees a request nobody else holds. */
static void req_drop(struct req *r)
{
	ctx_put(r->ctx);
	kfree(r);
}

static int submit(struct accel_dev *d, struct virtqueue *vq, struct req *r)
{
	struct scatterlist out, in, *sgs[2] = { &out, &in };
	unsigned long flags;
	bool notify = false;
	int err;

	sg_init_one(&out, &r->msg, sizeof(r->msg));
	sg_init_one(&in, &r->reply, sizeof(r->reply));
	spin_lock_irqsave(&d->lock, flags);
	err = virtqueue_add_sgs(vq, sgs, 1, 1, r, GFP_ATOMIC);
	if (!err)
		notify = virtqueue_kick_prepare(vq);
	spin_unlock_irqrestore(&d->lock, flags);
	if (notify)
		virtqueue_notify(vq);
	return err == -ENOSPC ? -EBUSY : err;
}

/*
 * Waits for a request's reply. On timeout or a signal the request is
 * abandoned: msld still holds it, and the completion frees it. Returns the
 * reply's status as a negative errno, or the wait's error.
 */
static long await(struct accel_dev *d, struct req *r, unsigned long timeout, bool interruptible)
{
	unsigned long flags;
	long ret;

	ret = interruptible ? wait_for_completion_interruptible_timeout(&r->done, timeout)
			    : wait_for_completion_timeout(&r->done, timeout);
	if (ret > 0)
		return -(long)le32_to_cpu(r->reply.status);
	spin_lock_irqsave(&d->lock, flags);
	if (completion_done(&r->done)) {
		spin_unlock_irqrestore(&d->lock, flags);
		return -(long)le32_to_cpu(r->reply.status);
	}
	r->abandoned = true;
	spin_unlock_irqrestore(&d->lock, flags);
	return ret == 0 ? -ETIMEDOUT : -EINTR;
}

/* A control message that doesn't need its reply: the completion frees it. */
static void send_async(struct accel_dev *d, struct accel_ctx *c, u32 type)
{
	struct req *r = req_new(c, type, 0);

	if (!r)
		return;
	r->abandoned = true;
	if (submit(d, d->ctl, r))
		req_drop(r);
}

/* The caller's distro: the <id> of its /msl/<id>/... cgroup, or "". */
static void caller_distro(char *out, size_t n)
{
	struct cgroup *cgrp;
	char *path, *id, *end;
	bool got;

	out[0] = '\0';
	path = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!path)
		return;
	rcu_read_lock();
	cgrp = task_dfl_cgroup(current);
	got = cgroup_tryget(cgrp);
	rcu_read_unlock();
	if (!got)
		goto out;
	if (cgroup_path_ns(cgrp, path, PATH_MAX, &init_cgroup_ns) >= 0 && str_has_prefix(path, "/msl/")) {
		id = path + strlen("/msl/");
		end = strchrnul(id, '/');
		*end = '\0';
		strscpy(out, id, n);
	}
	cgroup_put(cgrp);
out:
	kfree(path);
}

static long do_create(struct accel_ctx *c, struct msl_gpu_accel_create __user *p)
{
	struct accel_dev *d = c->dev;
	struct msl_gpu_accel_create a;
	struct req *r;
	long err;
	u64 off, len;

	if (copy_from_user(&a, p, sizeof(a)))
		return -EFAULT;
	a.engine[sizeof(a.engine) - 1] = '\0';
	if (!a.engine[0] || !a.data_bytes)
		return -EINVAL;
	mutex_lock(&c->lock);
	err = -EBUSY;
	if (c->id)
		goto unlock;
	err = -ENOMEM;
	r = req_new(c, MSG_CREATE, a.data_bytes);
	if (!r)
		goto unlock;
	r->msg.pid = cpu_to_le32(task_tgid_vnr(current));
	strscpy(r->msg.engine, a.engine, sizeof(r->msg.engine));
	caller_distro(r->msg.distro, sizeof(r->msg.distro));
	err = submit(d, d->ctl, r);
	if (err) {
		req_drop(r);
		goto unlock;
	}
	/* Starting an engine can take a moment; not interruptible, so a context
	 * msld created is never lost. On timeout, release still sends DESTROY. */
	err = await(d, r, CTL_TIMEOUT * 6, false);
	if (err) {
		if (err != -ETIMEDOUT)
			req_drop(r);
		goto unlock;
	}
	off = le64_to_cpu(r->reply.offset);
	len = le64_to_cpu(r->reply.len);
	c->id = le32_to_cpu(r->reply.ctx);
	req_drop(r);
	if (len < MSL_GPU_ACCEL_HDR_SIZE || !PAGE_ALIGNED(off) || !PAGE_ALIGNED(len) ||
	    off >= d->len || len > d->len - off) {
		dev_err(&d->vdev->dev, "CREATE: bad range %llx+%llx\n", off, len);
		err = -EPROTO;
		goto unlock;
	}
	c->hdr = memremap(d->phys + off, MSL_GPU_ACCEL_HDR_SIZE, MEMREMAP_WB);
	if (!c->hdr) {
		err = -ENOMEM;
		goto unlock;
	}
	if (READ_ONCE(*hdr_word(c, MSL_GPU_ACCEL_OFF_MAGIC)) != MSL_GPU_ACCEL_HDR_MAGIC) {
		dev_err(&d->vdev->dev, "CREATE: context %u has no header\n", c->id);
		err = -EPROTO;
		goto unlock;
	}
	c->offset = off;
	c->len = len;
	a.map_len = len;
	a.ctx = c->id;
	err = copy_to_user(p, &a, sizeof(a)) ? -EFAULT : 0;
unlock:
	/* A context that failed after msld created it is torn down at release:
	 * c->id is set, so release sends DESTROY. */
	mutex_unlock(&c->lock);
	return err;
}

static bool ctx_dead(struct accel_ctx *c)
{
	return READ_ONCE(*hdr_word(c, MSL_GPU_ACCEL_OFF_STATE)) != MSL_GPU_ACCEL_LIVE;
}

/*
 * Wakes the engine. Skipped while kick_pending is set: msld clears it before
 * it wakes the engine, so that earlier KICK's wake comes after the caller's
 * srv_seq bump.
 */
static long do_kick(struct accel_ctx *c)
{
	u64 *pending = hdr_word(c, MSL_GPU_ACCEL_OFF_KICK);
	struct req *r;
	int err;

	if (ctx_dead(c))
		return -EIO;
	smp_mb();  /* the caller's srv_seq bump before the kick_pending read */
	if (READ_ONCE(*pending) || cmpxchg(pending, 0, 1) != 0)
		return 0;
	r = req_new(c, MSG_KICK, 0);
	if (!r)
		goto fail;
	r->abandoned = true;
	err = submit(c->dev, c->dev->ctl, r);
	if (!err)
		return 0;
	req_drop(r);
fail:
	WRITE_ONCE(*pending, 0);
	return -EBUSY;
}

static long do_wait(struct accel_ctx *c, struct msl_gpu_accel_wait __user *p)
{
	struct msl_gpu_accel_wait w;
	struct req *r;
	long err, ret;

	if (copy_from_user(&w, p, sizeof(w)))
		return -EFAULT;
	if (ctx_dead(c))
		return -EIO;
	if (smp_load_acquire(hdr_word(c, MSL_GPU_ACCEL_OFF_GST_SEQ)) != w.seq)
		return 0;
	if (atomic_inc_return(&c->waits) > MSL_GPU_ACCEL_MAX_WAITS) {
		atomic_dec(&c->waits);
		return -EBUSY;
	}
	r = req_new(c, MSG_WAIT, w.seq);
	if (!r) {
		atomic_dec(&c->waits);
		return -ENOMEM;
	}
	err = submit(c->dev, c->dev->waits, r);
	if (err) {
		atomic_dec(&c->waits);
		req_drop(r);
		return err;
	}
	ret = wait_for_completion_interruptible_timeout(&r->done, usecs_to_jiffies(w.timeout_us ? w.timeout_us : 10000));
	if (ret > 0) {
		err = -(long)le32_to_cpu(r->reply.status);
		err = err == -EPIPE ? -EIO : err;
	} else {
		/* msld still holds it: ask for it back (other WAITs of the context
		 * return early too, which is allowed), and wait for it, so its slot
		 * is free when this returns. Abandoned only if msld doesn't answer. */
		send_async(c->dev, c, MSG_CANCEL);
		if (await(c->dev, r, CTL_TIMEOUT, false) == -ETIMEDOUT)
			return ret == 0 ? -ETIMEDOUT : -EINTR;
		err = ret == 0 ? -ETIMEDOUT : -EINTR;
	}
	atomic_dec(&c->waits);
	req_drop(r);
	return err;
}

static long accel_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct accel_ctx *c = f->private_data;

	if (cmd == MSL_GPU_ACCEL_CREATE)
		return do_create(c, (void __user *)arg);
	if (!READ_ONCE(c->len))
		return -ENXIO;
	switch (cmd) {
	case MSL_GPU_ACCEL_KICK:
		return do_kick(c);
	case MSL_GPU_ACCEL_WAIT:
		return do_wait(c, (void __user *)arg);
	}
	return -ENOTTY;
}

/* Cacheable mapping of [offset, offset + len) of the fd's own context. */
static int accel_mmap(struct file *f, struct vm_area_struct *vma)
{
	struct accel_ctx *c = f->private_data;
	u64 off = (u64)vma->vm_pgoff << PAGE_SHIFT, len = vma->vm_end - vma->vm_start;
	int err = -ENXIO;

	mutex_lock(&c->lock);
	if (!c->len)
		goto out;
	err = -EINVAL;
	if (off >= c->len || len > c->len - off)
		goto out;
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	err = remap_pfn_range(vma, vma->vm_start, (c->dev->phys + c->offset + off) >> PAGE_SHIFT,
			      len, vma->vm_page_prot);
out:
	mutex_unlock(&c->lock);
	return err;
}

static int accel_open(struct inode *inode, struct file *f)
{
	struct accel_ctx *c;

	if (!accel)
		return -ENODEV;
	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	kref_init(&c->ref);
	mutex_init(&c->lock);
	atomic_set(&c->waits, 0);
	c->dev = accel;
	f->private_data = c;
	return 0;
}

/* The last reference to the fd, so no mapping of the context is left. */
static int accel_release(struct inode *inode, struct file *f)
{
	struct accel_ctx *c = f->private_data;
	struct req *r;

	if (c->id) {
		r = req_new(c, MSG_DESTROY, 0);
		if (r && !submit(c->dev, c->dev->ctl, r)) {
			if (await(c->dev, r, CTL_TIMEOUT, false) != -ETIMEDOUT)
				req_drop(r);
		} else if (r) {
			req_drop(r);
		}
	}
	if (c->hdr)
		memunmap(c->hdr);
	c->hdr = NULL;
	ctx_put(c);
	return 0;
}

static const struct file_operations accel_fops = {
	.owner = THIS_MODULE,
	.open = accel_open,
	.release = accel_release,
	.unlocked_ioctl = accel_ioctl,
	.mmap = accel_mmap,
};

static int accel_probe(struct virtio_device *vdev)
{
	struct virtqueue_info info[] = { { "control", vq_done }, { "waits", vq_done } };
	struct virtqueue *vqs[2];
	struct virtio_shm_region shm;
	struct accel_dev *d;
	int err;

	if (accel)
		return -EBUSY;
	if (!virtio_get_shm_region(vdev, &shm, 0)) {
		dev_err(&vdev->dev, "no shared memory region 0\n");
		return -ENODEV;
	}
	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->vdev = vdev;
	d->phys = shm.addr;
	d->len = shm.len;
	spin_lock_init(&d->lock);
	vdev->priv = d;
	err = virtio_find_vqs(vdev, 2, vqs, info, NULL);
	if (err)
		goto free;
	d->ctl = vqs[0];
	d->waits = vqs[1];
	virtio_device_ready(vdev);
	d->misc = (struct miscdevice){ .minor = MISC_DYNAMIC_MINOR, .name = "msl_gpu_accel", .fops = &accel_fops, .mode = 0666 };
	err = misc_register(&d->misc);
	if (err)
		goto reset;
	accel = d;
	dev_info(&vdev->dev, "window %llu MiB at 0x%llx, queues %u/%u\n", d->len >> 20, d->phys,
		 virtqueue_get_vring_size(d->ctl), virtqueue_get_vring_size(d->waits));
	return 0;
reset:
	virtio_reset_device(vdev);
	vdev->config->del_vqs(vdev);
free:
	kfree(d);
	return err;
}

/* Not expected at runtime (the device lives as long as the VM). */
static void accel_remove(struct virtio_device *vdev)
{
	struct accel_dev *d = vdev->priv;

	misc_deregister(&d->misc);
	accel = NULL;
	virtio_reset_device(vdev);
	vdev->config->del_vqs(vdev);
	kfree(d);
}

static const struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_MSL_GPU_ACCEL, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct virtio_driver msl_gpu_accel_driver = {
	.driver.name = "msl_gpu_accel",
	.id_table = id_table,
	.probe = accel_probe,
	.remove = accel_remove,
};
module_virtio_driver(msl_gpu_accel_driver);

MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("MSL accelerator device");
MODULE_LICENSE("GPL");
