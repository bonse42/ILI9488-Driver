#!/usr/bin/env bash
set -e
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "${SCRIPT_DIR}/.." && pwd)

REMOTE_PI=${REMOTE_PI:-pi@raspberry}
BUILD_DIR="${ROOT_DIR}/build"

if [[ ! -d "${BUILD_DIR}" ]]; then
  echo "Missing build/ directory. Run scripts/build.sh first." >&2
  exit 1
fi

DEB_FILE=$(ls -1t "${BUILD_DIR}"/*.deb 2>/dev/null | head -n 1)
if [[ -z "${DEB_FILE}" ]]; then
  echo "No .deb package found in build/." >&2
  exit 1
fi

DEB_NAME=$(basename "${DEB_FILE}")
echo "Deploying ${DEB_NAME} to ${REMOTE_PI}..."

scp "${DEB_FILE}" "${REMOTE_PI}:/tmp/${DEB_NAME}"
ssh "${REMOTE_PI}" "sudo dpkg -i /tmp/${DEB_NAME} && rm /tmp/${DEB_NAME}"

echo "Deployed and installed successfully."
