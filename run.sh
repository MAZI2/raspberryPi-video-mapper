#!/usr/bin/env bash
set -euo pipefail

PATCH="$(awk -F= '/^[[:space:]]*project_patch[[:space:]]*=/{v=$2} END{gsub(/^[[:space:]]+|[[:space:]]+$/,"",v); gsub(/^["'"'"']|["'"'"']$/,"",v); print v}' configure.conf)"
USB_VIDEOS="$(lsblk -rno TRAN,MOUNTPOINT | awk '$1=="usb" && $2!=""{print $2}' | while read -r mp; do [ -d "$mp/raspberryPi-video-mapper/videos" ] && { echo "$mp/raspberryPi-video-mapper/videos"; break; }; done)"
[ -n "$USB_VIDEOS" ] || { echo "USB videos folder not found"; exit 1; }

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
