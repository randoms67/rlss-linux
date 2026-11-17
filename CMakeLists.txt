cmake_minimum_required(VERSION 3.16...4.0)

project(apk-launcher VERSION 0.1.0 LANGUAGES C CXX ASM)

# Some reused sub projects declare very old cmake_minimum_required versions
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-delete-null-pointer-checks")

# ---------------------------------------------------------------------------
# Location of the cloned https://github.com/minecraft-linux/mcpelauncher-manifest
# (with submodules). We reuse its Android linker, libc shim, fake JNI VM and
# SDL3 window layer; only the "client" part is replaced by our own generic one.
# ---------------------------------------------------------------------------
set(MCPELAUNCHER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mcpelauncher-manifest" CACHE PATH
    "Path to a recursive checkout of mcpelauncher-manifest")
get_filename_component(MCPELAUNCHER_DIR "${MCPELAUNCHER_DIR}" ABSOLUTE)
if(NOT EXISTS "${MCPELAUNCHER_DIR}/mcpelauncher-linker/CMakeLists.txt")
    message(FATAL_ERROR "mcpelauncher-manifest not found at ${MCPELAUNCHER_DIR}. "
        "Clone it with: git clone --recursive https://github.com/minecraft-linux/mcpelauncher-manifest")
endif()
set(CMAKE_MODULE_PATH "${MCPELAUNCHER_DIR}/cmake" ${CMAKE_MODULE_PATH})

# --- Architecture -----------------------------------------------------------
# The Android libraries inside an APK are loaded *in-process*, so the launcher
# binary must have the same architecture as the libs:
#   x86_64 launcher -> lib/x86_64       i686 launcher  -> lib/x86
#   aarch64 launcher -> lib/arm64-v8a   armhf launcher -> lib/armeabi-v7a
option(BUILD_X86 "Build a 32-bit x86 (i686) launcher on an x86_64 host (-m32)" OFF)

set(IS_64BIT FALSE)
if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT BUILD_X86)
    set(IS_64BIT TRUE)
endif()
set(IS_ARM_BUILD FALSE)
set(IS_ARMHF_BUILD FALSE)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|aarch64|ARM|AARCH64)")
    set(IS_ARM_BUILD TRUE)
    set(IS_ARMHF_BUILD TRUE)
elseif(BUILD_X86)
    set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -m32")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -m32")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -m32")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -m32")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -m32")
    set(CMAKE_LIBRARY_ARCHITECTURE "i386-linux-gnu")
    set(CMAKE_CXX_COMPILER_TARGET "i686-linux-gnu")
    set(CMAKE_SYSTEM_PROCESSOR i686)
    # Arch / Debian keep 32-bit libraries here
    list(APPEND CMAKE_PREFIX_PATH /usr/lib32 /usr/lib/i386-linux-gnu)
    set(FIND_LIBRARY_USE_LIB32_PATHS ON)
    set_property(GLOBAL PROPERTY FIND_LIBRARY_USE_LIB32_PATHS ON)
    set_property(GLOBAL PROPERTY FIND_LIBRARY_USE_LIB64_PATHS OFF)
endif()

# Dummy helper some sub projects expect from the manifest's top level CMakeLists
function(git_commit_hash DIR OUT_VAL)
    set(${OUT_VAL} "" PARENT_SCOPE)
endfunction()

# --- Window system: SDL3 (vendored, statically linked) ----------------------
set(GAMEWINDOW_SYSTEM SDL3 CACHE STRING "" FORCE)
option(SDL3_VENDORED "Use the vendored SDL3 from mcpelauncher-manifest" ON)
if(SDL3_VENDORED)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
    set(SDL_RENDER OFF CACHE BOOL "" FORCE)
    set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
    set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
    set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
    set(SDL_GPU OFF CACHE BOOL "" FORCE)
    set(SDL_METAL OFF CACHE BOOL "" FORCE)
    # the vendored SDL revision does not compile against current PipeWire
    # headers; PulseAudio output works on PipeWire systems too.
    set(SDL_PIPEWIRE OFF CACHE BOOL "" FORCE)
    set(SDL_VIDEO_DRIVER_X11_SUPPORTS_GENERIC_EVENTS 1 CACHE STRING "" FORCE)
    add_subdirectory("${MCPELAUNCHER_DIR}/sdl3" sdl3 EXCLUDE_FROM_ALL)
