# MSL Linux Kernel

[![Kernel](https://github.com/onexay/msl-kernel/actions/workflows/kernel.yml/badge.svg)](https://github.com/onexay/msl-kernel/actions/workflows/kernel.yml)

This repository builds an arm64 Linux kernel for MSL and publishes versioned kernel releases.

MSL uses an unmodified Linux source release with Apple's 6.18 kernel configuration in `configs/base.config` and MSL-specific options in `configs/msl.config`. The MSL config enables,
  * USB storage
  * quotas
  * NFS
  * device mapper
  * NBD
  * KVM
  * 16 KiB pages

## Why the kernel uses 16 KiB pages

Heavy macOS swap pressure repeatedly corrupts the guest Linux kernel within minutes and kills the VM. The hypothesis is that, with 4 KiB guest pages on a 16 KiB host, each host page backs four 4 KiB stage-2 entries, and guest TLB invalidations go through a workaround. If a compression burst evicts a host page and not every stage-2 translation, or an emulated TLBI, is invalidated, a vCPU keeps a stale translation. It then writes to a physical page that now belongs to something else, and later sees the restored (old or zeroed) page. See [#48](https://github.com/onexay/msl/issues/48).

## Build locally

On macOS, install Apple's `container` CLI, then run:

```sh
./scripts/build.sh
```

The script builds Linux 6.18.15 in a Debian trixie arm64 container. Set `KVER` to build another Linux 6.18.x release. The build downloads the matching kernel.org source archive.

To build on Debian or Ubuntu arm64 without the container wrapper, install required packages,

```sh
sudo apt install -y build-essential flex bison bc libelf-dev libssl-dev curl xz-utils cpio kmod python3
```

then run:

```sh
./scripts/build-linux.sh . out [linux-version]
```

The default version is 6.18.15. Both build methods write these files to `out/`:

- `Image`: arm64 kernel image.
- `config`: final kernel configuration.
- `tag`: release tag for these inputs.

To try the kernel in an MSL checkout, set `MSL_KERNEL_OUT` to this repository's `out` directory and run `scripts/build.sh` there.

## Release a kernel

The tag is derived from the Linux version and both files in `configs/`. `scripts/tag.sh` prints it; changing an input creates a new tag without a manual version bump.

1. Push the kernel input changes. The **Kernel** GitHub Actions workflow builds the image on an arm64 runner and uploads an artifact named with the tag.
2. Run `./scripts/publish.sh` after the workflow completes. The script downloads the artifact, verifies the kernel.org source checksum, and creates a GitHub release with the image, configuration, and corresponding source.
3. In the MSL checkout, pin the release with `scripts/pin.sh kernel <tag>` and commit the resulting changes.

`scripts/publish.sh` requires GitHub CLI (`gh`) authenticated with permission to create releases in `onexay/msl-kernel`. Set `MSL_KERNEL_REPO` to publish to another repository.

## Contributing

Follow MSL's [contribution guide](https://github.com/onexay/msl/blob/main/CONTRIBUTING.md). Sign off each commit with `git commit -s` under the [Developer Certificate of Origin](https://developercertificate.org/).

## License

The build scripts and configuration are Apache-2.0; see [LICENSE](LICENSE). The Linux kernel is GPL-2.0. Each published release includes the corresponding source.
