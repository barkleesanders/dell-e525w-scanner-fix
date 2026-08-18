# Dell E525w Scanner Fix for macOS

Restore ADF scanning on a Dell Color MFP E525w when Image Capture/AirScan fails,
`POST /eSCL/ScanJobs` returns HTTP 503, or the printer's native Wi-Fi scanner service is stuck.

This project provides a small open-source CLI that talks to Dell's locally installed scanner driver
at its native API. It supports both direct USB and Dell's native Wi-Fi protocol and creates a US
Letter PDF without Python, Homebrew packages, or cloud services.

> Unofficial community project. Not affiliated with or endorsed by Dell. The Dell driver is
> proprietary and is not included.

## One-line Wi-Fi fix

First install Dell's official E525w macOS driver. Then put a page in the ADF and run:

```bash
curl -fsSL https://raw.githubusercontent.com/barkleesanders/dell-e525w-scanner-fix/main/install.sh | bash -s -- --wifi
```

The CLI discovers the printer with mDNS. If discovery cannot identify it, provide its IPv4 address:

```bash
curl -fsSL https://raw.githubusercontent.com/barkleesanders/dell-e525w-scanner-fix/main/install.sh | bash -s -- --wifi --ip 192.168.1.50
```

## One-line USB workaround

If Wi-Fi remains broken, **connect the printer directly to the Mac with a USB cable**, load the ADF,
and run:

```bash
curl -fsSL https://raw.githubusercontent.com/barkleesanders/dell-e525w-scanner-fix/main/install.sh | bash -s -- --usb
```

USB bypasses the printer's eSCL and network scanner services entirely. It was the recovery path that
first proved the scanner hardware and ADF were healthy.

## Requirements

- macOS on Intel or Apple Silicon
- Dell Color MFP E525w
- Apple's command-line developer tools (`xcode-select --install` if `clang` is missing)
- [Dell's official E525w A05 macOS driver](https://www.dell.com/support/home/en-us/drivers/driversdetails?driverid=wmm7x)
- A US Letter page loaded in the automatic document feeder

Dell lists the A05 package as supporting macOS 10.6 through 11 and both Intel and Apple Silicon. The
recovery CLI has also worked with that installed driver on a newer Apple Silicon Mac, but Dell does
not officially support those newer operating systems for this discontinued model.

## Normal use after installation

```bash
# Wi-Fi, automatic discovery
dell-scan --wifi

# Wi-Fi, explicit address
dell-scan --wifi --ip 192.168.1.50

# USB cable connected directly to the Mac
dell-scan --usb

# Up to ten ADF pages and a chosen output file
dell-scan --wifi --pages 10 --output ~/Documents/Scans/batch.pdf
```

USB is the default if neither `--usb` nor `--wifi` is supplied. Output defaults to a timestamped PDF
under `~/Documents/Scans`. Run `dell-scan --help` for every option.

## What the installer does

The installer downloads only this repository's source files, compiles three small native binaries
with Apple's `clang`, and installs them under `~/.local`:

```text
~/.local/bin/dell-scan
~/.local/libexec/dell-e525w-scanner-fix/dell-scan-engine
~/.local/libexec/dell-e525w-scanner-fix/dell-wifi-bridge
~/.local/libexec/dell-e525w-scanner-fix/pgm-to-pdf
```

It does not download, copy, modify, or redistribute Dell's driver. Inspect `install.sh` before
running it if you do not want to pipe a network response directly to `bash`.

## How the fix works

The failure had three separate layers:

1. The printer advertised eSCL/AirScan and reported an idle, loaded ADF, but every ScanJobs request
   failed with HTTP 503. A certificate could not fix this because TLS was not the failing layer.
2. Dell's installed `SWLLD.dylib` still exposed a complete native scanner API. Direct USB calls to
   `FindScannerByLocation_pull`, `InitializeScanner`, `SetScanParameter`, `StartScan`, and `ReadScan`
   successfully transferred a full 300-DPI grayscale Letter page.
3. Dell's Wi-Fi path uses `FindScannerEx` and a proprietary stream on TCP 23010, not eSCL. A tiny
   loopback bridge sends that unchanged stream through Apple's `/usr/bin/nc`, avoiding a macOS local
   network denial encountered by the custom helper process.

The Wi-Fi service once stopped answering while printing, IPP, ping, and SNMP remained healthy. A
printer power cycle restored TCP 23010; the first post-restart attempt scanned a complete page over
Wi-Fi. The CLI deliberately does not probe that port before scanning because the legacy service
appears to tolerate only one session at a time.

See [research/PROTOCOL.md](research/PROTOCOL.md) for the sanitized technical evidence and the
diagnostic source files under `research/`.

## Troubleshooting

### `ADF is not ready`

Reseat the page in the top feeder until the printer detects it. Clear any physical jam before retrying.

### Wi-Fi times out or reports `connected=0`

Power the printer off and back on, wait for Wi-Fi to reconnect, and rerun `dell-scan --wifi`. If you
need the scan immediately, connect USB and run `dell-scan --usb`.

### Automatic discovery fails

Find the printer's IPv4 address in your router or the printer's network settings and use
`dell-scan --wifi --ip ADDRESS`.

### Dell driver is missing

Install Dell's A05 macOS package from the official link above. The CLI expects:

```text
/Library/Image Capture/Devices/Dell E525w Scanner (ICA).app/Contents/Resources/SWLLD.dylib
```

Use `--driver PATH` or `DELL_SCAN_DRIVER=PATH` only if your legitimate local installation uses a
different location.

### macOS asks for Local Network access

Allow Local Network access for Terminal (or the terminal application running the command). No scan
data leaves your LAN or Mac.

## Privacy and safety

- No telemetry, accounts, cloud upload, or background service
- Refuses to overwrite an existing PDF or preserved PGM directory
- Bounds page count and image allocations
- Cleans up only its task-specific temporary directory and child relay processes
- Scans in 300-DPI, 8-bit grayscale from the ADF

## Build and test

```bash
bash scripts/check.sh
```

The check compiles all original source with warnings as errors, runs Clang static analysis, runs
ShellCheck, performs an isolated installer test, and validates a generated PDF fixture.

## License

Original source in this repository is licensed under the [MIT License](LICENSE). Dell trademarks and
Dell's separately installed software remain the property of their respective owner.
