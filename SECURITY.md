# Security and privacy

`dell-scan` runs locally and sends scanner commands only to a directly attached USB device or to the
IPv4 address selected on the local network. It has no telemetry, account system, cloud upload, or
background daemon.

Do not attach scanned pages, private network configuration, serial numbers, service tags, or Dell
driver binaries to a public issue. Reproduce problems with the CLI's text output after removing any
private paths or addresses.

The installer compiles original source from this repository. It does not redistribute Dell software.
Users must obtain Dell's driver from Dell and are responsible for reviewing its terms and security
properties. The model is discontinued and the last driver is old, so use it only on systems where
that risk is acceptable.

For a potential vulnerability in this repository, open a GitHub security advisory rather than a
public issue.
