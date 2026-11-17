#!/usr/bin/env bash
# Runs INSIDE an Ubuntu 22.04 container (old glibc => AppImage works on most
# distros). Builds the launcher(s) for one AppImage architecture and packs
# the AppImage.
#
#   TARGET=x86_64   -> apk-launcher-x86_64 + apk-launcher-x86 (i686, multilib)
#   TARGET=aarch64  -> apk-launcher-aarch64 + apk-launcher-armhf (cross compiled)
set -euo pipefail

TARGET="${TARGET:-x86_64}"
WORK="${WORK:-/work}"
PROJECT="$WORK/${PROJECT_NAME:-linux-apk-launcher}"
MANIFEST="$WORK/mcpelauncher-manifest"
OUT="$PROJECT/dist"
BUILD="$PROJECT/build-appimage"
JOBS="${JOBS:-$(nproc)}"
export DEBIAN_FRONTEND=noninteractive

DEV_PKGS="zlib1g-dev libssl-dev libpng-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev
libxss-dev libxfixes-dev libxkbcommon-dev libwayland-dev libegl-dev libgles-dev libgl-dev libdecor-0-dev
libpulse-dev libasound2-dev libudev-dev libdbus-1-dev libdrm-dev libgbm-dev"
TOOLS="build-essential cmake ninja-build git python3 pkg-config file wget ca-certificates wayland-protocols
libwayland-bin zip clang lld llvm"

with_arch() { local a=$1; shift; for p in $*; do printf '%s:%s ' "$p" "$a"; done; }

setup_packages() {
    if [ -f /.apk-launcher-deps-$TARGET ]; then return; fi
    if [ "$TARGET" = x86_64 ]; then
        dpkg --add-architecture i386
        apt-get update
        apt-get install -y --no-install-recommends $TOOLS $DEV_PKGS g++-multilib \
            $(with_arch i386 zlib1g-dev libssl-dev libpng-dev)
    else
        # arm64/armhf packages come from ports.ubuntu.com
        . /etc/os-release
        sed -i 's/^deb http/deb [arch=amd64,i386] http/' /etc/apt/sources.list
        for comp in "" -updates -security; do
            echo "deb [arch=arm64,armhf] http://ports.ubuntu.com/ubuntu-ports $VERSION_CODENAME$comp main universe" \
                >> /etc/apt/sources.list.d/ports.list
        done
        dpkg --add-architecture arm64
        dpkg --add-architecture armhf
        apt-get update
        apt-get install -y --no-install-recommends $TOOLS $DEV_PKGS \
            crossbuild-essential-arm64 crossbuild-essential-armhf \
            $(with_arch arm64 $DEV_PKGS) $(with_arch armhf zlib1g-dev libssl-dev libpng-dev libx11-dev libwayland-dev libegl-dev libgles-dev libpulse-dev libasound2-dev libudev-dev libdbus-1-dev libxkbcommon-dev libdecor-0-dev libdrm-dev libgbm-dev)
    fi
    touch /.apk-launcher-deps-$TARGET
}

