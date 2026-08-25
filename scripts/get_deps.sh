#!/usr/bin/env bash
# Fetch STM32CubeF4 (CMSIS + HAL + USB Device Library + FreeRTOS) into
# third_party/. A shallow clone is enough for building.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-$HERE/third_party/STM32CubeF4}"
REPO="https://github.com/STMicroelectronics/STM32CubeF4.git"
# Pin a known-good tag; bump as needed.
TAG="${CUBE_TAG:-v1.28.0}"

if [ -d "$DEST/Drivers/STM32F4xx_HAL_Driver/Src" ]; then
    echo "STM32CubeF4 already present at: $DEST"
    exit 0
fi

mkdir -p "$(dirname "$DEST")"
echo "Cloning STM32CubeF4 $TAG -> $DEST"
git clone --depth 1 --branch "$TAG" "$REPO" "$DEST"
echo "Done. Build with: make"
