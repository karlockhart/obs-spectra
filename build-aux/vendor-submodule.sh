#!/usr/bin/env bash
# Convert a git submodule into a vendored git subtree at the same path,
# pinned to the commit the superproject currently references.
#
# Usage: build-aux/vendor-submodule.sh <submodule-path>
#   e.g. build-aux/vendor-submodule.sh plugins/obs-websocket
#
# Later, to pull upstream changes into the vendored copy:
#   git subtree pull --prefix=<path> <upstream-url> <ref> --squash

set -euo pipefail

path="${1:?usage: $0 <submodule-path>}"
path="${path%/}"

cd "$(git rev-parse --show-toplevel)"

if [[ -n "$(git status --porcelain)" ]]; then
  echo "error: working tree is not clean; commit or stash first" >&2
  exit 1
fi

name=$(git config -f .gitmodules --get-regexp '^submodule\..*\.path$' | awk -v p="$path" '$2 == p { print $1 }' | sed 's/^submodule\.//; s/\.path$//')
if [[ -z "$name" ]]; then
  echo "error: '$path' is not a submodule" >&2
  exit 1
fi

url=$(git config -f .gitmodules --get "submodule.$name.url")
commit=$(git ls-tree HEAD "$path" | awk '{ print $3 }')

echo "Vendoring $path from $url @ $commit"

# Drop the submodule
git submodule deinit -f -- "$path"
git rm -f -- "$path"
rm -rf ".git/modules/$name"
git commit -m "Remove submodule $path in preparation for vendoring"

# Re-add the same commit as a squashed subtree
git fetch --no-tags "$url" "$commit"
git subtree add --prefix="$path" "$commit" --squash \
  -m "Vendor $path from $url @ ${commit:0:9}"

echo "Done. Upstream: $url"
echo "Update later with: git subtree pull --prefix=$path $url <ref> --squash"
