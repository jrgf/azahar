#!/usr/bin/env bash

# Copyright Azahar Emulator Project
# Licensed under GPLv2 or any later version.
# Refer to the license.txt file included.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
APP_DIR="${ROOT_DIR}/src/switch_homebrew"
OUTPUT_DIR="${ROOT_DIR}/build/switch"
NRO_PATH="${APP_DIR}/azahar.nro"

if ! command -v make >/dev/null 2>&1; then
    echo "ERROR: make is not installed or not in PATH"
    exit 1
fi

if [[ -z "${DEVKITPRO:-}" ]]; then
    echo "ERROR: DEVKITPRO is not set"
    echo "Install devkitPro switch-dev, then log out and back in so the environment is configured."
    exit 1
fi

if [[ ! -f "${DEVKITPRO}/libnx/switch_rules" ]]; then
    echo "ERROR: libnx switch_rules not found at ${DEVKITPRO}/libnx/switch_rules"
    echo "Install or update the Switch toolchain with: sudo dkp-pacman -Syu switch-dev"
    exit 1
fi

make -C "${APP_DIR}" "$@"

if [[ ! -f "${NRO_PATH}" ]]; then
    if [[ $# -gt 0 ]]; then
        exit 0
    fi

    echo "ERROR: expected output not found: ${NRO_PATH}"
    exit 1
fi

mkdir -p "${OUTPUT_DIR}"
cp "${NRO_PATH}" "${OUTPUT_DIR}/azahar.nro"

echo "Built ${OUTPUT_DIR}/azahar.nro"
