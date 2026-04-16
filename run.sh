#!/usr/bin/env bash
set -euo pipefail

get_config_value() {
  local key="$1"
  local default_value="$2"
  local line value

  line="$(awk -F= -v key="$key" '
    $0 ~ "^[[:space:]]*" key "[[:space:]]*=" {
      v=$2
    }
    END {
      if (v == "") exit 1
      print v
    }
  ' configure.conf 2>/dev/null || true)"

  if [[ -z "${line}" ]]; then
    printf '%s\n' "${default_value}"
    return 0
  fi

  value="$(printf '%s' "${line}" | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//')"
  value="${value%\"}"
  value="${value#\"}"
  value="${value%\'}"
  value="${value#\'}"
  printf '%s\n' "${value}"
}

find_videos_under_root() {
  local root="$1"

  if [[ -d "$root/videos" ]]; then
    printf '%s\n' "$root/videos"
    return 0
  fi

  if [[ -d "$root/raspberryPi-video-mapper/videos" ]]; then
    printf '%s\n' "$root/raspberryPi-video-mapper/videos"
    return 0
  fi

  return 1
}

find_mount_for_device() {
  local device="$1"

  if command -v findmnt >/dev/null 2>&1; then
    findmnt -nr -S "$device" -o TARGET 2>/dev/null | head -n 1
    return 0
  fi

  lsblk -nrpo NAME,MOUNTPOINT "$device" 2>/dev/null | awk -v dev="$device" '
    $1 == dev && $2 != "" { print $2; exit }
  '
}

resolve_usb_videos_root() {
  local label="$1"
  local device=""
  local mount_point=""
  local mount_name=""

  [[ -z "$label" ]] && return 1
  [[ -e "/dev/disk/by-label/$label" ]] || return 1

  device="$(readlink -f "/dev/disk/by-label/$label" 2>/dev/null || true)"
  [[ -n "$device" ]] || return 1

  mount_point="$(find_mount_for_device "$device")"
  if [[ -z "$mount_point" ]]; then
    mount_name="$(basename "$device")"
    mount_point="/run/mapper-usb/$mount_name"
    mkdir -p "$mount_point"
    mount -o ro "$device" "$mount_point" 2>/dev/null || mount "$device" "$mount_point" 2>/dev/null || true
    mount_point="$(find_mount_for_device "$device")"
  fi

  [[ -n "$mount_point" ]] || return 1
  find_videos_under_root "$mount_point"
}

sync_video_cache() {
  local source_dir="$1"
  local cache_dir="$2"
  local sync_log=""
  local changed_count=""

  mkdir -p "$cache_dir"
  sync_log="$(mktemp)"

  printf 'Syncing video cache\n'
  printf '  Source: %s\n' "$source_dir"
  printf '  Cache:  %s\n' "$cache_dir"

  rsync \
    -a \
    --delete \
    --checksum \
    --itemize-changes \
    --info=progress2 \
    --out-format='%i %n%L' \
    "$source_dir"/ "$cache_dir"/ | tee "$sync_log"

  changed_count="$(sed '/^[[:space:]]*$/d' "$sync_log" | wc -l | tr -d ' ')"
  rm -f "$sync_log"

  if [[ "$changed_count" == "0" ]]; then
    printf 'Video cache is already up to date: %s\n' "$cache_dir"
  else
    printf 'Updated local video cache (%s change(s)): %s\n' "$changed_count" "$cache_dir"
  fi
}

PATCH="$(get_config_value "project_patch" "")"
USB_LABEL="$(get_config_value "usb_label" "")"
VIDEO_CACHE_DIR="$PWD/.cache/videos"
VIDEO_SOURCE_DIR=""
MEDIA_ROOT=""

if [[ -n "$USB_LABEL" ]]; then
  VIDEO_SOURCE_DIR="$(resolve_usb_videos_root "$USB_LABEL" || true)"
fi

if [[ -n "$VIDEO_SOURCE_DIR" ]]; then
  sync_video_cache "$VIDEO_SOURCE_DIR" "$VIDEO_CACHE_DIR"
  MEDIA_ROOT="$VIDEO_CACHE_DIR"
elif [[ -d "$VIDEO_CACHE_DIR" ]]; then
  printf 'USB videos source not available, using cached videos: %s\n' "$VIDEO_CACHE_DIR"
  MEDIA_ROOT="$VIDEO_CACHE_DIR"
fi

BUILD_DIR=/tmp/mapper-run
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
rsync -a --delete mapper/ "$BUILD_DIR/"

if [[ -n "$PATCH" ]]; then
  PATCH_FILE="$PATCH"
  [[ "${PATCH_FILE#/}" = "$PATCH_FILE" ]] && PATCH_FILE="$PWD/$PATCH_FILE"
  patch -d "$BUILD_DIR" -p1 < "$PATCH_FILE"
fi

(
  cd "$BUILD_DIR"
  make -j
  if [[ -n "${MEDIA_ROOT}" ]]; then
    SDL_VIDEODRIVER=kmsdrm MAPPER_MEDIA_ROOT="${MEDIA_ROOT}" ./mapping_video_keystone
  elif [[ -n "${USB_LABEL}" ]]; then
    SDL_VIDEODRIVER=kmsdrm MAPPER_USB_LABEL="${USB_LABEL}" ./mapping_video_keystone
  else
    SDL_VIDEODRIVER=kmsdrm ./mapping_video_keystone
  fi
)
