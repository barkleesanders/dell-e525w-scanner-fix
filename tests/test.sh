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

printf 'P5\n100 100\n255\n' >"$test_root/sparse-back.pgm"
dd if=/dev/zero bs=10000 count=1 2>/dev/null | tr '\000' '\377' \
  >>"$test_root/sparse-back.pgm"
printf '\x00\x00\x00\x00\x00' | dd of="$test_root/sparse-back.pgm" bs=1 seek=1025 \
  conv=notrunc 2>/dev/null
printf '\x00\x00\x00\x00\x00' | dd of="$test_root/sparse-back.pgm" bs=1 seek=1125 \
  conv=notrunc 2>/dev/null
"$converter" --drop-blank-backs "$test_root/sparse-preserved.pdf" \
  "$test_root/fixture.pgm" "$test_root/sparse-back.pgm" >"$test_root/sparse-preserved.out"
grep -F 'pdf_pages=2' "$test_root/sparse-preserved.out"
grep -F 'blank_backs_dropped=0' "$test_root/sparse-preserved.out"

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
side=1
while (( side <= 10 )); do
  page_path="$(printf '%s-page-%03d.pgm' "$4" "$side")"
  if (( side % 2 == 1 )); then
    printf 'P5\n4 4\n255\n\x00\x22\x44\x66\x22\x44\x66\x88\x44\x66\x88\xaa\x66\x88\xaa\xff' \
      >"$page_path"
  else
    printf 'P5\n4 4\n255\n\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff' \
      >"$page_path"
  fi
  side=$((side + 1))
done
EOF
chmod +x "$engine"
printf 'test driver\n' >"$test_root/fake-driver.dylib"
PATH="$mock_bin:$PATH" DELL_SCAN_DRIVER="$test_root/fake-driver.dylib" \
  DELL_SCAN_RETRY_DELAY=0 DELL_TEST_ATTEMPT_FILE="$test_root/attempt-count" \
  "$scanner" --usb --output "$test_root/retry.pdf" >"$test_root/retry.out" \
  2>"$test_root/retry.err"
grep -F 'retrying in 0 seconds (1/6)' "$test_root/retry.err"
grep -F 'pdf_pages=10' "$test_root/retry.out"
grep -F 'blank_backs_dropped=0' "$test_root/retry.out"
grep -F 'Raw sides captured: 10' "$test_root/retry.out"
[[ "$(<"$test_root/attempt-count")" == '2' ]]

PATH="$mock_bin:$PATH" DELL_SCAN_DRIVER="$test_root/fake-driver.dylib" \
  DELL_SCAN_RETRY_DELAY=0 DELL_TEST_ATTEMPT_FILE="$test_root/attempt-count" \
  "$scanner" --usb --drop-blank-backs --output "$test_root/drop-blank-backs.pdf" \
  >"$test_root/drop-blank-backs.out" 2>"$test_root/drop-blank-backs.err"
grep -F 'pdf_pages=5' "$test_root/drop-blank-backs.out"
grep -F 'blank_backs_dropped=5' "$test_root/drop-blank-backs.out"
grep -F 'Raw sides captured: 10' "$test_root/drop-blank-backs.out"
[[ "$(<"$test_root/attempt-count")" == '3' ]]

PATH="$mock_bin:$PATH" DELL_SCAN_DRIVER="$test_root/fake-driver.dylib" \
  DELL_SCAN_RETRY_DELAY=0 DELL_TEST_ATTEMPT_FILE="$test_root/attempt-count" \
  "$scanner" --usb --keep-blank-backs --output "$test_root/keep-blank-backs.pdf" \
  >"$test_root/keep-blank-backs.out" 2>"$test_root/keep-blank-backs.err"
grep -F 'pdf_pages=10' "$test_root/keep-blank-backs.out"
grep -F 'blank_backs_dropped=0' "$test_root/keep-blank-backs.out"
grep -F 'Raw sides captured: 10' "$test_root/keep-blank-backs.out"
[[ "$(<"$test_root/attempt-count")" == '4' ]]

if DELL_SCAN_RETRY_DELAY=61 DELL_SCAN_DRIVER="$test_root/fake-driver.dylib" \
  "$scanner" --usb --output "$test_root/bad-delay.pdf" >"$test_root/bad-delay.out" \
  2>"$test_root/bad-delay.err"; then
  echo 'out-of-range retry delay unexpectedly passed' >&2
  exit 1
fi
grep -F 'USB retry delay must be from 0 to 60 seconds' "$test_root/bad-delay.err"

