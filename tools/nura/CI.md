<!---
SPDX-FileCopyrightText: © 2026 Hannes Janetzek <hannes.janetzek@gmail.com>
SPDX-License-Identifier: LicenseRef-ZrythmLicense
-->

# Building Zrythm in nura-builds CI

For the nura-builds CI (`hjanetzek/nura-builds`, branch `ci-restructure`: `targets/zrythm.sh`,
`.github/workflows/zrythm.yml`, the `qt-*` package sets). It lists what the recipes in this
directory need from the CI, what they change in the shared package repository, and the problems
already solved in local pmbootstrap builds. How the packages work: [README.md](README.md).

## Source

| | |
|---|---|
| Repository | `https://github.com/hjanetzek/zrythm` (public fork of `zrythm/zrythm`) |
| Branch | `nura-duet` |
| Recipe directories | `tools/nura/deps`, `tools/nura/zrythm`, `tools/nura/postmarketos-ui-zrythm`, `tools/nura/zrythm-quirks-google-kukui` |
| Not linked | `tools/nura/qt6`: identical copies of nura-builds' `aports/qt6` (all nine modules, compared on 2026-11-16) |

- `nura-duet` is rebased onto Zrythm's `develop` regularly and force-pushed: never assume a
  fast-forward between runs; check out the branch tip fresh.
- `nura-duet` contains the clip launcher branch (`clip-launcher-fixes`) and the Nura commits on
  top of it.
- No git-lfs and no submodules are needed for the package build.
- A change to a Qt recipe here is made in nura-builds' `aports/qt6` as well (and the reverse):
  two recipes of one name in pmaports clash, so only one copy may be linked.

## Build order

Everything the `zrythm` package build needs must already be in the local package repository,
because `--src` builds do not build missing dependencies:

1. Qt 6.12 from the package sets, in this order: `qt-base` (qtbase, qtshadertools, qtsvg,
   qtlanguageserver), `qt-declarative`, `qt-wayland`, `qt-tools` (qttasktree, qttools,
   qtcanvaspainter). The recipes ask for the other Qt modules without a version, which Alpine's
   6.11 packages satisfy, so each set's 6.12 packages must be in the repository before the next set
   is built.
2. The `deps` stage (`tools/nura/deps`), with two ordering constraints:
   - `debug-assert` before `type-safe` (build dependency).
   - `nlohmann-json` before `nlohmann-json-schema-validator`, which is built against it.
3. `zrythm` with `--src` (after `seed-distfiles.sh`), then `postmarketos-ui-zrythm` and
   `zrythm-quirks-google-kukui` (both noarch).

## Effects on the shared package repository

- **nlohmann-json** (`deps/nlohmann-json`, `3.12.0_git20250510`) replaces Alpine's 3.12.0 for
  every package built against the repository, because apk prefers the higher version. It is the
  commit Zrythm pins (conan.lock calls it "3.12.1"); Alpine's 3.12.0 never compiles its
  `from_json` for `std::optional`. Same ABI namespace (`json_abi_v3_12_0`), header-only. Other
  users: `zynthian-ui` and `zynthian-ui-{vangelis,tomita}` (makedepends, headers only).
- **type-safe** is `arch="all"`, not noarch: pmbootstrap builds noarch packages in its native
  chroot, where a `debug-assert` built for the target architecture is not available.
- **scnlib** compiles in fast_float 6.1.6 (a second source of its recipe): it does not build
  against Alpine's fast_float 8.
- **postmarketos-ui-zrythm** makes its labwc session tinydm's default on install *and* upgrade
  (`tinydm-set-session -f`). Installing it on a zynthian device switches that device to Zrythm.
  It belongs in Zrythm images only, never in a zynthian image or a meta package that zynthian
  images pull in. `zrythm-quirks-google-kukui` is `install_if` the Duet's device package plus this
  UI package, so it follows the UI package.

## The `--src` build of zrythm

- **Distfiles**: `seed-distfiles.sh [config]` downloads the sources CPM would fetch (JUCE, CLAP,
  clap-helpers, the VST3 SDK and its four submodules, farbot, loop-tempo-estimator, pffft from
  Bitbucket) into `<work>/cache_distfiles`, checks them against the APKBUILD's sha512sums, and
  installs them with `sudo install -g 300` (the cache is owned by root). It needs network access.
  The commits are variables in the APKBUILD (`_jucecommit`, ...), so the script needs no changes
  when they change.
- **Where prepare() looks**: abuild's `SRCDEST`, then `/var/cache/distfiles` (pmbootstrap's cache
  mount), then `$srcdir`. abuild only uses `/var/cache/distfiles` as `SRCDEST` when
  `~/.cache/abuild` does not exist and the cache is writable, hence the explicit second path.
