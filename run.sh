#!/usr/bin/env bash
set -euo pipefail

list_usb_mounts() {
  lsblk -rno TRAN,MOUNTPOINT | awk '
    $1=="usb" {
      $1=""
      sub(/^ /, "", $0)
      if (length($0) > 0) {
        print $0
      }
    }
  '
}

find_usb_videos_dir() {
  local mp
  while IFS= read -r mp; do
    [[ -n "${mp}" ]] || continue
    if [[ -d "${mp}/videos" ]]; then
      printf '%s\n' "${mp}/videos"
      return 0
    fi
    if [[ -d "${mp}/raspberryPi-video-mapper/videos" ]]; then
      printf '%s\n' "${mp}/raspberryPi-video-mapper/videos"
      return 0
    fi
  done < <(list_usb_mounts)

  return 1
}

PATCH="$(awk -F= '/^[[:space:]]*project_patch[[:space:]]*=/{v=$2} END{gsub(/^[[:space:]]+|[[:space:]]+$/,"",v); gsub(/^["'"'"']|["'"'"']$/,"",v); print v}' configure.conf)"
USB_VIDEOS="$(find_usb_videos_dir || true)"
if [[ -z "${USB_VIDEOS}" ]]; then
  echo "USB videos folder not found."
  echo "Checked USB mount points:"
  list_usb_mounts | sed 's/^/  - /' || true
  echo "Expected either:"
  echo "  <mount>/videos"
  echo "  <mount>/raspberryPi-video-mapper/videos"
  exit 1
fi

BUILD_DIR=/tmp/mapper-run
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
rsync -a --delete mapper/ "$BUILD_DIR/"

if [ -n "$PATCH" ]; then
  PATCH_FILE="$PATCH"
  [ "${PATCH_FILE#/}" = "$PATCH_FILE" ] && PATCH_FILE="$PWD/$PATCH_FILE"
  patch -d "$BUILD_DIR" -p1 < "$PATCH_FILE"
fi

(
  cd "$BUILD_DIR"
  make -j
  SDL_VIDEODRIVER=kmsdrm MAPPER_MEDIA_ROOT="$USB_VIDEOS" ./mapping_video_keystone
)
