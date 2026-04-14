#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root_win="$(wslpath -w "$repo_root")"
package_script_win="${repo_root_win}\\\\package_windows.ps1"

exec powershell.exe -NoProfile -ExecutionPolicy Bypass -File \
  "$package_script_win" "$@"
