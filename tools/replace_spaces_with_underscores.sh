#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  ./scripts/replace_spaces_with_underscores.sh <target_dir> [--dry-run]

Description:
  Recursively renames files and directories by replacing spaces with "_".
  Processes paths from deepest to shallowest to handle nested directories safely.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

TARGET_DIR="${1:-}"
DRY_RUN="${2:-}"

if [[ -z "$TARGET_DIR" ]]; then
  usage
  exit 1
fi

if [[ ! -d "$TARGET_DIR" ]]; then
  echo "Error: target directory not found: $TARGET_DIR" >&2
  exit 1
fi

if [[ -n "$DRY_RUN" && "$DRY_RUN" != "--dry-run" ]]; then
  echo "Error: unknown argument: $DRY_RUN" >&2
  usage
  exit 1
fi

renamed=0
skipped=0

while IFS= read -r -d '' path; do
  dir_path="$(dirname "$path")"
  base_name="$(basename "$path")"
  new_base_name="$(printf '%s' "$base_name" | tr ' ' '_')"
  new_path="${dir_path}/${new_base_name}"

  [[ "$path" == "$new_path" ]] && continue

  if [[ -e "$new_path" ]]; then
    echo "Skipping (target exists): $path -> $new_path"
    skipped=$((skipped + 1))
    continue
  fi

  if [[ "$DRY_RUN" == "--dry-run" ]]; then
    echo "Would rename: $path -> $new_path"
  else
    mv "$path" "$new_path"
    echo "Renamed: $path -> $new_path"
  fi
  renamed=$((renamed + 1))
done < <(find "$TARGET_DIR" -depth -name '* *' -print0)

if [[ "$DRY_RUN" == "--dry-run" ]]; then
  echo "Dry run complete. Paths that would be renamed: $renamed. Skipped: $skipped."
else
  echo "Done. Renamed: $renamed. Skipped: $skipped."
fi
