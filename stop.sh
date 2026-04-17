#!/usr/bin/env bash
set -euo pipefail

SERVICE_NAME="mapper_boot_mapper.service"
PROCESS_NAME="mapping_video_keystone"

run_privileged() {
  if [[ "${EUID}" -eq 0 ]]; then
    "$@"
    return
  fi

  if command -v sudo >/dev/null 2>&1; then
    sudo "$@"
    return
  fi

  printf 'This command needs root privileges. Run with sudo.\n' >&2
  exit 1
}

printf 'Stopping %s...\n' "${SERVICE_NAME}"
run_privileged systemctl stop "${SERVICE_NAME}" 2>/dev/null || true

if run_privileged pgrep -x "${PROCESS_NAME}" >/dev/null 2>&1; then
  printf 'Killing remaining %s process(es)...\n' "${PROCESS_NAME}"
  run_privileged pkill -x "${PROCESS_NAME}" || true
fi

printf 'Service state:\n'
run_privileged systemctl status "${SERVICE_NAME}" --no-pager -l || true