else()
    find_package(SDL3 REQUIRED CONFIG COMPONENTS SDL3-shared)
endif()

# --- Reused mcpelauncher components -----------------------------------------
set(JNIVM_ENABLE_RETURN_NON_ZERO ON CACHE BOOL "" FORCE)
set(JNIVM_FAKE_JNI_MINECRAFT_LINUX_COMPAT ON CACHE BOOL "" FORCE)
set(JNIVM_ENABLE_DEBUG OFF CACHE BOOL "" FORCE)

foreach(sub logger base64 file-util properties-parser
            mcpelauncher-linker libc-shim game-window
            minecraft-imported-symbols mcpelauncher-common mcpelauncher-core
            axml-parser android-support-headers libjnivm)
    add_subdirectory("${MCPELAUNCHER_DIR}/${sub}" "mcpelauncher/${sub}" EXCLUDE_FROM_ALL)
endforeach()
# libjnivm asks for C++14, which no longer builds against recent libstdc++
set_target_properties(jnivm fake-jni baron PROPERTIES CXX_STANDARD 17)
# jnivm headers use std::string in unordered_map without including <string>
target_compile_options(jnivm PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include string>")

# --- The launcher -----------------------------------------------------------
add_executable(apk-launcher
    src/tls_guard.cpp  # must stay first: its TLS block sits at a fixed offset
    src/main.cpp
    src/zip_reader.cpp
    src/elf_scan.cpp
    src/apk_info.cpp
    src/host.cpp
    src/looper_input.cpp
    src/native_window.cpp
    src/egl.cpp
    src/assets.cpp
    src/audio.cpp
    src/android_misc.cpp
    src/jni_classes.cpp
    src/stubs.cpp
    src/keymap.cpp
    src/thread_mover.cpp
    src/ue4_support.cpp
    src/stackdump.cpp
    src/control.cpp
    src/uri_handler.cpp
    src/gamepad_bridge.cpp
    src/sigsetjmp_aarch64.S
)
target_include_directories(apk-launcher PRIVATE src "${MCPELAUNCHER_DIR}/mcpelauncher-client/src")
target_link_libraries(apk-launcher PRIVATE
    logger mcpelauncher-core gamewindow baron android-support-headers libc-shim
    linker axml_parser file-util SDL3::SDL3 ZLIB::ZLIB ${CMAKE_DL_LIBS} pthread)
find_package(ZLIB REQUIRED)
target_compile_definitions(apk-launcher PRIVATE
    APK_LAUNCHER_VERSION="${PROJECT_VERSION}"
    $<$<BOOL:${IS_ARMHF_BUILD}>:IS_ARM_BUILD>)

if(IS_ARMHF_BUILD AND NOT IS_64BIT)
    # 32-bit ARM: Android uses the soft-float calling convention, the host the
    # hard-float one. Reuse mcpelauncher's bionic libc/libm and GL thunks.
    target_compile_definitions(apk-launcher PRIVATE USE_ARMHF_SUPPORT)
    target_sources(apk-launcher PRIVATE "${MCPELAUNCHER_DIR}/mcpelauncher-client/src/armhf_support.cpp")
endif()

# Location of prebuilt helper libs (armhf libc.so etc) at runtime
target_compile_definitions(apk-launcher PRIVATE
    MCPELAUNCHER_BIN_DIR_DEV="${MCPELAUNCHER_DIR}/mcpelauncher-linux-bin")

install(TARGETS apk-launcher RUNTIME DESTINATION bin)
install(DIRECTORY "${MCPELAUNCHER_DIR}/mcpelauncher-linux-bin/lib/" DESTINATION share/apk-launcher/lib
        FILES_MATCHING PATTERN "libc.so" PATTERN "libm.so" PATTERN "liblog.so")

option(APK_LAUNCHER_STATIC_LIBSTDCXX "Link libstdc++/libgcc statically (portable AppImage builds)" OFF)
if(APK_LAUNCHER_STATIC_LIBSTDCXX)
    target_link_options(apk-launcher PRIVATE -static-libstdc++ -static-libgcc)
endif()