# Puts qemu-user + the ARM launchers + their ARM runtimes into the x86_64 AppDir
embed_arm_translation() {
    local appdir=$1
    if [ ! -x "$BUILD/aarch64/apk-launcher" ] || [ ! -f "$BUILD/arm-runtime/aarch64/.complete" ]; then
        echo "!! ARM launchers/runtime not built yet - run the aarch64 target first (skipping ARM translation)"
        return
    fi
    local t="$appdir/usr/lib/arm-translation"
    mkdir -p "$t"
    # prefer a recent static QEMU (scripts/build-appimage.sh fetches Debian's),
    # it translates ARM code much faster than Ubuntu 22.04's QEMU 6.2
    if [ -x "$BUILD/qemu/qemu-aarch64" ] && [ -x "$BUILD/qemu/qemu-arm" ]; then
        cp "$BUILD/qemu/qemu-aarch64" "$BUILD/qemu/qemu-arm" "$t/"
    else
        command -v qemu-aarch64-static >/dev/null || apt-get install -y --no-install-recommends qemu-user-static
        cp /usr/bin/qemu-aarch64-static "$t/qemu-aarch64"
        cp /usr/bin/qemu-arm-static "$t/qemu-arm"
    fi
    echo "  qemu: $("$t/qemu-aarch64" --version | head -1)"
    cp "$BUILD/aarch64/apk-launcher" "$appdir/usr/bin/apk-launcher-aarch64"
    cp -a "$BUILD/arm-runtime/aarch64" "$t/aarch64"
    if [ -x "$BUILD/armhf/apk-launcher" ] && [ -f "$BUILD/arm-runtime/armhf/.complete" ]; then
        cp "$BUILD/armhf/apk-launcher" "$appdir/usr/bin/apk-launcher-armhf"
        cp -a "$BUILD/arm-runtime/armhf" "$t/armhf"
        mkdir -p "$appdir/usr/share/apk-launcher/lib/armeabi-v7a"
        cp "$MANIFEST/mcpelauncher-linux-bin/lib/armeabi-v7a/"lib{c,m}.so "$appdir/usr/share/apk-launcher/lib/armeabi-v7a/" 2>/dev/null || true
    fi
    # GPU for ARM apps: Mesa "virpipe" in the ARM process sends GL work to a
    # native virglrenderer server that renders with the host GPU driver.
    command -v virgl_test_server >/dev/null || apt-get install -y --no-install-recommends virgl-server
    mkdir -p "$t/host"
    cp "$(command -v virgl_test_server)" "$t/host/virgl_test_server"
    bundle_libs "$t/host/virgl_test_server" x86_64-linux-gnu "$t/host"
    echo "==> embedded ARM translation layer ($(du -sh "$t" | cut -f1))"
}

# build <variant> <extra cmake args...>
build() {
    local variant=$1; shift
    local dir="$BUILD/$variant"
    cmake -S "$PROJECT" -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DMCPELAUNCHER_DIR="$MANIFEST" -DAPK_LAUNCHER_STATIC_LIBSTDCXX=ON \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ "$@"
    ninja -C "$dir" -j "$JOBS" apk-launcher
}

