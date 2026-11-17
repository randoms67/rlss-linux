#!/usr/bin/env bash
# Builds dist/RLSS-Linux-<arch>.AppImage using an Ubuntu 22.04 container
# (podman or docker), so the result runs on old and new distributions alike.
#
#   scripts/build-appimage.sh            # x86_64 AppImage (PCs, game runs through the ARM translation layer)
#   scripts/build-appimage.sh aarch64    # ARM64 AppImage (game runs natively), cross compiled
#   scripts/build-appimage.sh all
#
# Set NO_CONTAINER=1 to run the build directly on an Ubuntu/Debian host
# (the script installs packages with apt, so run that as root, e.g. in CI).
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(cd "$HERE/.." && pwd)"
if [ ! -d "$WORK/mcpelauncher-manifest/mcpelauncher-linker" ]; then
    echo "Expected a recursive clone of mcpelauncher-manifest next to this project:" >&2
    echo "  git clone --recursive https://github.com/minecraft-linux/mcpelauncher-manifest $WORK/mcpelauncher-manifest" >&2
    exit 1
fi

# The game: newest *.apk in apk/ (or APK_LAUNCHER_BUNDLE_APK, relative to the
# project). Without one, a launcher-only AppImage is built that asks for the
# APK on first start (that is what GitHub releases contain).
if [ -z "${APK_LAUNCHER_BUNDLE_APK:-}" ]; then
    apk="$(ls -t "$HERE"/apk/*.apk "$HERE"/apk/*.APK 2>/dev/null | head -1 || true)"
    [ -n "$apk" ] && APK_LAUNCHER_BUNDLE_APK="apk/$(basename "$apk")"
fi
if [ -n "${APK_LAUNCHER_BUNDLE_APK:-}" ]; then
    if [ ! -f "$HERE/$APK_LAUNCHER_BUNDLE_APK" ]; then
        echo "APK not found: $HERE/$APK_LAUNCHER_BUNDLE_APK" >&2
        exit 1
    fi
    echo "==> building with the game inside: $APK_LAUNCHER_BUNDLE_APK"
else
    echo "==> no APK in apk/: building the launcher only (it asks for the APK on first start)"
fi
export APK_LAUNCHER_BUNDLE_APK="${APK_LAUNCHER_BUNDLE_APK:-}"

targets=("${1:-x86_64}")
[ "${1:-}" = all ] && targets=(aarch64 x86_64)
# The x86_64 AppImage embeds the ARM launchers (ARM translation layer), which
# come from the aarch64 build: build that first when it is missing.
if [ "${targets[0]}" = x86_64 ] && [ "${ARM_TRANSLATION:-1}" = 1 ] && \
   [ ! -f "$HERE/build-appimage/arm-runtime/aarch64/.complete" ]; then
    targets=(aarch64 x86_64)
fi

# Recent static QEMU user-mode binaries for the ARM translation layer
if [ "${ARM_TRANSLATION:-1}" = 1 ] && [ ! -x "$HERE/build-appimage/qemu/qemu-aarch64" ]; then
    engine=$(command -v podman || command -v docker || true)
    if [ -n "$engine" ]; then
        mkdir -p "$HERE/build-appimage/qemu"
        "$engine" run --rm -v "$HERE/build-appimage/qemu:/out:z" docker.io/library/debian:trixie sh -c \
            'apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq qemu-user-static >/dev/null && cp /usr/bin/qemu-aarch64-static /out/qemu-aarch64 && cp /usr/bin/qemu-arm-static /out/qemu-arm' || true
    fi
fi

for t in "${targets[@]}"; do
    case "$t" in x86_64|aarch64) ;; *) echo "unknown target $t (x86_64|aarch64|all)" >&2; exit 1 ;; esac
    if [ "${NO_CONTAINER:-0}" = 1 ]; then
        TARGET=$t WORK="$WORK" PROJECT_NAME="$(basename "$HERE")" ARM_TRANSLATION="${ARM_TRANSLATION:-1}" bash "$HERE/scripts/container-build.sh"
        continue
    fi
    engine=$(command -v podman || command -v docker || true)
    if [ -z "$engine" ]; then
        echo "podman or docker is required (or run with NO_CONTAINER=1 on Ubuntu 22.04)" >&2
        exit 1
    fi
    # Reuse a container with the packages already installed
    name="apk-launcher-build-$t"
    if ! "$engine" container exists "$name" 2>/dev/null && ! "$engine" inspect "$name" >/dev/null 2>&1; then
        "$engine" create --name "$name" -v "$WORK:/work:z" -w /work \
            docker.io/library/ubuntu:22.04 sleep infinity >/dev/null
    fi
    "$engine" start "$name" >/dev/null
    "$engine" exec -e PODMAN="$(basename "$engine" | grep -x podman || true)" -e TARGET="$t" -e ARM_TRANSLATION="${ARM_TRANSLATION:-1}" -e APK_LAUNCHER_ICON="${APK_LAUNCHER_ICON:-}" -e APK_LAUNCHER_BUNDLE_APK="${APK_LAUNCHER_BUNDLE_APK:-}" -e WORK=/work -e PROJECT_NAME="$(basename "$HERE")" -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
        "$name" bash "/work/$(basename "$HERE")/scripts/container-build.sh"
    "$engine" stop -t 1 "$name" >/dev/null
done
ls -la "$HERE/dist"
