#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Print the next release version from the repository version and commits since
# the latest reachable SemVer tag. Conventional Commits select major/minor bumps.
set -eu
BASE_VERSION=${1:?usage: next-version.sh MAJOR.MINOR.PATCH}

is_semver() {
  printf '%s\n' "$1" | grep -Eq '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'
}
if ! is_semver "$BASE_VERSION"; then
  echo "base version must be MAJOR.MINOR.PATCH: $BASE_VERSION" >&2
  exit 2
fi

LATEST_TAG=$(git tag --merged HEAD --list 'v[0-9]*' --sort=-version:refname |
  sed -nE '/^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(\+[[:xdigit:]]{7,40})?$/ {p;q;}')

if [ -z "$LATEST_TAG" ]; then
  echo "$BASE_VERSION"
  exit 0
fi

LATEST=${LATEST_TAG#v}
LATEST=${LATEST%%+*}
COMMITS=$(git log "$LATEST_TAG..HEAD" --format=%B)
if [ -z "$COMMITS" ] && [ "$BASE_VERSION" = "$LATEST" ]; then
  echo "no commits since $LATEST_TAG; nothing to release" >&2
  exit 2
fi

MAJOR=${LATEST%%.*}
REST=${LATEST#*.}
MINOR=${REST%%.*}
PATCH=${REST#*.}

if printf '%s\n' "$COMMITS" | grep -Eq '^[[:alnum:]-]+(\([^)]*\))?!:|^[[:space:]]*BREAKING[- ]CHANGE:'; then
  NEXT="$((MAJOR + 1)).0.0"
elif printf '%s\n' "$COMMITS" | grep -Eq '^feat(\([^)]*\))?:'; then
  NEXT="$MAJOR.$((MINOR + 1)).0"
else
  NEXT="$MAJOR.$MINOR.$((PATCH + 1))"
fi

# Respect a deliberate major/minor bump in the repository's own version.
VERSION=$(printf '%s\n%s\n' "$BASE_VERSION" "$NEXT" | sort -V | tail -n 1)
echo "$VERSION"
