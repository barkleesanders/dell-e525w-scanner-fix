#!/bin/bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
check_root="$(mktemp -d "${TMPDIR:-/tmp}/dell-e525w-check.XXXXXX")"

cleanup() {
  find "$check_root" -type f -delete
  rmdir "$check_root" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

compile_flags=(-std=c17 -Wall -Wextra -Wpedantic -Werror -O2)
c_sources=(
  src/dell_scan_engine.c
  src/dell_wifi_bridge.c
  research/dell_swlld_probe.c
  research/dell_network_transport_probe.c
)

for relative_source in "${c_sources[@]}"; do
  output_name="$(basename "${relative_source%.c}")"
  clang "${compile_flags[@]}" "$repo_root/$relative_source" -o "$check_root/$output_name"
  analyzer_output="$(clang --analyze -std=c17 -Wall -Wextra -Wpedantic -Werror \
    "$repo_root/$relative_source" -o /dev/null 2>&1)"
  [[ -z "$analyzer_output" ]] || {
    printf '%s\n' "$analyzer_output" >&2
    exit 1
  }
done

clang "${compile_flags[@]}" "$repo_root/src/pgm_to_pdf.c" \
  -framework ApplicationServices -o "$check_root/pgm-to-pdf"
analyzer_output="$(clang --analyze -std=c17 -Wall -Wextra -Wpedantic -Werror \
  "$repo_root/src/pgm_to_pdf.c" -o /dev/null 2>&1)"
[[ -z "$analyzer_output" ]] || {
  printf '%s\n' "$analyzer_output" >&2
  exit 1
}

clang "${compile_flags[@]}" "$repo_root/tests/row_wrap_test.c" \
  -o "$check_root/row-wrap-test"
"$check_root/row-wrap-test"

shell_scripts=(bin/dell-scan install.sh scripts/check.sh tests/test.sh)
for relative_script in "${shell_scripts[@]}"; do
  bash -n "$repo_root/$relative_script"
done

if command -v shellcheck >/dev/null; then
  shellcheck "${shell_scripts[@]/#/$repo_root/}"
else
  echo 'warning: shellcheck is unavailable; syntax checks still ran' >&2
fi

bash "$repo_root/tests/test.sh"
echo 'checks=passed'
