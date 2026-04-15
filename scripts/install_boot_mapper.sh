#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run this installer with sudo or as root."
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_BOOT_SCRIPT="${SCRIPT_DIR}/mapper_boot_runner.sh"
INSTALLED_BOOT_SCRIPT="/usr/local/bin/mapper_boot_runner.sh"
SERVICE_PATH="/etc/systemd/system/mapper_boot_mapper.service"

if [[ ! -f "${SOURCE_BOOT_SCRIPT}" ]]; then
  echo "Missing source script: ${SOURCE_BOOT_SCRIPT}"
  exit 1
fi

install -m 0755 "${SOURCE_BOOT_SCRIPT}" "${INSTALLED_BOOT_SCRIPT}"

cat > "${SERVICE_PATH}" <<EOF
[Unit]
Description=Boot mapper updater and launcher
After=local-fs.target systemd-udev-settle.service
Wants=systemd-udev-settle.service

[Service]
Type=simple
ExecStart=${INSTALLED_BOOT_SCRIPT}
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable mapper_boot_mapper.service
systemctl restart mapper_boot_mapper.service

echo "Installed:"
echo "  Boot script source: ${SOURCE_BOOT_SCRIPT}"
echo "  Boot script target: ${INSTALLED_BOOT_SCRIPT}"
echo "  Service: ${SERVICE_PATH}"
echo
echo "Service status:"
systemctl --no-pager --full status mapper_boot_mapper.service || true
