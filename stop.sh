#!/usr/bin/env bash
set -euo pipefail

SERVICE_NAME="mapper_boot_mapper.service"
PROCESS_NAME="mapping_video_keystone"
WAIT_SECONDS=8

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

service_active() {
  run_privileged systemctl is-active --quiet "${SERVICE_NAME}"
}

process_active() {
  run_privileged pgrep -x "${PROCESS_NAME}" >/dev/null 2>&1
}

printf 'Stopping %s...\n' "${SERVICE_NAME}"
run_privileged systemctl stop --no-block "${SERVICE_NAME}" >/dev/null 2>&1 || true
run_privileged systemctl kill --signal=SIGTERM --kill-who=all "${SERVICE_NAME}" >/dev/null 2>&1 || true

if process_active; then
  printf 'Sending SIGTERM to %s...\n' "${PROCESS_NAME}"
  run_privileged pkill -TERM -x "${PROCESS_NAME}" >/dev/null 2>&1 || true
fi

for ((i=0; i<WAIT_SECONDS; i++)); do
  if ! service_active && ! process_active; then
    printf 'Mapper stopped cleanly.\n'
    run_privileged systemctl reset-failed "${SERVICE_NAME}" >/dev/null 2>&1 || true
    exit 0
  fi
  sleep 1
done

if process_active; then
  printf 'Force killing %s...\n' "${PROCESS_NAME}"
  run_privileged pkill -KILL -x "${PROCESS_NAME}" >/dev/null 2>&1 || true
fi

run_privileged systemctl kill --signal=SIGKILL --kill-who=all "${SERVICE_NAME}" >/dev/null 2>&1 || true
run_privileged systemctl stop --no-block "${SERVICE_NAME}" >/dev/null 2>&1 || true
run_privileged systemctl reset-failed "${SERVICE_NAME}" >/dev/null 2>&1 || true

printf 'Service state:\n'
run_privileged systemctl status "${SERVICE_NAME}" --no-pager -l || true
