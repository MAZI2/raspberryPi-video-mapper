#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run this installer with sudo or as root."
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_SOURCE_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
LOCAL_PROJECT_ROOT="/opt/raspberryPi-video-mapper"
LOCAL_SOURCE_DIR="${LOCAL_PROJECT_ROOT}/mapper-src"

APT_PACKAGES=(
  build-essential
  pkg-config
  libsdl2-dev
  libgpiod-dev
  gpiod
  libgles2-mesa-dev
  libgstreamer-plugins-base1.0-dev
  libgstreamer1.0-dev
  gstreamer1.0-tools
  gstreamer1.0-plugins-base
  gstreamer1.0-plugins-base-apps
  gstreamer1.0-plugins-good
  gstreamer1.0-plugins-bad
  gstreamer1.0-plugins-ugly
  gstreamer1.0-libav
  git
  patch
  rsync
)

echo "Updating apt package index..."
apt-get update

echo "Installing/verifying mapper dependencies..."
DEBIAN_FRONTEND=noninteractive apt-get install -y "${APT_PACKAGES[@]}"

INSTALLED_BOOT_SCRIPT="/usr/local/bin/mapper_boot_runner.sh"
SERVICE_PATH="/etc/systemd/system/mapper_boot_mapper.service"

echo "Bootstrapping local project copy to ${LOCAL_PROJECT_ROOT}..."
mkdir -p "${LOCAL_PROJECT_ROOT}"
rsync -a --delete \
  --exclude '.git/' \
  --exclude '.cache/' \
  --exclude 'mapper/*.o' \
  --exclude 'mapper/mapping_video_keystone' \
  "${PROJECT_SOURCE_ROOT}/" "${LOCAL_PROJECT_ROOT}/"
mkdir -p "${LOCAL_SOURCE_DIR}"
rsync -a --delete \
  "${PROJECT_SOURCE_ROOT}/mapper/" "${LOCAL_SOURCE_DIR}/"

cat > "${INSTALLED_BOOT_SCRIPT}" <<'EOS'
#!/usr/bin/env bash
set -euo pipefail

LOCAL_PROJECT_ROOT="/opt/raspberryPi-video-mapper"
LOCAL_SOURCE_DIR="${LOCAL_PROJECT_ROOT}/mapper-src"
LOCAL_MAPPER_DIR="${LOCAL_PROJECT_ROOT}/mapper"
BOOT_CONFIG_NAME="configure.conf"
LEGACY_BOOT_CONFIG_NAME="start_on_boot.conf"
LOCAL_BOOT_CONFIG="${LOCAL_PROJECT_ROOT}/${BOOT_CONFIG_NAME}"
BINARY_NAME="mapping_video_keystone"
BOOT_WAIT_SECONDS=30
BOOT_WAIT_INTERVAL=2
TEMP_MOUNT_BASE="/run/mapper-usb-mounts"
ACTIVE_TEMP_MOUNT=""
TEMP_CANDIDATE_DIR=""

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
  if [[ -n "${TEMP_CANDIDATE_DIR}" && -d "${TEMP_CANDIDATE_DIR}" ]]; then
    rm -rf "${TEMP_CANDIDATE_DIR}" || true
  fi
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
  local device type transport mount_point scan_mount candidate temp_mount part_label

  while read -r device type transport mount_point; do
    [[ "${type}" == "part" ]] || continue

    if [[ -n "${CONFIGURED_USB_LABEL}" ]]; then
      part_label="$(lsblk -no LABEL "${device}" 2>/dev/null | head -n1 | sed -E 's/^[[:space:]]+|[[:space:]]+$//g')"
      [[ "${part_label}" == "${CONFIGURED_USB_LABEL}" ]] || continue
    fi

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

copy_if_changed() {
  local src="$1"
  local dst="$2"

  if [[ -f "${dst}" ]] && cmp -s "${src}" "${dst}"; then
    return 1
  fi

  cp "${src}" "${dst}"
  return 0
}

get_config_value() {
  local key="$1"
  local default_value="$2"

  if [[ ! -f "${LOCAL_BOOT_CONFIG}" ]]; then
    printf '%s\n' "${default_value}"
    return 0
  fi

  local line value
  line="$(grep -E "^[[:space:]]*${key}[[:space:]]*=" "${LOCAL_BOOT_CONFIG}" | tail -n 1 || true)"
  if [[ -z "${line}" ]]; then
    printf '%s\n' "${default_value}"
    return 0
  fi

  value="${line#*=}"
  value="$(printf '%s' "${value}" | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//')"

  value="${value%\"}"
  value="${value#\"}"
  value="${value%\'}"
  value="${value#\'}"

  printf '%s\n' "${value}"
}

