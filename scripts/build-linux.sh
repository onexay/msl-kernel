#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Build the MSL kernel on Debian/Ubuntu arm64 (inside Apple `container` via
# scripts/build.sh, or natively on CI's ubuntu-24.04-arm runner).
# Needs: build-essential flex bison bc libelf-dev libssl-dev curl xz-utils cpio kmod python3.
#   scripts/build-linux.sh <kernel dir> <out dir> [linux version]
# Output: <out>/{Image,config}
set -euxo pipefail
K=$(cd "$1" && pwd); OUT=$2; VER=${3:-6.18.15}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
W=$(mktemp -d)
cd "$W" && curl -sSfL "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$VER.tar.xz" | tar xJ
cd "linux-$VER"
cp "$K/configs/base.config" .config
scripts/kconfig/merge_config.sh -m .config "$K/configs/msl.config"
make olddefconfig
for o in USB_XHCI_PCI USB_STORAGE QUOTA NFSD BLK_DEV_DM BLK_DEV_NBD ARM64_16K_PAGES VIRTUALIZATION KVM; do grep -q "^CONFIG_$o=y" .config || { echo "missing $o"; exit 1; }; done
# uname -r includes the short Git commit hash, e.g. 6.18.15-msl-a1a22bd.
COMMIT=$(git -C "$K" rev-parse --short=7 HEAD)
LV="-msl-$COMMIT"
make -j"$(nproc)" LOCALVERSION="$LV" Image
REL=$(make -s LOCALVERSION="$LV" kernelrelease)
[ "$REL" = "$VER$LV" ] || { echo "kernelrelease $REL doesn't match $VER$LV"; exit 1; }
cp arch/arm64/boot/Image "$OUT/Image"
cp .config "$OUT/config"
rm -rf "$W"
