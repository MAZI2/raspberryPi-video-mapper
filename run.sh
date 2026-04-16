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

PATCH="$(get_config_value "project_patch" "")"
USB_LABEL="$(get_config_value "usb_label" "")"

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
  if [[ -n "${USB_LABEL}" ]]; then
    SDL_VIDEODRIVER=kmsdrm MAPPER_USB_LABEL="${USB_LABEL}" ./mapping_video_keystone
  else
    SDL_VIDEODRIVER=kmsdrm ./mapping_video_keystone
  fi
)
