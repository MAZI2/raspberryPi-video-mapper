#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run this installer with sudo or as root."
  exit 1
fi

INSTALLED_BOOT_SCRIPT="/usr/local/bin/mapper_boot_runner.sh"
SERVICE_PATH="/etc/systemd/system/mapper_boot_mapper.service"

cat > "${INSTALLED_BOOT_SCRIPT}" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

USB_RELATIVE_PATH="raspberryPi-video-mapper/mapper"
LOCAL_PROJECT_ROOT="/opt/raspberryPi-video-mapper"
LOCAL_MAPPER_DIR="${LOCAL_PROJECT_ROOT}/mapper"
BINARY_NAME="mapping_video_keystone"
BOOT_WAIT_SECONDS=30
BOOT_WAIT_INTERVAL=2
TEMP_MOUNT_BASE="/run/mapper-usb-mounts"
ACTIVE_TEMP_MOUNT=""

log() {
  printf '[%s] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*"
}

unmount_temp_mount() {
  local mount_point="$1"
  [[ -n "${mount_point}" ]] || return 0

  if mountpoint -q "${mount_point}" 2>/dev/null; then
    umount "${mount_point}" || true
  fi
  rmdir "${mount_point}" 2>/dev/null || true
}

cleanup() {
  unmount_temp_mount "${ACTIVE_TEMP_MOUNT}"
}
trap cleanup EXIT

mount_partition_temporarily() {
  local device="$1"
  local mount_point="${TEMP_MOUNT_BASE}/$(basename "${device}")"

  mkdir -p "${mount_point}"

  if mount -o ro "${device}" "${mount_point}" >/dev/null 2>&1; then
    printf '%s\n' "${mount_point}"
    return 0
  fi

  rmdir "${mount_point}" 2>/dev/null || true
  return 1
}

find_source_dir() {
  local device type transport mount_point scan_mount candidate temp_mount

  while read -r device type transport mount_point; do
    [[ "${type}" == "part" && "${transport}" == "usb" ]] || continue

    temp_mount=""
    scan_mount="${mount_point:-}"

    if [[ -z "${scan_mount}" ]]; then
      if ! temp_mount="$(mount_partition_temporarily "${device}")"; then
        continue
      fi
      scan_mount="${temp_mount}"
    fi

    candidate="${scan_mount}/${USB_RELATIVE_PATH}"
    if [[ -d "${candidate}" ]]; then
      ACTIVE_TEMP_MOUNT="${temp_mount}"
      printf '%s\n' "${candidate}"
      return 0
    fi

    unmount_temp_mount "${temp_mount}"
  done < <(lsblk -rpn -o PATH,TYPE,TRAN,MOUNTPOINT)

  return 1
}

project_differs() {
  local source_dir="$1"
  local destination_dir="$2"

  if [[ ! -d "${destination_dir}" ]]; then
    return 0
  fi

  if command -v rsync >/dev/null 2>&1; then
    if rsync -ain --delete "${source_dir}/" "${destination_dir}/" | grep -q .; then
      return 0
    fi
    return 1
  fi

  if diff -qr "${source_dir}" "${destination_dir}" >/dev/null 2>&1; then
    return 1
  fi
  return 0
}

sync_project() {
  local source_dir="$1"
  local destination_dir="$2"

  mkdir -p "$(dirname "${destination_dir}")"

  if command -v rsync >/dev/null 2>&1; then
    rsync -a --delete "${source_dir}/" "${destination_dir}/"
    return 0
  fi

  rm -rf "${destination_dir}"
  cp -a "${source_dir}" "${destination_dir}"
}

mkdir -p "${LOCAL_PROJECT_ROOT}" "${TEMP_MOUNT_BASE}"

log "Searching USB drives for ${USB_RELATIVE_PATH}"
source_dir=""
attempts=$((BOOT_WAIT_SECONDS / BOOT_WAIT_INTERVAL))

for ((i=0; i<=attempts; i++)); do
  if source_dir="$(find_source_dir)"; then
    break
  fi
  sleep "${BOOT_WAIT_INTERVAL}"
done

updated=0
if [[ -n "${source_dir}" ]]; then
  log "Found project on USB: ${source_dir}"
  if project_differs "${source_dir}" "${LOCAL_MAPPER_DIR}"; then
    log "Updating local project copy at ${LOCAL_MAPPER_DIR}"
    sync_project "${source_dir}" "${LOCAL_MAPPER_DIR}"
    updated=1
  else
    log "Local project copy is already up to date"
  fi
else
  log "No USB project found, using local copy if available"
fi

if [[ ! -d "${LOCAL_MAPPER_DIR}" ]]; then
  log "No local project copy at ${LOCAL_MAPPER_DIR}; cannot continue"
  exit 1
fi

if [[ "${updated}" -eq 1 || ! -x "${LOCAL_MAPPER_DIR}/${BINARY_NAME}" ]]; then
  log "Building project with make -j"
  (
    cd "${LOCAL_MAPPER_DIR}"
    make -j
  )
fi

log "Launching ${BINARY_NAME}"
cd "${LOCAL_MAPPER_DIR}"
exec env SDL_VIDEODRIVER=kmsdrm "./${BINARY_NAME}"
EOF

chmod 0755 "${INSTALLED_BOOT_SCRIPT}"

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
echo "  Boot script target: ${INSTALLED_BOOT_SCRIPT}"
echo "  Service: ${SERVICE_PATH}"
echo
echo "Service status:"
systemctl --no-pager --full status mapper_boot_mapper.service || true
