#!/bin/bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
test_root="$(mktemp -d "${TMPDIR:-/tmp}/dell-e525w-test.XXXXXX")"

cleanup() {
  find "$test_root" -type f -delete
  find "$test_root" -depth -type d -exec rmdir {} \; 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

DELL_SCAN_SOURCE_DIR="$repo_root" DELL_SCAN_PREFIX="$test_root/prefix" \
  bash "$repo_root/install.sh"

scanner="$test_root/prefix/bin/dell-scan"
converter="$test_root/prefix/libexec/dell-e525w-scanner-fix/pgm-to-pdf"
"$scanner" --version | grep -E '^dell-scan [0-9]+\.[0-9]+\.[0-9]+$'
"$scanner" --help >/dev/null

if "$scanner" --wifi --ip 999.1.1.1 >"$test_root/invalid.out" \
  2>"$test_root/invalid.err"; then
  echo 'invalid IPv4 address unexpectedly passed' >&2
  exit 1
fi
grep -F 'invalid printer IPv4 address: 999.1.1.1' "$test_root/invalid.err"

printf 'P5\n4 4\n255\n\x00\x22\x44\x66\x22\x44\x66\x88\x44\x66\x88\xaa\x66\x88\xaa\xff' \
  >"$test_root/fixture.pgm"
"$converter" "$test_root/fixture.pdf" "$test_root/fixture.pgm"
file "$test_root/fixture.pdf" | grep -F 'PDF document'

if command -v qpdf >/dev/null; then
  qpdf --check "$test_root/fixture.pdf" >/dev/null
  [[ "$(qpdf --show-npages "$test_root/fixture.pdf")" == '1' ]]
fi

echo 'tests=passed'
