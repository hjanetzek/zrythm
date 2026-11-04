#!/bin/sh
# SPDX-FileCopyrightText: © 2026 Hannes Janetzek <hannes.janetzek@gmail.com>
# SPDX-License-Identifier: LicenseRef-ZrythmLicense
#
# Build Nura packages and install them on a running device over SSH.
#
# Usage: deploy.sh [-c CONFIG] [-H HOST] [-u USER] [-a ARCH] [-k] [-n] [-S] [-r] [-d] PACKAGE...
#   -c CONFIG  pmbootstrap config (default: ~/.config/pmbootstrap-zrythm-duet.cfg)
#   -H HOST    device address (default: <hostname from the config>.local, via avahi)
#   -u USER    SSH user on the device (default: user from the config)
#   -a ARCH    package architecture (default: aarch64)
#   -k         install this work dir's package signing key on the device if needed (a device
#              whose image was built with another work dir, e.g. another kiosk image, rejects the
#              packages: "UNTRUSTED signature")
#   -n         do not build; install the newest packages already built
#   -S         strict build: let pmbootstrap zap the build chroots first (its default). Without
#              it builds run with --lax, which keeps the chroot, the --src copy and its CMake
#              build directory, so ninja only rebuilds what changed.
#   -r         restart the kiosk session afterwards (systemctl restart tinydm)
#   -d         dry run: print the commands instead of running them
#
# Examples:
#   deploy.sh -r zrythm                  # build zrythm from this checkout, install, restart
#   deploy.sh zrythm zrythm-dbg          # also install the debug info
#   deploy.sh -H 192.168.178.93 postmarketos-ui-zrythm
#
# zrythm is built from this checkout (pmbootstrap build --src); other packages are built from
# their APKBUILDs, zrythm first (with --src, pmbootstrap does not build missing dependencies).
# A subpackage (zrythm-dbg) is built with its main package. Installing uses
# "pmbootstrap sideload": it copies the newest version of each package and runs "sudo apk add"
# on the device, so sudo asks for passwords on both machines.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
checkout=$(cd "$here/../.." && pwd)
cfg=$HOME/.config/pmbootstrap-zrythm-duet.cfg
host=
user=
arch=aarch64
installkey=
build=1
lax=--lax
restart=0
dry=0

usage() { sed -n '5,/^set -eu/{/^set -eu/d;s/^# \{0,1\}//;p}' "$0"; exit "${1:-0}"; }

while getopts c:H:u:a:knSrdh opt; do
	case $opt in
		c) cfg=$OPTARG ;;
		H) host=$OPTARG ;;
		u) user=$OPTARG ;;
		a) arch=$OPTARG ;;
		k) installkey=--install-key ;;
		n) build=0 ;;
		S) lax= ;;
		r) restart=1 ;;
		d) dry=1 ;;
		h) usage ;;
		*) usage 1 ;;
	esac
done
shift $((OPTIND - 1))
[ $# -gt 0 ] || usage 1
[ -f "$cfg" ] || { echo "no pmbootstrap config: $cfg" >&2; exit 1; }

cfgval() { sed -n "s/^$1 *= *//p" "$cfg" | head -n 1; }
[ -n "$user" ] || user=$(cfgval user)
[ -n "$host" ] || host=$(cfgval hostname).local

run() {
	echo "+ $*"
	[ "$dry" = 1 ] || "$@"
}
pmb() { run pmbootstrap -c "$cfg" "$@"; }

if [ "$build" = 1 ]; then
	# Main packages to build: drop subpackage suffixes, no duplicates, zrythm first.
	mains=
	for pkg in "$@"; do
		main=${pkg%-dbg}
		case " $mains " in *" $main "*) ;; *) mains="$mains $main" ;; esac
	done
	others=
	for main in $mains; do
		if [ "$main" = zrythm ]; then
			# shellcheck disable=SC2086 # empty $lax is intended
			pmb build $lax --arch "$arch" --src="$checkout" zrythm
		else
			others="$others $main"
		fi
	done
	# Also noarch packages for the device's architecture: otherwise pmbootstrap builds them for
	# the host and pulls in a host build of zrythm without --src.
	# shellcheck disable=SC2086 # word splitting intended
	[ -z "$others" ] || pmb build $lax --arch "$arch" $others
fi

# shellcheck disable=SC2086 # empty $installkey is intended
pmb sideload $installkey --arch "$arch" --host "$host" --user "$user" "$@"

if [ "$restart" = 1 ]; then
	run ssh -t "$user@$host" 'sudo systemctl restart tinydm'
fi
