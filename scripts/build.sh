#!/usr/bin/env bash
set -e
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "${SCRIPT_DIR}/.." && pwd)

BUILD_DIR="${ROOT_DIR}/build"
TOOLCHAIN_FILE="${ROOT_DIR}/toolchains/aarch64-rpi.cmake"
DEB_ARCH=arm64

rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
  -DCPACK_DEBIAN_PACKAGE_ARCHITECTURE="${DEB_ARCH}"

cmake --build "${BUILD_DIR}" -j"$(nproc)"
cd "${BUILD_DIR}" && cpack -G DEB

echo ""
echo "Build complete: $(ls -1 "${BUILD_DIR}"/*.deb)"