# Copy non-system shared libraries a binary needs into the AppDir
EXCLUDE='^(libc|libm|libdl|librt|libpthread|libresolv|libutil|ld-linux.*|libgcc_s|libstdc\+\+|libGL.*|libEGL.*|libGLES.*|libGLX.*|libOpenGL|libX.*|libxcb.*|libwayland.*|libasound|libpulse.*|libdbus-1|libudev|libdrm|libgbm|libz|libxkbcommon.*|libdecor.*)\.so'
bundle_libs() {
    local bin=$1 triplet=$2 dest=$3
    mkdir -p "$dest"
    local queue=("$bin") seen=""
    while [ ${#queue[@]} -gt 0 ]; do
        local f=${queue[0]}; queue=("${queue[@]:1}")
        for lib in $(readelf -d "$f" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p'); do
            [[ " $seen " == *" $lib "* ]] && continue
            seen="$seen $lib"
            [[ "$lib" =~ $EXCLUDE ]] && continue
            local src=""
            for d in /usr/lib/$triplet /lib/$triplet; do
                [ -e "$d/$lib" ] && src="$d/$lib" && break
            done
            if [ -n "$src" ]; then
                cp -L "$src" "$dest/$lib"
                queue+=("$dest/$lib")
                echo "  bundled $lib"
            fi
        done
    done
}

# ---------------------------------------------------------------------------
# ARM translation layer: a minimal ARM user space (glibc, Mesa llvmpipe
# EGL/GLES, X11/Wayland/PulseAudio client libs) that the x86_64 AppImage runs
# the ARM launchers in, under qemu user-mode emulation.
# ---------------------------------------------------------------------------
ARM_RUNTIME_ROOTS="libc6 libstdc++6 libgcc-s1 libatomic1 zlib1g libssl3 libpng16-16 libegl1 libegl-mesa0 libgles2
libgl1-mesa-dri libglapi-mesa libgbm1 libx11-6 libx11-xcb1 libxext6 libxrandr2 libxcursor1 libxi6 libxfixes3
libxss1 libxkbcommon0 libwayland-client0 libwayland-egl1 libwayland-cursor0 libdecor-0-0 libpulse0 libasound2
libudev1 libdbus-1-3 libdrm2 libxcb-dri3-0 libxcb-present0 libxcb-sync1 libxshmfence1 libxcb-xfixes0 libxcb-randr0
libxcb-shm0 libxcb-dri2-0 libxcb-glx0"
# arch-independent / tool packages we never need inside the runtime
ARM_RUNTIME_SKIP='^(debconf|perl-base|dpkg|tar|coreutils|adduser|passwd|lsb-base|init-system-helpers|sensible-utils|base-files|libc-bin|gcc-12-base|ucf|systemd|libpam.*|login|util-linux|mount|bash|dash|grep|sed|findutils|diffutils|debianutils|hostname|ncurses-base|ncurses-bin|procps)(:|$)'

make_arm_runtime() {
    local arch=$1 dest=$2
    [ -f "$dest/.complete" ] && return
    echo "==> collecting $arch runtime into $dest"
    rm -rf "$dest" "$BUILD/debs-$arch"
    mkdir -p "$dest" "$BUILD/debs-$arch"
    local pkgs
    pkgs=$(apt-cache depends --recurse --no-recommends --no-suggests --no-conflicts --no-breaks \
            --no-replaces --no-enhances $(with_arch "$arch" $ARM_RUNTIME_ROOTS) \
        | grep -E '^[a-z0-9]' | sed "s/:$arch\$//" | sort -u | grep -Ev "$ARM_RUNTIME_SKIP")
    (cd "$BUILD/debs-$arch" && for p in $pkgs; do
        apt-get download -q "$p:$arch" >/dev/null 2>&1 || apt-get download -q "$p" >/dev/null 2>&1 || echo "  (skip $p)"
    done)
    for d in "$BUILD/debs-$arch"/*.deb; do dpkg -x "$d" "$dest"; done
    rm -rf "$dest"/usr/share/{doc,man,info,locale,lintian,bug,bash-completion} "$dest"/etc/init.d "$BUILD/debs-$arch"
    # libraries the launcher itself links against
    touch "$dest/.complete"
    du -sh "$dest"
}

get_appimagetool() {
    local tool="$BUILD/appimagetool-$(uname -m).AppImage"
    if [ ! -x "$tool" ]; then
        wget -q -O "$tool" "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$(uname -m).AppImage"
        chmod +x "$tool"
    fi
    echo "$tool"
}

apply_patches() {
    # fixes for the reused mcpelauncher libraries (idempotent)
    # (safe.directory: the checkout belongs to another user than the container's root)
    local g=(git -c safe.directory='*' -C "$MANIFEST/libc-shim")
    for p in "$PROJECT"/patches/libc-shim-*.patch; do
        [ -f "$p" ] || continue
        if "${g[@]}" apply --check "$p" 2>/dev/null; then
            "${g[@]}" apply "$p" && echo "  applied $(basename "$p")"
        elif "${g[@]}" apply --reverse --check "$p" 2>/dev/null; then
            echo "  already applied: $(basename "$p")"
        else
            echo "!! $(basename "$p") does not apply to $MANIFEST/libc-shim (wrong mcpelauncher-manifest revision?)" >&2
            exit 1
        fi
    done
}

main() {
    setup_packages
    apply_patches
    mkdir -p "$BUILD" "$OUT"
    local appdir="$BUILD/AppDir-$TARGET"
    rm -rf "$appdir"
    mkdir -p "$appdir/usr/bin" "$appdir/usr/lib" "$appdir/usr/share/apk-launcher/lib" \
             "$appdir/usr/share/applications" "$appdir/usr/share/icons/hicolor/scalable/apps" \
             "$appdir/usr/share/mime/packages"

    if [ "$TARGET" = x86_64 ]; then
        build x86_64
        build x86 -DBUILD_X86=ON -DCMAKE_LIBRARY_PATH=/usr/lib/i386-linux-gnu
        cp "$BUILD/x86_64/apk-launcher" "$appdir/usr/bin/apk-launcher-x86_64"
        cp "$BUILD/x86/apk-launcher" "$appdir/usr/bin/apk-launcher-x86"
        bundle_libs "$appdir/usr/bin/apk-launcher-x86_64" x86_64-linux-gnu "$appdir/usr/lib/x86_64"
        bundle_libs "$appdir/usr/bin/apk-launcher-x86" i386-linux-gnu "$appdir/usr/lib/x86"
    else
        build aarch64 -DCMAKE_TOOLCHAIN_FILE="$PROJECT/cmake/toolchain-aarch64.cmake"
        build armhf -DCMAKE_TOOLCHAIN_FILE="$PROJECT/cmake/toolchain-armhf.cmake"
        cp "$BUILD/aarch64/apk-launcher" "$appdir/usr/bin/apk-launcher-aarch64"
        cp "$BUILD/armhf/apk-launcher" "$appdir/usr/bin/apk-launcher-armhf"
        bundle_libs "$appdir/usr/bin/apk-launcher-aarch64" aarch64-linux-gnu "$appdir/usr/lib/aarch64"
        bundle_libs "$appdir/usr/bin/apk-launcher-armhf" arm-linux-gnueabihf "$appdir/usr/lib/armhf"
        # bionic libc/libm used for the armeabi-v7a soft-float ABI bridge
        mkdir -p "$appdir/usr/share/apk-launcher/lib/armeabi-v7a"
        cp "$MANIFEST/mcpelauncher-linux-bin/lib/armeabi-v7a/"lib{c,m}.so "$appdir/usr/share/apk-launcher/lib/armeabi-v7a/" 2>/dev/null || true
        # runtimes used by the x86_64 AppImage's ARM translation layer
        make_arm_runtime arm64 "$BUILD/arm-runtime/aarch64"
        make_arm_runtime armhf "$BUILD/arm-runtime/armhf"
    fi
    if [ "$TARGET" = x86_64 ] && [ "${ARM_TRANSLATION:-1}" = 1 ]; then
        embed_arm_translation "$appdir"
    fi
    for b in "$appdir"/usr/bin/apk-launcher-*; do
        case "$b" in
            *armhf) arm-linux-gnueabihf-strip "$b" 2>/dev/null || llvm-strip "$b" ;;
            *aarch64) aarch64-linux-gnu-strip "$b" 2>/dev/null || llvm-strip "$b" ;;
            *) strip "$b" ;;
        esac
    done

    install -m755 "$PROJECT/appimage/AppRun" "$appdir/AppRun"
    cp "$PROJECT/appimage/apk-launcher.desktop" "$appdir/"
    # APK_LAUNCHER_BUNDLE_APK=<apk>: build with the game inside (starts it
    # directly). Without it, AppRun asks for the APK on first start.
    if [ -n "${APK_LAUNCHER_BUNDLE_APK:-}" ] && [ -f "$PROJECT/$APK_LAUNCHER_BUNDLE_APK" ]; then
        cp "$PROJECT/$APK_LAUNCHER_BUNDLE_APK" "$appdir/usr/share/apk-launcher/app.apk"
        echo "  bundled game: $APK_LAUNCHER_BUNDLE_APK ($(du -h "$appdir/usr/share/apk-launcher/app.apk" | cut -f1))"
    fi
    cp "$appdir/apk-launcher.desktop" "$appdir/usr/share/applications/"
    # Icon: the game's icon (appimage/rlss-linux.png), APK_LAUNCHER_ICON overrides it
    local icon="$PROJECT/${APK_LAUNCHER_ICON:-appimage/rlss-linux.png}"
    cp "$icon" "$appdir/apk-launcher.png"
    size=$(python3 -c "import struct,sys;d=open(sys.argv[1],'rb').read(24);print(struct.unpack('>I',d[16:20])[0])" "$appdir/apk-launcher.png")
    mkdir -p "$appdir/usr/share/icons/hicolor/${size}x${size}/apps"
    cp "$appdir/apk-launcher.png" "$appdir/usr/share/icons/hicolor/${size}x${size}/apps/"
    ln -sf apk-launcher.png "$appdir/.DirIcon"
    cp "$PROJECT/LICENSE" "$appdir/usr/share/apk-launcher/" 2>/dev/null || true

    local tool
    tool="$(get_appimagetool)"
    local version
    version="$(sed -n 's/^project(apk-launcher VERSION \([0-9.]*\).*/\1/p' "$PROJECT/CMakeLists.txt")"
    local outname="RLSS-Linux-$TARGET.AppImage"
    APPIMAGE_EXTRACT_AND_RUN=1 ARCH="$TARGET" VERSION="$version" "$tool" --no-appstream "$appdir" "$OUT/$outname"
    # docker runs as real root: hand the files back (rootless podman maps root to the user already)
    if [ -n "${HOST_UID:-}" ] && [ "${HOST_UID}" != 0 ] && [ -z "${PODMAN:-}" ]; then chown -R "$HOST_UID:$HOST_GID" "$OUT" "$BUILD" 2>/dev/null || true; fi
    echo "==> $OUT/$outname"
}

main "$@"
