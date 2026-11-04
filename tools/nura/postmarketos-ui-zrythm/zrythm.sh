# Sourced by /usr/share/cage-ui/cage-ui-session.sh: run Zrythm full-screen in cage.
# The session runs with "set -eu -o pipefail" (from cage-ui-autorotate.sh): never fail here.
# Settings: /etc/default/zrythm.

ZRYTHM_OUTPUT_SCALE=1.5
ZRYTHM_QUANTUM=256
[ -f /etc/default/zrythm ] && . /etc/default/zrythm

# Zrythm is a native Wayland client: with a fractional output scale, Qt draws
# at the panel's resolution and the compositor does not scale its buffer.
zrythm_set_scale() {
	wlr-randr --json | jq -r '.[] | select(.enabled) | .name' |
	while read -r output; do
		wlr-randr --output "$output" --scale "$1"
	done
}
[ -n "$ZRYTHM_OUTPUT_SCALE" ] && { zrythm_set_scale "$ZRYTHM_OUTPUT_SCALE" || true; }

# The audio engine opens ALSA "default", i.e. PipeWire. During login PipeWire is
# only socket-activated and WirePlumber has not set up a sink yet, so that open
# would fail. Wait (up to 10 s) until there is a default sink.
zrythm_wait_for_audio() {
	i=0
	while [ "$i" -lt 100 ]; do
		wpctl inspect @DEFAULT_AUDIO_SINK@ >/dev/null 2>&1 && return 0
		sleep 0.1
		i=$((i + 1))
	done
	return 1
}
zrythm_wait_for_audio || true

# A fixed quantum for the whole PipeWire graph (fine for a kiosk), so the ALSA
# device's period and PipeWire's match and the engine's callbacks come evenly.
if [ -n "$ZRYTHM_QUANTUM" ]; then
	pw-metadata -n settings 0 clock.force-quantum "$ZRYTHM_QUANTUM" >/dev/null 2>&1 || true
	export PIPEWIRE_LATENCY="$ZRYTHM_QUANTUM/48000"
fi

CAGE_UI_COMMAND=/usr/libexec/zrythm/zrythm-kiosk
