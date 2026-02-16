#!/usr/bin/env bash
set -e
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "${SCRIPT_DIR}/.." && pwd)

rm -rf "${ROOT_DIR}/build"
mkdir -p "${ROOT_DIR}/build"
cd "${ROOT_DIR}/build"

TOOLCHAIN_FILE=./toolchains/aarch64-rpi.cmake
DEB_ARCH=arm64

cmake "${ROOT_DIR}" \
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
  -DCPACK_DEBIAN_PACKAGE_ARCHITECTURE="${DEB_ARCH}"

make -j
cpack -G DEB