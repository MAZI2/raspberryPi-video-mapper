#!/usr/bin/env bash
set -euo pipefail

SERVICE_NAME="mapper_boot_mapper.service"
WAIT_SECONDS=10

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

service_is_running() {
  local state
  state="$(run_privileged systemctl show -p ActiveState --value "${SERVICE_NAME}" 2>/dev/null || true)"
  [[ "${state}" == "active" || "${state}" == "activating" || "${state}" == "deactivating" ]]
}

mapper_pids() {
  run_privileged pgrep -f 'mapping_video_keystone|mapper_boot_runner\.sh' 2>/dev/null || true
}

print_remaining() {
  local pids
  pids="$(mapper_pids)"
  if [[ -n "${pids}" ]]; then
    printf 'Remaining mapper-related processes:\n'
    run_privileged ps -fp ${pids} || true
  fi
}

printf 'Stopping %s...\n' "${SERVICE_NAME}"
run_privileged systemctl stop --no-block "${SERVICE_NAME}" >/dev/null 2>&1 || true
run_privileged systemctl kill --kill-who=all --signal=SIGTERM "${SERVICE_NAME}" >/dev/null 2>&1 || true

if [[ -n "$(mapper_pids)" ]]; then
  printf 'Sending SIGTERM to mapper processes...\n'
  run_privileged pkill -TERM -f 'mapping_video_keystone|mapper_boot_runner\.sh' >/dev/null 2>&1 || true
fi

for ((i=0; i<WAIT_SECONDS; i++)); do
  if ! service_is_running && [[ -z "$(mapper_pids)" ]]; then
    printf 'Mapper stopped cleanly.\n'
    run_privileged systemctl reset-failed "${SERVICE_NAME}" >/dev/null 2>&1 || true
    exit 0
  fi
  sleep 1
done

if [[ -n "$(mapper_pids)" ]]; then
  printf 'Force killing remaining mapper processes...\n'
  run_privileged pkill -KILL -f 'mapping_video_keystone|mapper_boot_runner\.sh' >/dev/null 2>&1 || true
fi

run_privileged systemctl kill --kill-who=all --signal=SIGKILL "${SERVICE_NAME}" >/dev/null 2>&1 || true
run_privileged systemctl stop --no-block "${SERVICE_NAME}" >/dev/null 2>&1 || true
run_privileged systemctl reset-failed "${SERVICE_NAME}" >/dev/null 2>&1 || true

sleep 1

if service_is_running || [[ -n "$(mapper_pids)" ]]; then
  printf 'Mapper still appears to be running.\n' >&2
  print_remaining
  printf 'Service state:\n' >&2
  run_privileged systemctl status "${SERVICE_NAME}" --no-pager -l || true
  exit 1
fi

printf 'Mapper stopped after force kill.\n'
