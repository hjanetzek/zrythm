#!/bin/sh
# SPDX-FileCopyrightText: © 2026 Hannes Janetzek <hannes.janetzek@gmail.com>
# SPDX-License-Identifier: LicenseRef-ZrythmLicense
#
# Put the remote sources of the zrythm package (the sources CPM would fetch) into
# pmbootstrap's distfiles cache, so that "pmbootstrap build --src=... zrythm"
# (which skips $source) builds without network.
# Usage: seed-distfiles.sh [pmbootstrap config]   (needs sudo: the cache is owned by root)
set -eu

cfg=${1:-$HOME/.config/pmbootstrap-zrythm-duet.cfg}
work=$(sed -n 's/^work *= *//p' "$cfg")
[ -n "$work" ] || { echo "no work dir in $cfg" >&2; exit 1; }
dest="$work/cache_distfiles"
here=$(cd "$(dirname "$0")" && pwd)
apkbuild="$here/zrythm/APKBUILD"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# The APKBUILD's "_name=value" assignments, to expand the "name::url" entries.
grep -E '^_[a-z0-9]+=[A-Za-z0-9._-]+$' "$apkbuild" > "$tmp/vars"
. "$tmp/vars"

sed -n '/^source="/,/^\t"/p' "$apkbuild" | grep '::' | sed 's/^\t//' |
	while IFS= read -r entry; do
		entry=$(eval "echo \"$entry\"")
		name=${entry%%::*}
		url=${entry#*::}
		if [ -e "$dest/$name" ]; then
			echo "have $name"
			continue
		fi
		echo "fetch $name"
		curl -fsSL -o "$tmp/$name" "$url"
	done

sed -n '/^sha512sums="/,/^"/p' "$apkbuild" | grep '  ' > "$tmp/sha512sums"
for f in "$tmp"/*.tar.gz; do
	[ -e "$f" ] || continue
	name=$(basename "$f")
	(cd "$tmp" && grep "  $name\$" sha512sums | sha512sum -c -)
	sudo install -m 664 -g 300 "$f" "$dest/$name"
done
ls -l "$dest" | grep -E 'JUCE-|clap|farbot|loop-tempo|vst3' || true
