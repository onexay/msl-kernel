# MSL Linux Kernel

[![Kernel](https://github.com/onexay/msl-kernel/actions/workflows/kernel.yml/badge.svg)](https://github.com/onexay/msl-kernel/actions/workflows/kernel.yml)

This repository builds an arm64 Linux kernel for MSL and publishes versioned kernel releases.

MSL uses an unmodified Linux source release with Apple's 6.18 kernel configuration in `configs/base.config` and MSL-specific options in `configs/msl.config`. The MSL config enables USB storage, quotas, NFS, device mapper, NBD, KVM and 16 KiB pages.

## Why the kernel uses 16 KiB pages

Heavy macOS swap pressure repeatedly corrupts the guest Linux kernel within minutes and kills the VM. The hypothesis is that, with 4 KiB guest pages on a 16 KiB host, each host page backs four 4 KiB stage-2 entries, and guest TLB invalidations go through a workaround. If a compression burst evicts a host page and not every stage-2 translation, or an emulated TLBI, is invalidated, a vCPU keeps a stale translation. It then writes to a physical page that now belongs to something else, and later sees the restored (old or zeroed) page. See [#48](https://github.com/onexay/msl/issues/48).

## Build locally

On macOS, install Apple's `container` CLI, then run:

```sh
./scripts/build.sh
```

The script builds Linux 6.18.15 in a Debian trixie arm64 container. Set `KVER` to build another Linux 6.18.x release. The build downloads the matching kernel.org source archive.

To build on Debian or Ubuntu arm64 without the container wrapper, install the required packages:

```sh
sudo apt install -y build-essential flex bison bc libelf-dev libssl-dev curl xz-utils cpio kmod python3
```

Then run:

```sh
./scripts/build-linux.sh . out [linux-version]
```

Both build methods write `Image`, `config`, `kernel.version` (the `uname -r` value) and `build-info.txt` (the full source commit) to `out/`. To try a local kernel in an MSL checkout, set `MSL_KERNEL_OUT` to this repository's `out` directory and run `scripts/build.sh` there.

## Release a kernel

Push the kernel changes to main, then choose **Actions → Kernel → Run workflow**. The first SemVer release uses the Linux version in `scripts/build.sh`; later releases bump from the latest SemVer tag based on commits since that release. Use Conventional Commit subjects: `feat:` bumps minor, `BREAKING CHANGE:` or a type with `!` (such as `feat!:`) bumps major, and other commits bump patch. If the configured Linux version is higher than the calculated version, the workflow uses it. The release tag adds the seven-character source commit hash as SemVer build metadata, such as `v6.18.15+abc1234`. The workflow verifies the matching Linux source checksum and publishes the image, configuration, kernel release string, source commit, checksums and source archive.

In the MSL checkout, run `scripts/pin.sh kernel <release-tag>` and commit the updated pin files.

## Contributing

Follow MSL's [contribution guide](https://github.com/onexay/msl/blob/main/CONTRIBUTING.md). Sign off each commit with `git commit -s` under the [Developer Certificate of Origin](https://developercertificate.org/).

## License

The build scripts and configuration are Apache-2.0; see [LICENSE](LICENSE). The Linux kernel is GPL-2.0. Each published release includes the corresponding source.