- **Checks in prepare()** that fail the build on purpose:
  - Every CPM commit in the APKBUILD must be a `GIT_TAG` in `package-lock.cmake`, and pffft's must
    match loop-tempo-estimator's `source/cmake/pffft.cmake`. After a rebase that bumps a
    dependency, the APKBUILD's commits and checksums need updating (and the distfiles seeding).
  - `pkgver` must match `VERSION.txt`. pmbootstrap's `--src` replaces pkgver with
    `<version>_p<timestamp>` and drops the pre-release suffix (`2.0.0_alpha7` →
    `2.0.0_p20261105...`); then only the version numbers are compared. A new Zrythm version needs
    a pkgver update in the APKBUILD.
- **CPM sources** are passed as `FETCHCONTENT_SOURCE_DIR_<NAME>`, not `CPM_<name>_SOURCE`: the
  latter drops the declarations in `package-lock.cmake` (`DOWNLOAD_ONLY`, `SOURCE_SUBDIR`), and the
  VST3 SDK's whole CMake project (VSTGUI, GTK samples) would be configured. FetchContent does not
  patch an overridden source; prepare() applies loop-tempo-estimator's patch.
  `FETCHCONTENT_FULLY_DISCONNECTED=ON`: the build never downloads.
- **CMake options** (APKBUILD `build()`): `ZRYTHM_BUNDLE_RUNTIME=OFF` (no Qt, QML modules or
  libstdc++ copied into the package), `ZRYTHM_LTO=OFF`, `ZRYTHM_WITH_LIBDW=ON` (musl has no
  `execinfo.h`), `ZRYTHM_CHECK_UPDATES=OFF`, no tests, manpage or shell completions.
  `Release` at `-O2`: no debug info (no `-dbg` subpackage) and no tests. abuild strips the
  binaries, so backtraces on the device carry no function names.
- **LTO** is off because under pmbootstrap on x86_64 hosts its code generation runs under qemu. On
  native arm64 runners it would be possible, but it multiplies link time and memory; leave it off
  unless release builds are measured with it.
- **Compile jobs**: pmbootstrap's `JOBS`, capped at one job per 2 GB of available memory (some
  translation units need over 1.5 GB). On a 16 GB runner that is about 7 jobs.
- **Size and time**: about 1300 ninja steps. On a 48-thread x86_64 host with crossdirect, 896
  steps took about 11 minutes with 25 jobs. On a 4-core runner expect 1–2 hours (estimate, not
  measured); ccache helps the weekly runs only if its cache is kept between runs.
- `-mcpu=cortex-a73` is set on aarch64 only (the Duet's big cores; the code still runs on any
  ARMv8.0 CPU). x86_64 builds use Alpine's default flags.

## Runtime

- `zrythm` depends on the Qt 6.12 packages by name (`qt6-qtbase-wayland`, `qt6-qtdeclarative`,
  `qt6-qtsvg`, `qt6-qtwayland`, `qt6-qtcanvaspainter`, `qt6-qttasktree`), because Qt loads their
  plugins at run time. A device without the package repository would get Alpine's 6.11 for these
  and fail.
- `postmarketos-ui-zrythm` depends on Alpine's `labwc`, `pipewire-alsa`, `wlr-randr`, `jq`,
  `drm_info` and the postmarketOS base UI packages; nothing of it is built in CI besides the
  package itself.
- Settings on the device: `/etc/default/zrythm` (README, "The session"). `ZRYTHM_PROJECT` is empty
  in the package: the kiosk starts with Zrythm's project selector.

## Images

- UI `zrythm`, devices `google-kukui` (aarch64) and `qemu-amd64` (x86_64), as in
  `targets/zrythm.sh`.
- Zrythm's project format still changes between develop versions without migration (e.g.
  `channelSends` became required in November 2026). Do not put demo projects into images until
  the format is stable; projects saved with an older build fail to load in a newer one.

## Checking a build without a device

`tools/nura/preview-kiosk.sh` runs a desktop build of the same checkout in a headless labwc with
the kiosk's configuration (1920×1200, scale 2) and takes a screenshot with grim. It needs a desktop
build (`conanbuild/Debug`) and `labwc`, `grim`, `wlr-randr`; it is meant for local checks, not
for the CI runners.

## Open points

- `tools/nura/qt6` carries APKBUILDs and patches from aports MR !107047 without a licence
  statement (aports has none); Zrythm's `reuse lint` fails on those 11 files. Once Alpine ships
  Qt 6.12, both copies of the recipes go away (except qtcanvaspainter and qttasktree until aports
  packages them).
- Native plugin GUIs without a parent window: a resizable one is made fullscreen and kept below
  the main window by the kiosk's window rule (README, "Known issues").
