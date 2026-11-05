#!/bin/sh
# SPDX-FileCopyrightText: © 2026 Hannes Janetzek <hannes.janetzek@gmail.com>
# SPDX-License-Identifier: LicenseRef-ZrythmLicense
#
# Run a desktop build of Zrythm the way the kiosk on the Lenovo Duet shows it,
# without a device: a headless labwc with the kiosk's configuration
# (postmarketos-ui-zrythm: window rules, theme) and one 1920x1200 output at
# scale 2. Nothing appears on the desktop; screenshots are taken with grim.
#
# Usage: preview-kiosk.sh [-b BUILD_DIR] [-s SCALE] [-m MODE] [-w SECONDS] [-o FILE] [-k] [-- ZRYTHM_ARG...]
#   -b BUILD_DIR  build directory (default: conanbuild/Debug in this checkout)
#   -s SCALE      output scale (default: 2)
#   -m MODE       output mode in pixels (default: 1920x1200)
#   -w SECONDS    wait before the screenshot (default: 20)
#   -o FILE       screenshot file (default: kiosk-preview.png in the current directory)
#   -k            keep the session running after the screenshot; prints the
#                 WAYLAND_DISPLAY to use with grim, wlrctl or lswt, and stops on Ctrl+C
#   ZRYTHM_ARG    arguments for zrythm, e.g. a project directory to open
#
# Needs labwc, grim and wlr-randr (and wlrctl/lswt for interaction).
set -eu

here=$(cd "$(dirname "$0")" && pwd)
checkout=$(cd "$here/../.." && pwd)
build=$checkout/conanbuild/Debug
scale=2
mode=1920x1200
wait=20
out=$PWD/kiosk-preview.png
keep=0

usage() { sed -n '5,/^set -eu/{/^set -eu/d;s/^# \{0,1\}//;p}' "$0"; exit "${1:-0}"; }

while getopts b:s:m:w:o:kh opt; do
	case $opt in
		b) build=$(cd "$OPTARG" && pwd) ;;
		s) scale=$OPTARG ;;
		m) mode=$OPTARG ;;
		w) wait=$OPTARG ;;
		o) out=$OPTARG ;;
		k) keep=1 ;;
		h) usage ;;
		*) usage 1 ;;
	esac
done
shift $((OPTIND - 1))

for tool in labwc grim wlr-randr; do
	command -v "$tool" >/dev/null || { echo "$tool not found" >&2; exit 1; }
done
[ -x "$build/products/bin/zrythm" ] || { echo "no zrythm in $build/products/bin" >&2; exit 1; }

tmp=$(mktemp -d)
labwc_pid=
cleanup() {
	[ -z "$labwc_pid" ] || kill "$labwc_pid" 2>/dev/null || true
	rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

# The kiosk's labwc configuration; the autostart only reports the socket.
ui=$here/postmarketos-ui-zrythm
cp "$ui/rc.xml" "$ui/themerc-override" "$ui/menu.xml" "$tmp/"
printf 'echo "$WAYLAND_DISPLAY" > %s/display\n' "$tmp" > "$tmp/autostart"

WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1 \
	labwc -C "$tmp" > "$tmp/labwc.log" 2>&1 &
labwc_pid=$!

i=0
while [ ! -s "$tmp/display" ]; do
	i=$((i + 1))
	[ "$i" -lt 100 ] || { cat "$tmp/labwc.log" >&2; echo "labwc did not start" >&2; exit 1; }
	sleep 0.1
done
WAYLAND_DISPLAY=$(cat "$tmp/display")
export WAYLAND_DISPLAY

output=$(wlr-randr | sed -n '1s/ .*//p')
wlr-randr --output "$output" --custom-mode "$mode" --scale "$scale"

# As zrythm-kiosk starts it on the device.
(
	. "$build/generators/conanrun.sh" 2>/dev/null || true
	QT_QPA_PLATFORM=wayland QT_WAYLAND_DISABLE_WINDOWDECORATION=1 \
		exec "$build/products/bin/zrythm" "$@"
) > "$tmp/zrythm.log" 2>&1 &

sleep "$wait"
grim "$out"
echo "screenshot: $out"

if [ "$keep" = 1 ]; then
	echo "WAYLAND_DISPLAY=$WAYLAND_DISPLAY (Ctrl+C to stop)"
	wait "$labwc_pid"
fi