should_start_app() {
  local value
  value="$(get_config_value "start_on_boot" "true")"
  value="$(printf '%s' "${value}" | tr -d '[:space:]' | tr '[:upper:]' '[:lower:]')"

  case "${value}" in
    1|true|yes|on) return 0 ;;
    0|false|no|off) return 1 ;;
    *)
      log "Invalid ${BOOT_CONFIG_NAME} start_on_boot='${value}', defaulting to start"
      return 0
      ;;
  esac
}

resolve_patch_path() {
  local configured_patch="$1"
  local source_root="$2"

  [[ -n "${configured_patch}" ]] || return 1

  if [[ "${configured_patch}" = /* ]]; then
    [[ -f "${configured_patch}" ]] && { printf '%s\n' "${configured_patch}"; return 0; }
    return 1
  fi

  if [[ -n "${source_root}" && -f "${source_root}/${configured_patch}" ]]; then
    printf '%s\n' "${source_root}/${configured_patch}"
    return 0
  fi

  if [[ -f "${LOCAL_PROJECT_ROOT}/${configured_patch}" ]]; then
    printf '%s\n' "${LOCAL_PROJECT_ROOT}/${configured_patch}"
    return 0
  fi

  return 1
}

resolve_project_patch() {
  local project_name="$1"
  local source_root="$2"

  [[ -n "${project_name}" ]] || return 1
  resolve_patch_path "patches/${project_name}.patch" "${source_root}"
}

prepare_candidate() {
  local source_dir="$1"
  local candidate_dir="$2"

  mkdir -p "${candidate_dir}"

  if command -v rsync >/dev/null 2>&1; then
    rsync -a --delete "${source_dir}/" "${candidate_dir}/"
    return 0
  fi

  cp -a "${source_dir}/." "${candidate_dir}/"
}

apply_patch_to_candidate() {
  local candidate_dir="$1"
  local patch_file="$2"

  if command -v git >/dev/null 2>&1; then
    if (cd "${candidate_dir}" && git apply --whitespace=nowarn "${patch_file}"); then
      return 0
    fi
  fi

  if command -v patch >/dev/null 2>&1; then
    (cd "${candidate_dir}" && patch -p1 --forward < "${patch_file}")
    return $?
  fi

  log "Failed to apply patch: neither git nor patch command is available"
  return 1
}

candidate_differs() {
  local candidate_dir="$1"
  local destination_dir="$2"

  if [[ ! -d "${destination_dir}" ]]; then
    return 0
  fi

  if command -v rsync >/dev/null 2>&1; then
    if rsync -ain --delete "${candidate_dir}/" "${destination_dir}/" | grep -q .; then
      return 0
    fi
    return 1
  fi

  if diff -qr "${candidate_dir}" "${destination_dir}" >/dev/null 2>&1; then
    return 1
  fi

  return 0
}

deploy_candidate() {
  local candidate_dir="$1"
  local destination_dir="$2"

  mkdir -p "$(dirname "${destination_dir}")"

  if command -v rsync >/dev/null 2>&1; then
    rsync -a --delete "${candidate_dir}/" "${destination_dir}/"
    return 0
  fi

  rm -rf "${destination_dir}"
  mkdir -p "${destination_dir}"
  cp -a "${candidate_dir}/." "${destination_dir}/"
}

mkdir -p "${LOCAL_PROJECT_ROOT}" "${TEMP_MOUNT_BASE}"

CONFIGURED_USB_LABEL="$(get_config_value "usb_label" "")"
CONFIGURED_MAPPER_FOLDER_NAME="$(get_config_value "mapper_folder_name" "raspberryPi-video-mapper-main")"
CONFIGURED_VIDEOS_PATH="$(get_config_value "videos_path" "videos")"
USB_RELATIVE_PATH="${CONFIGURED_MAPPER_FOLDER_NAME}/mapper"

if [[ -n "${CONFIGURED_USB_LABEL}" ]]; then
  log "Using configured USB label filter: ${CONFIGURED_USB_LABEL}"
fi
log "Using configured mapper folder name: ${CONFIGURED_MAPPER_FOLDER_NAME}"
log "Using configured videos path: ${CONFIGURED_VIDEOS_PATH}"

log "Searching USB drives for ${USB_RELATIVE_PATH}"
source_dir=""
source_root=""
attempts=$((BOOT_WAIT_SECONDS / BOOT_WAIT_INTERVAL))

for ((i=0; i<=attempts; i++)); do
  if source_dir="$(find_source_dir)"; then
    break
  fi
  sleep "${BOOT_WAIT_INTERVAL}"
done

if [[ -n "${source_dir}" ]]; then
  source_root="$(dirname "${source_dir}")"
  log "Found project on USB: ${source_dir}"

  if [[ -f "${source_root}/${BOOT_CONFIG_NAME}" ]]; then
    if copy_if_changed "${source_root}/${BOOT_CONFIG_NAME}" "${LOCAL_BOOT_CONFIG}"; then
      log "Updated ${BOOT_CONFIG_NAME}"
    fi
  elif [[ -f "${source_root}/${LEGACY_BOOT_CONFIG_NAME}" ]]; then
    if copy_if_changed "${source_root}/${LEGACY_BOOT_CONFIG_NAME}" "${LOCAL_BOOT_CONFIG}"; then
      log "Migrated ${LEGACY_BOOT_CONFIG_NAME} -> ${BOOT_CONFIG_NAME}"
    fi
  fi
else
  if [[ -d "${LOCAL_SOURCE_DIR}" ]]; then
    source_dir="${LOCAL_SOURCE_DIR}"
    source_root="${LOCAL_PROJECT_ROOT}"
    log "No USB project found, using local source copy: ${LOCAL_SOURCE_DIR}"
  else
    log "No USB project found, using local copy if available"
  fi
fi

configured_project="$(get_config_value "project" "")"
configured_patch="$(get_config_value "project_patch" "")"
patch_file=""
if [[ -n "${configured_project}" ]]; then
  if [[ -n "${source_root}" ]]; then
    if patch_file="$(resolve_project_patch "${configured_project}" "${source_root}")"; then
      log "Project selected: ${configured_project}"
    else
      log "Configured project patch not found: patches/${configured_project}.patch"
      exit 1
    fi
  else
    log "Configured project '${configured_project}' will be applied when a source copy is available"
  fi
elif [[ -n "${configured_patch}" ]]; then
  if [[ -n "${source_root}" ]]; then
    if patch_file="$(resolve_patch_path "${configured_patch}" "${source_root}")"; then
      log "Legacy project patch selected: ${configured_patch}"
    else
      log "Configured patch not found: ${configured_patch}"
      exit 1
    fi
  else
    log "Configured patch '${configured_patch}' will be applied when a source copy is available"
  fi
fi

updated=0
if [[ -n "${source_dir}" ]]; then
  if [[ "${source_dir}" != "${LOCAL_SOURCE_DIR}" ]]; then
    if candidate_differs "${source_dir}" "${LOCAL_SOURCE_DIR}"; then
      log "Updating local source copy at ${LOCAL_SOURCE_DIR}"
      deploy_candidate "${source_dir}" "${LOCAL_SOURCE_DIR}"
    else
      log "Local source copy is already up to date"
    fi
  fi

  TEMP_CANDIDATE_DIR="$(mktemp -d /run/mapper-candidate.XXXXXX)"
  prepare_candidate "${source_dir}" "${TEMP_CANDIDATE_DIR}"

  if [[ -n "${patch_file}" ]]; then
    log "Applying patch: ${patch_file}"
    apply_patch_to_candidate "${TEMP_CANDIDATE_DIR}" "${patch_file}"
  fi

  if candidate_differs "${TEMP_CANDIDATE_DIR}" "${LOCAL_MAPPER_DIR}"; then
    log "Deploying updated mapper project to ${LOCAL_MAPPER_DIR}"
    deploy_candidate "${TEMP_CANDIDATE_DIR}" "${LOCAL_MAPPER_DIR}"
    updated=1
  else
    log "Local mapper project is already up to date"
  fi
fi

if [[ ! -d "${LOCAL_SOURCE_DIR}" ]]; then
  log "No local source copy at ${LOCAL_SOURCE_DIR}; cannot continue"
  exit 1
fi

if [[ ! -d "${LOCAL_MAPPER_DIR}" ]]; then
  log "No local deployed project at ${LOCAL_MAPPER_DIR}; it will be created from source on next successful sync"
  exit 1
fi

if [[ "${updated}" -eq 1 || ! -x "${LOCAL_MAPPER_DIR}/${BINARY_NAME}" ]]; then
  log "Building project with make -j"
  (
    cd "${LOCAL_MAPPER_DIR}"
    make -j
  )
fi

if ! should_start_app; then
  log "${BOOT_CONFIG_NAME} disables startup; skipping launch"
  exit 0
fi

log "Launching ${BINARY_NAME}"
(
  cd "${LOCAL_MAPPER_DIR}"
  if [[ -n "${CONFIGURED_USB_LABEL}" ]]; then
    SDL_VIDEODRIVER=kmsdrm MAPPER_USB_LABEL="${CONFIGURED_USB_LABEL}" MAPPER_FOLDER_NAME="${CONFIGURED_MAPPER_FOLDER_NAME}" MAPPER_VIDEOS_PATH="${CONFIGURED_VIDEOS_PATH}" "./${BINARY_NAME}"
  else
    SDL_VIDEODRIVER=kmsdrm MAPPER_FOLDER_NAME="${CONFIGURED_MAPPER_FOLDER_NAME}" MAPPER_VIDEOS_PATH="${CONFIGURED_VIDEOS_PATH}" "./${BINARY_NAME}"
  fi
)
app_rc=$?
log "${BINARY_NAME} exited with code ${app_rc}"
exit "${app_rc}"
EOS

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
