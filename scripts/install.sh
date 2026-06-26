#!/usr/bin/env bash
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

echo "==> Creating system user"
if ! id -u mcls >/dev/null 2>&1; then
    sudo useradd --system --no-create-home --shell /usr/sbin/nologin mcls
fi

echo "==> Installing binary"
sudo install -Dm755 "${BINARY}" "${PREFIX}/bin/mcls"
sudo ln -sf "${PREFIX}/bin/mcls" "${PREFIX}/bin/mclsd"

echo "==> Installing configuration"
sudo install -d -m 0755 /etc/mcls
# Always refresh the reference copy; safe to overwrite.
sudo install -Dm644 "${PROJECT_ROOT}/config/config.toml" /etc/mcls/config.toml.example
if sudo test -f /etc/mcls/config.toml; then
    echo "    Keeping existing /etc/mcls/config.toml (not overwritten)"
else
    sudo install -Dm644 "${PROJECT_ROOT}/config/config.toml" /etc/mcls/config.toml
    echo "    Installed default /etc/mcls/config.toml (copy from config.toml.example for custom setups)"
fi

echo "==> Creating state directory"
sudo install -d -o mcls -g mcls -m 0750 /var/lib/mcls/logs
sudo install -d -o mcls -g mcls -m 0750 /var/lib/mcls/tmp
sudo touch /var/lib/mcls/database.sqlite
sudo chown mcls:mcls /var/lib/mcls/database.sqlite
sudo chmod 0640 /var/lib/mcls/database.sqlite

echo "==> Installing systemd unit"
sudo install -Dm644 "${PROJECT_ROOT}/systemd/mavlink-companion-log-service.service" \
    /lib/systemd/system/mavlink-companion-log-service.service

echo "==> Enabling service"
sudo systemctl daemon-reload
sudo systemctl enable mavlink-companion-log-service.service

echo "Installation complete."
echo "Start with: sudo systemctl start mavlink-companion-log-service"
