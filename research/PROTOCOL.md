# Sanitized investigation and protocol notes

These notes preserve the evidence needed to reproduce the fix without publishing scanned pages,
device identifiers, private network details, proprietary Dell binaries, Ghidra projects, or
decompiled Dell code.

## Proven behavior

- Device: Dell Color MFP E525w over USB and Wi-Fi
- USB identity: vendor `0x413c`, product `0x5909`
- Dell ICA functional unit: ADF unit 3
- Scan profile: US Letter, 300 DPI, 8-bit grayscale, ADF
- Driver geometry: 2560 by 3328 bytes/rows
- Visible output: 2550 by 3300 pixels
- Bytes transferred per page: 8,519,680
- Observed front-side row correction cycle: 0, 2048, 512, 1536 pixels, repeating
- Native network service: TCP 23010
- Broken compatibility path: eSCL unusable. `POST /eSCL/ScanJobs` returned HTTP 503 during the
  original failure; a later re-measurement found no eSCL service advertised or served at all
  (see "Independent re-verification" below)
- Working paths: Dell native USB; Dell native Wi-Fi after restarting its stuck printer service

The proof PDFs and source pages contained private information and are intentionally not distributed.
Their structure, page dimensions, and readability were validated locally before publication.

## Long USB batch findings

A sanitized archive run captured 370 raw side images across 18 sessions. The driver returned 186
document fronts and 184 blank backs. Every even raw side was blank or near-blank, and every odd side
contained document data; the converter's conservative blank-back threshold separated the two sets
in that run. The largest clean session contained 29 physical sheets (58 raw sides).

The observed fault did not correlate with a high sheet count. A one-side ceiling while additional
sheets remained caused Dell's cleanup path to wait indefinitely and left the printer displaying
`Computer(USB) Sending...`. Allowing the native session to reach feeder-empty completed cleanup
normally. A physical power cycle cleared the orphaned printer-side job; a web restart and a direct
abort did not. Immediately after power-on, USB enumeration can precede scanner-service readiness,
so the public CLI retries safe pre-scan failures at six-second intervals.

The same run exposed a reproducible horizontal row rotation on front-side images. By raw side number
modulo 8, the lossless correction offsets were 0, 2048, 512, and 1536 pixels. Applying the offsets as
circular byte reordering restored continuous page rows without scaling, interpolation, or cropping.
Back-side geometry with actual printed content was not validated, so the engine leaves those rows
unchanged rather than guessing.

## Driver interface used by the original source

The public scan engine dynamically resolves these exported symbols from the user's legitimate local
installation of `SWLLD.dylib`:

```text
FindScannerByLocation_pull
FindScannerEx
InitializeDriver
InitializeScanner
GetADFMode
GetScannerStatus
SetScanParameter
GetScanParameter
StartScan
ReadScan
StopScan
CancelScan
TerminateDriver
```

USB discovery calls `FindScannerByLocation_pull(locationID)`. Wi-Fi discovery calls
`FindScannerEx(true, address)`. Both then use the same initialization, parameter, start, read,
cancel, and termination sequence.

## Network finding

Symbol and debugger analysis showed that Dell's `CNetDevice` opens TCP port 23010. The correct IPv4
socket address reached the connect call, but an unsigned custom helper received macOS
`EHOSTUNREACH` while Apple's `/usr/bin/nc` could make the same connection. The production workaround
therefore has two pieces:

1. `dell-wifi-bridge` accepts one loopback connection from Dell's unmodified driver.
2. The shell launches `/usr/bin/nc` and connects the bridge to it with two private FIFOs.

This keeps the proprietary protocol unchanged. Only the process responsible for the outbound local
network socket changes.

The native scanner listener later wedged after interrupted discovery experiments. HTTP 80, IPP 631,
ICMP, and SNMP still worked, but 23010 timed out. A restart-only standard Printer-MIB reset recovered
the listener, after which a full ADF page scanned successfully over Wi-Fi. Do not use the network
transport probe casually: this legacy service appears single-session and a connection-only probe can
consume or wedge it until the printer restarts.

## Independent re-verification

A later pass re-measured the same printer and Mac from scratch against live sources rather than
against these notes. It confirmed the core claims and sharpened one of them. No device
identifiers, addresses, or scanned content are recorded here.

