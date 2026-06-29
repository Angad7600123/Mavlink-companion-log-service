#!/usr/bin/env bash
# Rebuild and reinstall the mcls binary only. Never touches /etc/mcls/config.toml.
set -euo pipefail

PREFIX="${PREFIX:-/usr/local}"
BUILD_DIR="${BUILD_DIR:-build}"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=cmake-build-common.sh
source "$(dirname "${BASH_SOURCE[0]}")/cmake-build-common.sh"

require_build_tools

echo "==> Building MAVLink Companion Log Service"
mcls_cmake_configure
mcls_cmake_build 2

BINARY="${BUILD_PATH}/mcls"
if [[ ! -s "${BINARY}" ]]; then
    echo "ERROR: ${BINARY} is missing or empty; refusing to install." >&2
    exit 1
fi

echo "==> Installing binary (config unchanged)"
sudo install -Dm755 "${BINARY}" "${PREFIX}/bin/mcls"
sudo ln -sf "${PREFIX}/bin/mcls" "${PREFIX}/bin/mclsd"

echo "Update complete. Config was not modified: /etc/mcls/config.toml"
echo "Restart with: sudo systemctl restart mavlink-companion-log-service"
