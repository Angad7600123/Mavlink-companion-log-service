#!/usr/bin/env bash
# Rebuild and reinstall the mcls binary only. Never touches /etc/mcls/config.toml.
set -euo pipefail

PREFIX="${PREFIX:-/usr/local}"
BUILD_DIR="${BUILD_DIR:-build}"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "==> Building MAVLink Companion Log Service"
cmake -S "${PROJECT_ROOT}" -B "${PROJECT_ROOT}/${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${PROJECT_ROOT}/${BUILD_DIR}" --parallel

BINARY="${PROJECT_ROOT}/${BUILD_DIR}/mcls"
if [[ ! -s "${BINARY}" ]]; then
    echo "ERROR: ${BINARY} is missing or empty; refusing to install." >&2
    exit 1
fi

echo "==> Installing binary (config unchanged)"
sudo install -Dm755 "${BINARY}" "${PREFIX}/bin/mcls"
sudo ln -sf "${PREFIX}/bin/mcls" "${PREFIX}/bin/mclsd"

echo "Update complete. Config was not modified: /etc/mcls/config.toml"
echo "Restart with: sudo systemctl restart mavlink-companion-log-service"