**The compatibility path is absent, not merely erroring.** A Bonjour browse for `_uscan._tcp` and
`_uscans._tcp` returned no scan service. `/eSCL/ScannerStatus` and `/eSCL/ScannerCapabilities`
returned HTTP 404 over both HTTP and HTTPS, including lowercase and mixed-case path variants,
while the printer's own web server answered normally in the same window. The 404 is therefore a
real routing answer, not an unreachable host. TCP 23010 was open at the same time. This is a
stronger statement than the original HTTP 503: in this state the device offers no eSCL scan
service for a generic AirScan client to use at all, which is why a compatibility-layer fix cannot
exist and the native path is the only option.

**The driver is modern, and that matters.** `SWLLD.dylib` and the ICA executable are both
universal `x86_64 arm64` Mach-O binaries dated June 2021, so they run natively on Apple silicon.
The common assumption that Dell scanning broke because the driver is a 32-bit relic does not hold
for the A05 package. The library is healthy; the consumer-facing layers stacked on top of it are
what fail. That is precisely why calling the library directly works.

**Dell's scanner app cannot be automated.** The installed `Dell E525w Scanner (ICA).app` declares
`LSUIElement = 1`, bundle identifier `dltsp4zICA`, version 1.0.2.6, and ships a single Mach-O with
no command-line entry point. It is an Image Capture plugin that only runs when a user drives
Apple's GUI. Batch, headless, and scheduled scanning are not features it withholds; they are
outside what its interface can express.

**The exported symbols exist.** `nm -gU` on the installed `SWLLD.dylib` lists
`_FindScannerByLocation`, `_FindScannerByLocation_pull`, `_FindScannerEx`,
`_FindScannerEx_Scopeid`, `_InitializeScanner`, `_SetScanParameter`, `_StartScan`, and `_ReadScan`
as global text symbols, alongside a C++ `CScanner` class.

**Dell's published package.** The linked official driver page lists the file as
`Dell A05_Mac.dmg`, version V1.1.5.8, release date 26 July 2021, supporting macOS 10.6 to 11. Dell
hosts more than one E525w macOS driver page with differing stated ranges, so quote the specific
`driverid` when citing a support claim.

## Why a certificate was not the fix

The failing eSCL request reached the printer and received an HTTP application response. It was not a
TLS certificate negotiation failure. The native Dell protocol used by this workaround is also a raw
TCP stream rather than HTTPS, so installing or generating a certificate would not repair either
failure mode.

## Reproducibility hashes

These hashes identify the locally analyzed Dell files but the files are not redistributed:

```text
SWLLD.dylib (universal)   08ef3e86caac303b238f68ce615bd08d64b1549c13cdcc4ec20ae231dba6291d
ICA executable (universal) 16cd8e206dd6548382c286d230a88643d430482c6157df2636ea542108108ff1
SWLLD arm64 slice         12b234d0556b59bb6ecce200bb85cfa8875d620e1ffa2150415bffd7ae59e0a0
ICA arm64 slice           9fa77d320b47523aa3e0b1ca05f047c3aa4e7eecfbb19508a2548e1eb1684160
```

Dell publishes SHA-256 `0191fdee9c4caddc90cb46c2c17b7d96727689686e019ff114edea807b9615a6`
for its A05 macOS installer on the linked official driver page.

## Included diagnostic source

- `dell_swlld_probe.c` exercises discovery, initialization, readiness, ADF state, and cleanup without
  starting a scan.
- `dell_network_transport_probe.c` compares a plain socket, Dell's `tcp_connect`, and
  `CNetDevice::OpenEx`. It is research code, not required by the installer, and may wedge port 23010.

Compiled probes, driver files, decompiler output, and scanned fixtures are excluded deliberately.

## Original development-session telemetry

The original reporter supplied this agent-session telemetry. It describes the debugging interface,
not printer throughput or the installed CLI:

```text
Worked for 8m 27s
WebSocket: 34 events sent (123ms)
WebSocket: 7,793 events received (391.9s)
Responses API overhead: 17.4s
Responses API inference: 49.9s
TTFT: 25.1s (iapi), 32.5s (service)
```
