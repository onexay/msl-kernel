# MSL kernel

The Linux kernel [MSL](https://github.com/onexay/msl) runs in its VM: unmodified kernel.org Linux (arm64), configured with Apple's 6.18 config (`base.config`, extracted from the kernel Apple's `container` ships) plus `msl.fragment`. The fragment adds XHCI/usb-storage, quota, nfsd, device-mapper, NBD and KVM (for msl's `nestedVirtualization`), and switches to 16 KiB pages to match the host ([onexay/msl#48](https://github.com/onexay/msl/issues/48)).

msl bundles a published release of this kernel. The release it uses is pinned in msl's `kernel/release.tag` and `kernel/release.sha256`.

## Build

```console
$ ./build.sh              # builds in Debian trixie with Apple's `container` → out/{Image,config,tag}
```

`build.sh` and CI both run `build-linux.sh`. To try a local build in msl: `MSL_KERNEL_OUT=<this repo>/out scripts/build.sh` in an msl checkout.

## Release

Tags come from their inputs: `tag.sh` prints `kernel-<linux>-msl-<hash>`, where the hash covers the Linux version, `base.config` and `msl.fragment`. There's no manual version bump.

1. Push the config change. CI's *Kernel* workflow builds it on `ubuntu-24.04-arm` and uploads an artifact named after the tag.
2. Run `./publish.sh`. It downloads that artifact and publishes it under the tag, with `release.sha256` and the corresponding kernel.org source.
3. In msl, run `scripts/pin.sh kernel <tag>` and commit the pin.

## Contributing

Same rules as msl: see its [CONTRIBUTING.md](https://github.com/onexay/msl/blob/main/CONTRIBUTING.md). Sign off every commit (`git commit -s`, [DCO](https://developercertificate.org/)).

## Licence

The scripts and config in this repository are Apache-2.0 ([LICENSE](LICENSE)). The Linux kernel built from them is GPL-2.0. Each release attaches its corresponding source.
