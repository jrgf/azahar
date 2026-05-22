#!/usr/bin/env bash

# Copyright Azahar Emulator Project
# Licensed under GPLv2 or any later version.
# Refer to the license.txt file included.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-switch-core-cmake"
AZAHAR_SWITCH_ENABLE_OPENGL="${AZAHAR_SWITCH_ENABLE_OPENGL:-OFF}"
AZAHAR_SWITCH_OPENGL_SPIKE="${AZAHAR_SWITCH_OPENGL_SPIKE:-${AZAHAR_SWITCH_ENABLE_OPENGL}}"
# uam (deko3d) and libEGL (switch-mesa) both vendor Mesa's glsl_types and
# clash at link time. The two renderers are mutually exclusive at runtime via
# AZAHAR_SWITCH_OPENGL_SPIKE, so default deko3d off whenever OpenGL is on.
if [[ "${AZAHAR_SWITCH_ENABLE_OPENGL}" == "ON" ]]; then
    AZAHAR_SWITCH_ENABLE_DEKO3D="${AZAHAR_SWITCH_ENABLE_DEKO3D:-OFF}"
else
    AZAHAR_SWITCH_ENABLE_DEKO3D="${AZAHAR_SWITCH_ENABLE_DEKO3D:-ON}"
fi

if [[ -n "${CMAKE:-}" ]]; then
    CMAKE_BIN="${CMAKE}"
elif command -v cmake >/dev/null 2>&1; then
    CMAKE_BIN="cmake"
elif [[ -x /opt/homebrew/bin/cmake ]]; then
    CMAKE_BIN="/opt/homebrew/bin/cmake"
elif [[ -x /usr/local/bin/cmake ]]; then
    CMAKE_BIN="/usr/local/bin/cmake"
else
    echo "ERROR: cmake is not installed or not in PATH"
    exit 1
fi

if [[ -z "${DEVKITPRO:-}" ]]; then
    if [[ -d /opt/devkitpro ]]; then
        export DEVKITPRO=/opt/devkitpro
    else
        echo "ERROR: DEVKITPRO is not set"
        exit 1
    fi
fi

if [[ -z "${DEVKITA64:-}" ]]; then
    export DEVKITA64="${DEVKITPRO}/devkitA64"
fi

export PATH="${DEVKITA64}/bin:${DEVKITPRO}/tools/bin:${PATH}"

"${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE="${DEVKITPRO}/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG" \
    -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG" \
    -DENABLE_SWITCH_HOMEBREW_CORE=ON \
    -DENABLE_QT=OFF \
    -DENABLE_SDL2=OFF \
    -DENABLE_TESTS=OFF \
    -DENABLE_WEB_SERVICE=OFF \
    -DENABLE_SCRIPTING=OFF \
    -DENABLE_GDBSTUB=OFF \
    -DENABLE_OPENAL=OFF \
    -DENABLE_CUBEB=OFF \
    -DENABLE_LIBUSB=OFF \
    -DENABLE_ROOM=OFF \
    -DENABLE_ROOM_STANDALONE=OFF \
    -DENABLE_OPENGL="${AZAHAR_SWITCH_ENABLE_OPENGL}" \
    -DENABLE_VULKAN=OFF \
    -DENABLE_DEKO3D="${AZAHAR_SWITCH_ENABLE_DEKO3D}" \
    -DAZAHAR_SWITCH_OPENGL_SPIKE="${AZAHAR_SWITCH_OPENGL_SPIKE}" \
    -DENABLE_SOFTWARE_RENDERER=ON

"${CMAKE_BIN}" --build "${BUILD_DIR}" --target azahar-switch-core-nro "$@"
