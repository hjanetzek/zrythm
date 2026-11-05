<!---
SPDX-FileCopyrightText: © 2026 Hannes Janetzek <hannes.janetzek@gmail.com>
SPDX-License-Identifier: LicenseRef-ZrythmLicense
-->

# Zrythm on postmarketOS / Nura

Packages for building Nura (formerly postmarketOS) images that boot straight into Zrythm,
running in a [cage](https://github.com/cage-kiosk/cage) kiosk session on PipeWire. The setup
follows the Kick (`behringer-kick/Linux/nura`) and Oscairn (`oscairn/os/nura`) ports, which run
on the same device: real-time limits, waiting for PipeWire, touchscreen rotation, CPU governor,
USB boot.

| Target | Nura device | Status |
|---|---|---|
| Lenovo IdeaPad Duet (Chromebook) | `google-kukui` (mainline MediaTek kernel) | packages written, not built yet |

## Packages

| Package | What it does |
|---|---|
| `zrythm/` | Builds Zrythm from this checkout for musl: `/usr/bin/zrythm` and its plugin scanner; debug info (`-g1`) in `zrythm-dbg` |
| `postmarketos-ui-zrythm/` | Kiosk UI: `postmarketos-base-ui-cage` + `zrythm` + `pipewire-alsa`. `/etc/cage-ui/zrythm.sh` sets the output scale, waits for PipeWire and forces its quantum; `/usr/libexec/zrythm/zrythm-kiosk` starts Zrythm as a native Wayland client and restarts it if it exits; settings in `/etc/default/zrythm`; a `tinydm.service` drop-in raises the real-time and locked-memory limits |
| `zrythm-quirks-google-kukui/` | Lenovo Duet only: udev rule that rotates the touchscreen and pen, and the CPU governor and idle settings applied before each session; installed automatically (`install_if` the device package and the Zrythm UI) |
| `qt6/` | Qt 6.12.0, which Alpine edge does not ship yet (it has 6.11): the recipes of aports MR [!107047](https://gitlab.alpinelinux.org/alpine/aports/-/merge_requests/107047) for the modules Zrythm needs, plus `qt6-qtcanvaspainter` and `qt6-qttasktree` (new in 6.12, not in aports) |
| `deps/` | Libraries Zrythm needs that aports does not package: au, scnlib, magic_enum, type_safe (with debug_assert), gsl-lite, json-schema-validator, MPMCQueue, at the versions in `conan.lock` |
| `deploy.sh` | Builds packages (`zrythm` from this checkout) and installs them on a running device over SSH (`pmbootstrap sideload`), optionally restarting the session |
| `seed-distfiles.sh` | Puts the sources CPM would fetch (JUCE, CLAP, clap-helpers, the VST3 SDK with its submodules, farbot, loop-tempo-estimator with pffft) into pmbootstrap's distfiles cache, so `zrythm` builds offline |

Notes on `qt6/`:

- Each APKBUILD is the MR's, with changes marked `# zrythm:`: Release builds without LTO and
  without debug info (no `-dbg` subpackages), and `qt6-qttools` without qdoc and the clang-based
  `lupdate` parser (Zrythm only needs Linguist), so the build needs no clang/llvm. Under
  pmbootstrap, LTO code generation runs under qemu, and debug info and the llvm static libraries
  take several GB in the build chroot.
- The versions (6.12.0) are higher than aports' 6.11, so pmbootstrap builds these instead of
  using Alpine's binary packages. `qt6-qttools` and `qt6-qtwayland` also leave their own
  packages (named in `$depends_dev`) out of `makedepends`: abuild skips them, pmbootstrap would
  install them, or aports' older versions, before the build. Once aports has Qt 6.12, drop the copies of the MR's recipes;
  `qt6-qtcanvaspainter` and `qt6-qttasktree` stay until aports packages them.

Notes on `zrythm`:

- There is no source tarball: it is built from a local checkout with
  `pmbootstrap build --src=<checkout> zrythm`. `--src` skips `$source`, so `prepare()` takes the
  CPM sources from the distfiles cache (seeded by `seed-distfiles.sh`), assembles the VST3 SDK
  with its submodules and applies loop-tempo-estimator's patch; CMake uses them through
  FetchContent's `FETCHCONTENT_SOURCE_DIR_<NAME>`, which keeps the declarations in
  `package-lock.cmake` (`DOWNLOAD_ONLY`, `SOURCE_SUBDIR`). CPM's own `CPM_<name>_SOURCE` drops
  them, and the VST3 SDK's whole CMake project (VSTGUI, GTK samples) would be configured. The
  commits are in the APKBUILD as well as in `package-lock.cmake` (pffft's in
  loop-tempo-estimator's `source/cmake/pffft.cmake`); `prepare()` stops if they differ, and if
  `pkgver` does not match `VERSION.txt`.
- The other dependencies come from Alpine (fmt, spdlog, Boost 1.89, libsndfile, lilv, oneTBB, …)
  and from `qt6/` and `deps/`, not from Conan. Zrythm's CMake finds lilv, zstd and xxHash through
  pkg-config when there is no CMake package, and takes the LV2 spec bundles from `lv2.pc`.
- CMake options: `ZRYTHM_BUNDLE_RUNTIME=OFF` (no Qt, QML modules or libstdc++ copied next to
  Zrythm; they come from the system packages), `ZRYTHM_LTO=OFF` (no link-time optimization, which
  would run under qemu), `ZRYTHM_CHECK_UPDATES=OFF`, no manpage, shell completions or tests.
- Build type `RelWithDebInfo` with `-g1` (function names and lines for backtraces and `perf`),
  `-mcpu=cortex-a73` on aarch64, in `build-nura/` inside the `--src` copy (ignored by
  `.gitignore`, so rsync keeps it between `--lax` builds). Compile jobs: pmbootstrap's `jobs`
  setting, but at most one per 2 GB of available memory.
- `-D_LARGEFILE64_SOURCE`: musl 1.2.4+ hides `stat64` and friends without it.
- Audio goes through JUCE's ALSA backend to ALSA `default`, which `pipewire-alsa` routes to
  PipeWire; the build has no JACK backend.

## Host setup

As for Kick (see its README, "Host setup"): pmbootstrap 3.12.1 from git, static qemu-user
registered for aarch64 **without** the `P` flag, `multipath-tools`.

All commands below work when pasted into fish or bash, run from the zrythm checkout.

The pmbootstrap config shares the work directory of the Kick and Oscairn images
(`~/postmarketos/kick-pi4`), so chroots, the package cache and built packages are reused. Skip
this if `~/.config/pmbootstrap-zrythm-duet.cfg` exists:

```sh
printf '%s\n' '[pmbootstrap]' 'device = google-kukui' 'hostname = zrythm-duet' 'is_default_channel = False' 'locale = C.UTF-8' 'service_manager = systemd' 'ssh_key_glob = ~/.ssh/id_ed25519.pub' 'ssh_keys = True' 'timezone = Europe/Berlin' 'ui = cage' "user = $USER" "work = $HOME/postmarketos/kick-pi4" '' '[providers]' 'postmarketos-base-ui-audio-backend = postmarketos-base-ui-audio-backend-pipewire' '' '[mirrors]' > ~/.config/pmbootstrap-zrythm-duet.cfg
```

Link the packages into pmbootstrap's pmaports checkout and seed the distfiles cache (needs sudo:
the cache is owned by root):

```sh
ln -sfn "$PWD/tools/nura" ~/.local/var/pmbootstrap/cache_git/pmaports/zrythm
tools/nura/seed-distfiles.sh
```

(`~/.local/var/pmbootstrap/cache_git/pmaports` is pmbootstrap's default; check with
`pmbootstrap config aports`.) If the work directory is new, run
`pmbootstrap -c ~/.config/pmbootstrap-zrythm-duet.cfg init` first (channel **edge**, systemd,
audio backend **PipeWire**).

## Build and install

Build Qt and the libraries once, then Zrythm and the kiosk packages, and switch the UI to
Zrythm. Qt goes in three stages: the recipes ask for the other Qt modules without a version, and
aports' 6.11 packages satisfy that, so pmbootstrap would not build ours first. Once a stage's 6.12
packages are in the local repository, apk prefers them.

```sh
pmbootstrap -c ~/.config/pmbootstrap-zrythm-duet.cfg build --arch aarch64 qt6-qtbase qt6-qtshadertools qt6-qtsvg qt6-qtlanguageserver qt6-qttasktree
pmbootstrap -c ~/.config/pmbootstrap-zrythm-duet.cfg build --arch aarch64 qt6-qtdeclarative
pmbootstrap -c ~/.config/pmbootstrap-zrythm-duet.cfg build --arch aarch64 qt6-qttools qt6-qtwayland qt6-qtcanvaspainter
pmbootstrap -c ~/.config/pmbootstrap-zrythm-duet.cfg build --arch aarch64 au debug-assert gsl-lite magic-enum mpmcqueue nlohmann-json-schema-validator scnlib type-safe
pmbootstrap -c ~/.config/pmbootstrap-zrythm-duet.cfg build --arch aarch64 --src="$PWD" zrythm
pmbootstrap -c ~/.config/pmbootstrap-zrythm-duet.cfg build --arch aarch64 postmarketos-ui-zrythm zrythm-quirks-google-kukui
pmbootstrap -c ~/.config/pmbootstrap-zrythm-duet.cfg config ui zrythm
```

- With `--src`, pmbootstrap does not build missing dependencies, and `install` cannot build
  `zrythm` without a source: build Qt, the libraries and `zrythm` first.
- Build the noarch packages with `--arch aarch64`; otherwise pmbootstrap builds them for the host
  and pulls in an x86_64 `zrythm` without `--src`, which fails.
- `install_if` packages (the Duet quirks) are not built by `install`: build them by hand.

Write the image to a USB stick (8 GB or more; check the device with `lsblk` first):

```sh
pmbootstrap -c ~/.config/pmbootstrap-zrythm-duet.cfg install --sdcard=/dev/sdX
```

Booting the Duet from USB (developer mode, `crossystem dev_boot_usb=1`, Ctrl+U), Wi-Fi without
a screen keyboard and the pmbootstrap pitfalls between installs (`shutdown`, `wipefs`) are in
the Kick README; they apply unchanged.

The SSH key is installed: `ssh <user>@<hostname>.local`. Zrythm's output goes to
`~/.local/state/tinydm.log`, its own log to `~/.cache/Zrythm.org/Zrythm/`.

## Updating a running device

```sh
tools/nura/deploy.sh -r zrythm                   # build from this checkout, install, restart the session
tools/nura/deploy.sh zrythm zrythm-dbg           # also install the debug info
tools/nura/deploy.sh -n -H 192.168.178.93 postmarketos-ui-zrythm   # already built, other address
tools/nura/deploy.sh -k -n zrythm postmarketos-ui-zrythm           # a device with another work dir's image
```

`deploy.sh` reads the device's user and hostname from the pmbootstrap config (default
`~/.config/pmbootstrap-zrythm-duet.cfg`, host `<hostname>.local`), builds with `--lax` (keeps
the build chroot and the CMake build directory, so ninja rebuilds only what changed; `-S` for
a strict build) and installs with `pmbootstrap sideload`. `-k` installs this work dir's signing
key on a device whose image was built with another work dir. `-d` prints the commands.

The Qt packages and the libraries in `deps/` are installed with Zrythm the first time
(`deploy.sh -n qt6-qtbase …` or `apk add` on the device after `sideload`).

## The session

`postmarketos-ui-zrythm` runs, in the cage session started by tinydm as the default user:

1. `/etc/cage-ui/zrythm.sh` (sourced by `cage-ui-session.sh`): sets every output to scale 1.5
   (`ZRYTHM_OUTPUT_SCALE`), waits up to 10 s for PipeWire's default sink and forces PipeWire's
   quantum to 256 frames (`ZRYTHM_QUANTUM`).
2. `/usr/libexec/zrythm/zrythm-kiosk`: starts `zrythm` as a native Wayland client
   (`QT_QPA_PLATFORM=wayland`, no window decorations) with an unlimited locked-memory limit, and
   restarts it two seconds after it exits.

Only one kiosk UI package may be installed: the cage session sources every `/etc/cage-ui/*.sh`
in alphabetical order and the last `CAGE_UI_COMMAND` wins. On a device with the Kick or Oscairn
image, remove theirs (`sudo apk del postmarketos-ui-kick postmarketos-ui-oscairn`).

Settings, in `/etc/default/zrythm` (a config file, kept on upgrades); `sudo systemctl restart
tinydm` applies them:

| Variable | Default | Meaning |
|---|---|---|
| `ZRYTHM_OUTPUT_SCALE` | `1.5` | output scale for every output; empty leaves it |
| `ZRYTHM_QUANTUM` | `256` | PipeWire's quantum for the whole graph; empty lets PipeWire choose |
| `ZRYTHM_ARGS` | empty | more arguments for `zrythm` |
| `ZRYTHM_QSG_RHI_BACKEND` | unset (OpenGL) | Qt Quick's graphics API (`QSG_RHI_BACKEND`), e.g. `vulkan` |
| `ZRYTHM_GOVERNOR`, `ZRYTHM_IDLE_CPUS` | `schedutil`, `4-7` on the Duet | CPU governor and the CPUs kept out of deep idle (`zrythm-quirks-google-kukui`) |

## Lenovo Duet

The hardware (MediaTek MT8183: 4× Cortex-A53, 4× Cortex-A73; Mali-G72 with Panfrost; 1920×1200
DSI panel mounted rotated; one USB 2.0 USB-C port) and the reasons for the touchscreen rule,
governor and idle settings are described in the Oscairn README ("Lenovo Duet"); the quirks
package here applies the same settings.

Qt draws with OpenGL ES through EGL on Panfrost (aports builds Qt with `opengles2` on ARM).

## Known issues

- Not built or run on the device yet.
- The 256-frame quantum is a starting point; check `pw-top` for xruns.
- JUCE's ALSA backend does not ask for real-time scheduling for its audio thread; PipeWire's data
  loop is `SCHED_FIFO`. Whether the engine needs more is to be measured on the device.
