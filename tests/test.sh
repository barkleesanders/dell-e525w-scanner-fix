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

printf 'P5\n4 4\n255\n\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff' \
  >"$test_root/blank-back.pgm"
printf 'P5\n4 4\n255\n\xff\x00\xff\x00\xff\x00\xff\x00\xff\x00\xff\x00\xff\x00\xff\x00' \
  >"$test_root/second-front.pgm"
"$converter" --drop-blank-backs "$test_root/filtered.pdf" "$test_root/fixture.pgm" \
  "$test_root/blank-back.pgm" "$test_root/second-front.pgm" >"$test_root/filtered.out"
grep -F 'pdf_pages=2' "$test_root/filtered.out"
grep -F 'blank_backs_dropped=1' "$test_root/filtered.out"

mock_bin="$test_root/mock-bin"
mkdir "$mock_bin"
cat >"$mock_bin/ioreg" <<'EOF'
#!/bin/bash
printf '%s\n' '"locationID" = 123'
EOF
chmod +x "$mock_bin/ioreg"

engine="$test_root/prefix/libexec/dell-e525w-scanner-fix/dell-scan-engine"
cat >"$engine" <<'EOF'
#!/bin/bash
set -euo pipefail
attempt=0
if [[ -f "$DELL_TEST_ATTEMPT_FILE" ]]; then
  attempt="$(<"$DELL_TEST_ATTEMPT_FILE")"
fi
attempt=$((attempt + 1))
printf '%s\n' "$attempt" >"$DELL_TEST_ATTEMPT_FILE"
if (( attempt == 1 )); then
  exit 10
fi
[[ "$2" == 'usb' && "$3" == '123' && "$5" == '100' ]]
printf 'P5\n4 4\n255\n\x00\x22\x44\x66\x22\x44\x66\x88\x44\x66\x88\xaa\x66\x88\xaa\xff' \
  >"$4-page-001.pgm"
printf 'P5\n4 4\n255\n\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff' \
  >"$4-page-002.pgm"
EOF
chmod +x "$engine"
printf 'test driver\n' >"$test_root/fake-driver.dylib"
PATH="$mock_bin:$PATH" DELL_SCAN_DRIVER="$test_root/fake-driver.dylib" \
  DELL_SCAN_RETRY_DELAY=0 DELL_TEST_ATTEMPT_FILE="$test_root/attempt-count" \
  "$scanner" --usb --output "$test_root/retry.pdf" >"$test_root/retry.out" \
  2>"$test_root/retry.err"
grep -F 'retrying in 0 seconds (1/3)' "$test_root/retry.err"
grep -F 'pdf_pages=1' "$test_root/retry.out"
grep -F 'blank_backs_dropped=1' "$test_root/retry.out"
grep -F 'Raw sides captured: 2' "$test_root/retry.out"
[[ "$(<"$test_root/attempt-count")" == '2' ]]

if command -v qpdf >/dev/null; then
  qpdf --check "$test_root/fixture.pdf" >/dev/null
  [[ "$(qpdf --show-npages "$test_root/fixture.pdf")" == '1' ]]
  qpdf --check "$test_root/filtered.pdf" >/dev/null
  [[ "$(qpdf --show-npages "$test_root/filtered.pdf")" == '2' ]]
  qpdf --check "$test_root/retry.pdf" >/dev/null
  [[ "$(qpdf --show-npages "$test_root/retry.pdf")" == '1' ]]
fi

echo 'tests=passed'
