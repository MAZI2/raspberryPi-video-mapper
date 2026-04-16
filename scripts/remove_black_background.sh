#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  remove_black_background.sh INPUT_VIDEO OUTPUT_VIDEO [options]
  remove_black_background.sh INPUT_DIR OUTPUT_DIR [options]

Options:
  --similarity N      Key similarity for colorkey (default: 0.10)
  --blend N           Edge softness for colorkey (default: 0.02)
  --key-color HEX     Key color in hex, e.g. 0x000000 (default: 0x000000)
  --codec NAME        prores | vp9 | qtrle | h264 (default: prores)
  --crf N             CRF for vp9 codec (default: 18)
  --h264-crf N        CRF for h264/mp4 codec (default: 18)
  --mask-output PATH  Also export alpha mask video as grayscale mp4
  --ext EXT           Output extension in directory mode (default: mov)
  -h, --help          Show this help

Examples:
  ./scripts/remove_black_background.sh in.mp4 out.mov
  ./scripts/remove_black_background.sh in.mp4 out.mov --similarity 0.15 --blend 0.04
  ./scripts/remove_black_background.sh in.mp4 out.webm --codec vp9 --mask-output mask.mp4
  ./scripts/remove_black_background.sh in.mp4 out.mp4 --codec h264
  ./scripts/remove_black_background.sh videos/freund videos_converted/freund

Notes:
  - MP4/H.264 does not carry alpha reliably.
  - This is color keying: best results require a clean, evenly lit black background.
EOF
}

if [[ $# -lt 2 ]]; then
  usage
  exit 1
fi

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "Error: ffmpeg is required but not found in PATH."
  exit 1
fi

INPUT="$1"
OUTPUT="$2"
shift 2

INPUT="${INPUT%/}"
OUTPUT="${OUTPUT%/}"

SIMILARITY="0.10"
BLEND="0.02"
KEY_COLOR="0x000000"
CODEC="prores"
CRF="18"
H264_CRF="18"
MASK_OUTPUT=""
OUT_EXT="mov"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --similarity)
      SIMILARITY="${2:-}"
      shift 2
      ;;
    --blend)
      BLEND="${2:-}"
      shift 2
      ;;
    --key-color)
      KEY_COLOR="${2:-}"
      shift 2
      ;;
    --codec)
      CODEC="${2:-}"
      shift 2
      ;;
    --crf)
      CRF="${2:-}"
      shift 2
      ;;
    --h264-crf)
      H264_CRF="${2:-}"
      shift 2
      ;;
    --mask-output)
      MASK_OUTPUT="${2:-}"
      shift 2
      ;;
    --ext)
      OUT_EXT="${2:-}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1"
      usage
      exit 1
      ;;
  esac
done

FILTER="colorkey=${KEY_COLOR}:${SIMILARITY}:${BLEND}"

abs_path() {
  local p="$1"
  if [[ -d "$p" ]]; then
    (cd "$p" && pwd -P)
  else
    local d
    d="$(dirname "$p")"
    local b
    b="$(basename "$p")"
    (cd "$d" && printf '%s/%s\n' "$(pwd -P)" "$b")
  fi
}

is_video_ext() {
  local p
  p="$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')"
  [[ "$p" == *.mp4 || "$p" == *.mov || "$p" == *.mkv || "$p" == *.m4v || "$p" == *.ts ]]
}

convert_one() {
  local in_file="$1"
  local out_file="$2"
  local mask_file="$3"

  if [[ ! -f "$in_file" ]]; then
    echo "Error: input file not found: $in_file"
    return 1
  fi

  mkdir -p "$(dirname "$out_file")"

  case "$CODEC" in
    prores)
      ffmpeg -nostdin -y -i "$in_file" -vf "${FILTER},format=yuva444p10le" -c:v prores_ks -profile:v 4444 -pix_fmt yuva444p10le -an "$out_file"
      ;;
    vp9)
      ffmpeg -nostdin -y -i "$in_file" -vf "${FILTER},format=yuva420p" -c:v libvpx-vp9 -pix_fmt yuva420p -b:v 0 -crf "$CRF" -an "$out_file"
      ;;
    qtrle)
      ffmpeg -nostdin -y -i "$in_file" -vf "${FILTER},format=argb" -c:v qtrle -pix_fmt argb -an "$out_file"
      ;;
    h264)
      ffmpeg -nostdin -y -i "$in_file" -vf "format=rgba,${FILTER},format=yuv420p" -c:v libx264 -pix_fmt yuv420p -crf "$H264_CRF" -an "$out_file"
      echo "Warning: h264/mp4 output does not preserve transparency."
      ;;
    *)
      echo "Error: unsupported codec '$CODEC' (use: prores | vp9 | qtrle | h264)"
      exit 1
      ;;
  esac

  if [[ -n "$mask_file" ]]; then
    mkdir -p "$(dirname "$mask_file")"
    ffmpeg -nostdin -y -i "$in_file" -vf "${FILTER},alphaextract,format=gray" -c:v libx264 -pix_fmt yuv420p -crf 18 -an "$mask_file"
  fi
}

if [[ -f "$INPUT" ]]; then
  convert_one "$INPUT" "$OUTPUT" "$MASK_OUTPUT"
  echo "Done."
  echo "  Output: $OUTPUT"
  if [[ -n "$MASK_OUTPUT" ]]; then
    echo "  Mask:   $MASK_OUTPUT"
  fi
  exit 0
fi

if [[ -d "$INPUT" ]]; then
  INPUT_ABS="$(abs_path "$INPUT")"
  mkdir -p "$OUTPUT"
  OUTPUT_ABS="$(abs_path "$OUTPUT")"
  converted=0

  while IFS= read -r -d '' in_file; do
    is_video_ext "$in_file" || continue

    rel_path="${in_file#"$INPUT_ABS"/}"
    rel_dir="$(dirname "$rel_path")"
    base_name="$(basename "$in_file")"
    stem="${base_name%.*}"
    out_file="${OUTPUT_ABS}/${rel_dir}/${stem}.${OUT_EXT}"

    if [[ -n "$MASK_OUTPUT" ]]; then
      mask_file="${OUTPUT_ABS}/${rel_dir}/${stem}_mask.mp4"
    else
      mask_file=""
    fi

    echo "Converting: $in_file -> $out_file"
    convert_one "$in_file" "$out_file" "$mask_file"
    converted=$((converted + 1))
  done < <(find "$INPUT_ABS" -type f -print0)

  echo "Done."
  echo "  Converted files: $converted"
  echo "  Output dir:      $OUTPUT_ABS"
  exit 0
fi

echo "Error: input path not found: $INPUT"
exit 1
