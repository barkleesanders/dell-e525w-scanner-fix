#!/bin/bash

set -euo pipefail

readonly repository='barkleesanders/dell-e525w-scanner-fix'
readonly default_base_url="https://raw.githubusercontent.com/$repository/main"
readonly default_driver='/Library/Image Capture/Devices/Dell E525w Scanner (ICA).app/Contents/Resources/SWLLD.dylib'
readonly driver_page='https://www.dell.com/support/home/en-us/drivers/driversdetails?driverid=wmm7x'

install_prefix="${DELL_SCAN_PREFIX:-$HOME/.local}"
source_root="${DELL_SCAN_SOURCE_DIR:-}"
base_url="${DELL_SCAN_BASE_URL:-$default_base_url}"
scan_mode=''
driver_path="${DELL_SCAN_DRIVER:-$default_driver}"
staging_dir=''
declare -a scan_arguments=()

usage() {
  cat <<EOF
Install dell-scan from $repository.

Usage:
  curl -fsSL $default_base_url/install.sh | bash
  curl -fsSL $default_base_url/install.sh | bash -s -- --wifi
  curl -fsSL $default_base_url/install.sh | bash -s -- --usb

Installer options:
      --prefix PATH     Installation prefix (default: ~/.local)
      --wifi            Install, then scan over Wi-Fi
      --usb             Install, then scan over a directly connected USB cable

The scan options --ip, --max-sides, --pages, --output, --driver, --keep-pgm,
--drop-blank-backs, and --keep-blank-backs are passed to dell-scan. With no --wifi or --usb option,
this script only installs.
EOF
}

fail() {
  printf 'install: %s\n' "$*" >&2
  exit 2
}

cleanup() {
  if [[ -n "$staging_dir" && -d "$staging_dir" ]]; then
    find "$staging_dir" -type f -delete
    rmdir "$staging_dir" 2>/dev/null || true
  fi
}
trap cleanup EXIT HUP INT TERM

while (( $# > 0 )); do
  case "$1" in
    --prefix)
      (( $# >= 2 )) || fail '--prefix requires a path'
      install_prefix="$2"
      shift 2
      ;;
    --wifi|--usb)
      scan_mode="${1#--}"
      scan_arguments+=("$1")
      shift
      ;;
    --ip|--max-sides|--pages|-p|--output|-o|--driver)
      (( $# >= 2 )) || fail "$1 requires a value"
      if [[ "$1" == '--ip' ]]; then
        scan_mode='wifi'
      elif [[ "$1" == '--driver' ]]; then
        driver_path="$2"
      fi
      scan_arguments+=("$1" "$2")
      shift 2
      ;;
    --keep-pgm|--drop-blank-backs|--keep-blank-backs)
      scan_arguments+=("$1")
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      fail "unknown option: $1"
      ;;
  esac
done

[[ "$(uname -s)" == 'Darwin' ]] || fail 'this recovery tool currently supports macOS only'
command -v clang >/dev/null ||
  fail "Apple's command-line developer tools are required; run: xcode-select --install"
command -v curl >/dev/null || fail 'curl is required'

if [[ ! -r "$driver_path" ]]; then
  printf '%s\n' "Dell's official E525w scanner driver was not found at:" >&2
  printf '  %s\n' "$driver_path" >&2
  printf '%s\n' "Install it from Dell before scanning: $driver_page" >&2
  [[ -z "$scan_mode" ]] || exit 2
fi

staging_dir="$(mktemp -d "${TMPDIR:-/tmp}/dell-e525w-install.XXXXXX")"

fetch_file() {
  local relative_path="$1"
  local destination="$2"
  if [[ -n "$source_root" ]]; then
    [[ -r "$source_root/$relative_path" ]] ||
      fail "local source file is missing: $source_root/$relative_path"
    /usr/bin/install -m 0644 "$source_root/$relative_path" "$destination"
  else
    curl -fsSL "$base_url/$relative_path" -o "$destination"
  fi
}

fetch_file 'src/dell_scan_engine.c' "$staging_dir/dell_scan_engine.c"
fetch_file 'src/dell_image_quality.h' "$staging_dir/dell_image_quality.h"
fetch_file 'src/dell_row_wrap.h' "$staging_dir/dell_row_wrap.h"
fetch_file 'src/dell_wifi_bridge.c' "$staging_dir/dell_wifi_bridge.c"
fetch_file 'src/pgm_to_pdf.c' "$staging_dir/pgm_to_pdf.c"
fetch_file 'bin/dell-scan' "$staging_dir/dell-scan"

readonly compile_flags=(-std=c17 -Wall -Wextra -Wpedantic -Werror -O2)
clang "${compile_flags[@]}" "$staging_dir/dell_scan_engine.c" \
  -o "$staging_dir/dell-scan-engine"
clang "${compile_flags[@]}" "$staging_dir/dell_wifi_bridge.c" \
  -o "$staging_dir/dell-wifi-bridge"
clang "${compile_flags[@]}" "$staging_dir/pgm_to_pdf.c" \
  -framework ApplicationServices -o "$staging_dir/pgm-to-pdf"

install_bin="$install_prefix/bin"
install_libexec="$install_prefix/libexec/dell-e525w-scanner-fix"
/usr/bin/install -d "$install_bin" "$install_libexec"
/usr/bin/install -m 0755 "$staging_dir/dell-scan" "$install_bin/dell-scan"
/usr/bin/install -m 0755 "$staging_dir/dell-scan-engine" \
  "$install_libexec/dell-scan-engine"
/usr/bin/install -m 0755 "$staging_dir/dell-wifi-bridge" \
  "$install_libexec/dell-wifi-bridge"
/usr/bin/install -m 0755 "$staging_dir/pgm-to-pdf" "$install_libexec/pgm-to-pdf"

printf 'Installed dell-scan to %s\n' "$install_bin/dell-scan"
case ":$PATH:" in
  *":$install_bin:"*) ;;
  *) printf 'Add this directory to PATH: %s\n' "$install_bin" ;;
esac

if [[ -n "$scan_mode" ]]; then
  printf 'Starting %s scan...\n' "$scan_mode"
  DELL_SCAN_DRIVER="$driver_path" "$install_bin/dell-scan" "${scan_arguments[@]}"
else
  printf '%s\n' 'Next: dell-scan --wifi'
  printf '%s\n' 'USB fallback: connect the cable, then run: dell-scan --usb'
fi
