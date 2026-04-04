#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root_win="$(wslpath -w "$repo_root")"
build_script_win="${repo_root_win}\\\\build.ps1"

args=("$@")
if [ "${#args[@]}" -eq 0 ]; then
  args=("-Static")
fi

exec powershell.exe -NoProfile -ExecutionPolicy Bypass -File \
  "$build_script_win" "${args[@]}"
