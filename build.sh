#!/usr/bin/env bash
# Build Rocket League Sideswipe for Linux.
#
#   1. put the game APK into apk/
#   2. ./build.sh            -> dist/RLSS-Linux-x86_64.AppImage   (PCs)
#      ./build.sh aarch64    -> dist/RLSS-Linux-aarch64.AppImage  (ARM64 Linux)
#      ./build.sh all        -> both
#
# Needs docker or podman, and mcpelauncher-manifest next to this folder
# (see README.md).
exec "$(dirname "$(readlink -f "$0")")/scripts/build-appimage.sh" "${1:-x86_64}"