# Structural guard for the Apple Bash 3.2 defect fixed in 1.1.1: with `set -u`,
# "${array[@]}" is an unbound variable when the array is empty. Both `bash -n`
# and the linter accept that form, so only this invariant catches a
# reintroduction. Any array that starts empty and is conditionally appended must
# be expanded behind a ${#array[@]} count guard.
unguarded_arrays=''
for relative_script in bin/dell-scan install.sh scripts/check.sh tests/test.sh; do
  script_path="$repo_root/$relative_script"
  while IFS= read -r array_name; do
    [[ -n "$array_name" ]] || continue
    grep -qF "\${#${array_name}[@]}" "$script_path" ||
      unguarded_arrays="$unguarded_arrays $relative_script:$array_name"
  done < <(grep -oE '^[[:space:]]*(declare -a )?[A-Za-z_][A-Za-z0-9_]*=\(\)' "$script_path" |
    sed -E 's/^[[:space:]]*(declare -a )?//; s/=\(\)$//' | sort -u)
done
if [[ -n "$unguarded_arrays" ]]; then
  echo "empty-initialized arrays expanded without a count guard:$unguarded_arrays" >&2
  exit 1
fi

# The Wi-Fi bridge previously had no runtime coverage at all -- check.sh only
# compiled it. This exercises a real loopback session: bidirectional relay,
# clean-EOF exit status, and the one-line byte summary that replaced ~130
# per-chunk log lines per page.
bridge="$test_root/prefix/libexec/dell-e525w-scanner-fix/dell-wifi-bridge"
bridge_dir="$test_root/bridge"
mkdir "$bridge_dir"
mkfifo "$bridge_dir/to_printer" "$bridge_dir/from_printer"
( cat "$bridge_dir/to_printer" >"$bridge_dir/captured" ) &
bridge_capture_pid=$!
( printf 'PRINTER-REPLY'; sleep 2 ) >"$bridge_dir/from_printer" &
bridge_feed_pid=$!
"$bridge" 127.0.0.1 "$bridge_dir/to_printer" "$bridge_dir/from_printer" \
  >"$bridge_dir/out" 2>"$bridge_dir/err" &
bridge_pid=$!
sleep 1

bridge_client_status=0
python3 - "$bridge_dir/received" <<'PYEOF' || bridge_client_status=$?
import socket, sys
with socket.create_connection(("127.0.0.1", 23010), timeout=10) as s:
    s.sendall(b"SCAN-REQUEST")
    s.shutdown(socket.SHUT_WR)
    chunks = []
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        chunks.append(chunk)
open(sys.argv[1], "wb").write(b"".join(chunks))
PYEOF

wait "$bridge_pid"
bridge_exit=$?
kill "$bridge_capture_pid" "$bridge_feed_pid" 2>/dev/null || true
wait "$bridge_capture_pid" "$bridge_feed_pid" 2>/dev/null || true

(( bridge_client_status == 0 )) || {
  echo "bridge client failed (status $bridge_client_status)" >&2
  exit 1
}
(( bridge_exit == 0 )) || {
  echo "bridge exited $bridge_exit on a clean session" >&2
  cat "$bridge_dir/err" >&2
  exit 1
}
[[ "$(<"$bridge_dir/captured")" == 'SCAN-REQUEST' ]] ||
  { echo 'bridge did not relay client bytes to the printer side' >&2; exit 1; }
[[ "$(<"$bridge_dir/received")" == 'PRINTER-REPLY' ]] ||
  { echo 'bridge did not relay printer bytes back to the client' >&2; exit 1; }
grep -qF 'bridge_to_printer_bytes=12 bridge_from_printer_bytes=13' "$bridge_dir/err" ||
  { echo 'bridge byte summary missing or wrong' >&2; cat "$bridge_dir/err" >&2; exit 1; }
bridge_err_lines="$(wc -l <"$bridge_dir/err" | tr -d ' ')"
(( bridge_err_lines == 1 )) || {
  echo "bridge logged $bridge_err_lines stderr lines; expected exactly 1 summary" >&2
  echo 'per-chunk logging buries the real error when dell-scan cats this log' >&2
  exit 1
}

if command -v qpdf >/dev/null; then
  qpdf --check "$test_root/fixture.pdf" >/dev/null
  [[ "$(qpdf --show-npages "$test_root/fixture.pdf")" == '1' ]]
  qpdf --check "$test_root/filtered.pdf" >/dev/null
  [[ "$(qpdf --show-npages "$test_root/filtered.pdf")" == '2' ]]
  qpdf --check "$test_root/sparse-preserved.pdf" >/dev/null
  [[ "$(qpdf --show-npages "$test_root/sparse-preserved.pdf")" == '2' ]]
  qpdf --check "$test_root/retry.pdf" >/dev/null
  [[ "$(qpdf --show-npages "$test_root/retry.pdf")" == '10' ]]
  qpdf --check "$test_root/drop-blank-backs.pdf" >/dev/null
  [[ "$(qpdf --show-npages "$test_root/drop-blank-backs.pdf")" == '5' ]]
  qpdf --check "$test_root/keep-blank-backs.pdf" >/dev/null
  [[ "$(qpdf --show-npages "$test_root/keep-blank-backs.pdf")" == '10' ]]
fi

echo 'tests=passed'
