/* SPDX-License-Identifier: (GPL-2.0 WITH Linux-syscall-note) OR Apache-2.0 */
/*
 * msl-accel: MSL's accelerator device (onexay/msl: docs/internals/accel.md).
 * Userspace interface of /dev/msl_gpu_accel, and the context header layout shared
 * by the guest driver, guest clients, msld and host engines.
 *
 * One fd is one context:
 *   fd = open("/dev/msl_gpu_accel", O_RDWR | O_CLOEXEC);
 *   ioctl(fd, MSL_GPU_ACCEL_CREATE, &create);        engine name, data bytes
 *   p = mmap(0, create.map_len, ..., MAP_SHARED, fd, 0);
 *   p[0, MSL_GPU_ACCEL_HDR_SIZE) is the header, the rest is the engine's data area.
 *   ioctl(fd, MSL_GPU_ACCEL_KICK)                     wake the engine
 *   ioctl(fd, MSL_GPU_ACCEL_WAIT, &wait)              sleep until the guest seq moves
 *   close(fd), after munmap: the context and its engine go away.
 */
#ifndef MSL_GPU_ACCEL_H
#define MSL_GPU_ACCEL_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#elif defined(__linux__)
#include <linux/types.h>
#include <sys/ioctl.h>
#else  /* macOS: host engines use only the header layout */
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint32_t __u32;
typedef uint64_t __u64;
#endif

#define MSL_GPU_ACCEL_ENGINE_MAX 32   /* engine name, including the NUL */

struct msl_gpu_accel_create {
	char  engine[MSL_GPU_ACCEL_ENGINE_MAX];  /* in: registered engine name */
	__u64 data_bytes;  /* in: size of the data area after the header */
	__u64 map_len;     /* out: header + data, rounded up; mmap this */
	__u32 ctx;         /* out: context id, for logs */
	__u32 pad;
};

/* Returns 0 once the guest seq differs from `seq`, or early (spuriously);
 * callers loop. -ETIMEDOUT after timeout_us (0: 10 ms), -EIO if the engine is
 * gone, -EBUSY with MSL_GPU_ACCEL_MAX_WAITS already outstanding on the context. */
struct msl_gpu_accel_wait {
	__u64 seq;
	__u32 timeout_us;
	__u32 pad;
};

#define MSL_GPU_ACCEL_CREATE _IOWR('A', 0x10, struct msl_gpu_accel_create)
#define MSL_GPU_ACCEL_KICK   _IO('A', 0x11)
#define MSL_GPU_ACCEL_WAIT   _IOW('A', 0x12, struct msl_gpu_accel_wait)

#define MSL_GPU_ACCEL_MAX_WAITS 4

/*
 * Context header: the first MSL_GPU_ACCEL_HDR_SIZE bytes of the mapping. Every
 * word is a little-endian u64 on its own 64-byte line.
 *
 *   srv_seq       bumped by the guest client when it has work for the engine
 *   srv_sleeping  set by the engine before it sleeps on srv_seq
 *   gst_seq       bumped by the engine when it has results for the client
 *   gst_sleeping  set by the client before MSL_GPU_ACCEL_WAIT
 *   relay_seq     bumped by the engine after gst_seq, to make msld release WAITs
 *   kick_pending  set by the driver when it queues a KICK; msld clears it
 *                 before waking the engine, so a KICK is skipped only while
 *                 an earlier one is certain to wake the engine afterwards
 *   state         MSL_GPU_ACCEL_LIVE, or MSL_GPU_ACCEL_DEAD once the engine is gone
 */
#define MSL_GPU_ACCEL_HDR_SIZE      16384
#define MSL_GPU_ACCEL_HDR_MAGIC     0x3158544341534dULL  /* "MSACTX1" */
#define MSL_GPU_ACCEL_OFF_MAGIC     0
#define MSL_GPU_ACCEL_OFF_STATE     64
#define MSL_GPU_ACCEL_OFF_SRV_SEQ   128
#define MSL_GPU_ACCEL_OFF_SRV_SLEEP 192
#define MSL_GPU_ACCEL_OFF_GST_SEQ   256
#define MSL_GPU_ACCEL_OFF_GST_SLEEP 320
#define MSL_GPU_ACCEL_OFF_RELAY_SEQ 384
#define MSL_GPU_ACCEL_OFF_KICK      448

#define MSL_GPU_ACCEL_LIVE 0
#define MSL_GPU_ACCEL_DEAD 1

#endif
