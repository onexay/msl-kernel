#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Print the kernel release tag for the current inputs:
#   kernel-<linux version>-msl-<first 7 of sha256(linux version, base.config, msl.fragment, drivers/*)>
# Same inputs, same tag; any config change gives a new one (no manual bump).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
VER=${KVER:-$(sed -n 's/^VER=\${KVER:-\(.*\)}$/\1/p' "$HERE/build.sh")}
H=$( { echo "linux-$VER"; cat "$HERE/base.config" "$HERE/msl.fragment" "$HERE"/drivers/*; } | shasum -a 256 | cut -c1-7)
echo "kernel-$VER-msl-$H"
