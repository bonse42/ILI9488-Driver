#!/usr/bin/env bash
set -e

REMOTE_PI=${REMOTE_PI:-pi@raspberry.local}
REMOTE_PATH=${REMOTE_PATH:-/home/pi}

if [[ ! -d ./build ]]; then
  echo "Missing build/ directory. Run scripts/build.sh first." >&2
  exit 1
fi

DEB_FILE=$(ls -1 ./build/*.deb | head -n 1)
if [[ -z "${DEB_FILE}" ]]; then
  echo "No .deb package found in build/." >&2
  exit 1
fi

ssh ${REMOTE_PI} "mkdir -p ${REMOTE_PATH}"
rsync -av "${DEB_FILE}" ${REMOTE_PI}:${REMOTE_PATH}/