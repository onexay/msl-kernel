#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Publish the kernel CI built for the current commit (.github/workflows/kernel.yml,
# artifact named kernel-<linux>-msl-<commit hash>) as that
# GitHub release, with release.sha256 and the kernel.org source (GPL-2.0).
# Downloads it into out/. msl then pins it: scripts/pin.sh kernel <tag> in msl.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
VER=${KVER:-$(sed -n 's/^VER=${KVER:-\(.*\)}$/\1/p' "$HERE/scripts/build.sh")}
COMMIT=$(git -C "$HERE" rev-parse HEAD | cut -c1-7)
TAG="kernel-$VER-msl-$COMMIT"
REPO=${MSL_KERNEL_REPO:-onexay/msl-kernel}
if gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
  echo "$TAG is already published" >&2; exit 1
fi
RUN=$(gh api "repos/$REPO/actions/artifacts?name=$TAG&per_page=20" --jq '[.artifacts[] | select(.expired | not)][0].workflow_run.id // empty')
[ -n "$RUN" ] || { echo "CI hasn't built $TAG yet: push this commit and wait for the Kernel workflow" >&2; exit 1; }
rm -rf "$HERE/out" && gh run download "$RUN" --repo "$REPO" --name "$TAG" --dir "$HERE/out"
echo "kernel from CI run $RUN ($(cat "$HERE/out/build-info.txt"))"
LINUX=$(sed -n 's/^# Linux\/arm64 \([^ ]*\) Kernel Configuration/\1/p' "$HERE/out/config")
case $TAG in "kernel-$LINUX-msl-"*) ;; *) echo "$TAG doesn't match the built kernel ($LINUX)" >&2; exit 1 ;; esac
[ "$(cat "$HERE/out/build-info.txt" 2>/dev/null)" = "commit $(git -C "$HERE" rev-parse HEAD)" ] || { echo "the CI artifact wasn't built from this commit" >&2; exit 1; }
(cd "$HERE/out" && shasum -a 256 Image config) > "$HERE/out/release.sha256"

# GPL-2.0: ship the corresponding source, verified against kernel.org's checksums.
SRC=$HERE/dist/linux-$LINUX.tar.xz
URL=https://cdn.kernel.org/pub/linux/kernel/v${LINUX%%.*}.x
mkdir -p "$HERE/dist"
[ -s "$SRC" ] || curl -fsSL -o "$SRC" "$URL/linux-$LINUX.tar.xz"
curl -fsSL -o "$HERE/dist/sha256sums.asc" "$URL/sha256sums.asc"
want=$(awk -v f="linux-$LINUX.tar.xz" '$2==f{print $1}' "$HERE/dist/sha256sums.asc")
[ "$(shasum -a 256 "$SRC" | cut -d' ' -f1)" = "$want" ] || { echo "linux-$LINUX.tar.xz: checksum mismatch" >&2; exit 1; }

gh release create "$TAG" "$HERE/out/Image" "$HERE/out/config" "$HERE/out/release.sha256" "$SRC" --repo "$REPO" \
  --title "MSL kernel $LINUX (${TAG##*-msl-})" \
  --notes "Linux $LINUX (arm64), built by CI run $RUN (scripts/build-linux.sh on ubuntu-24.04-arm) from configs/base.config + configs/msl.config, $(cat "$HERE/out/build-info.txt"). msl bundles the release it pins (scripts/pin.sh). GPL-2.0. Corresponding source: linux-$LINUX.tar.xz (attached, unmodified from kernel.org) and the attached config; scripts/build-linux.sh reproduces the build."
echo "published $TAG; pin it in msl: scripts/pin.sh kernel $TAG"
