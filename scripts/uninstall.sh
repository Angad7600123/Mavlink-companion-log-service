#!/usr/bin/env bash
set -euo pipefail

PREFIX="${PREFIX:-/usr/local}"

echo "==> Stopping service"
sudo systemctl stop mavlink-companion-log-service.service || true
sudo systemctl disable mavlink-companion-log-service.service || true

echo "==> Removing unit and binary"
sudo rm -f /lib/systemd/system/mavlink-companion-log-service.service
sudo rm -f "${PREFIX}/bin/mcls" "${PREFIX}/bin/mclsd"
sudo systemctl daemon-reload

echo "==> Removing configuration (state directory preserved)"
sudo rm -rf /etc/mcls

echo "Uninstall complete."
echo "State data remains in /var/lib/mcls unless removed manually."
