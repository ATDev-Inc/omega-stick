# Security Policy

## Supported Versions

Omega Stick is in early development. There are no tagged releases yet, so
only the `main` branch is supported. Security fixes will land there first.

## Scope

Omega Stick is USB HID firmware for an embedded device (ESP32-S3). It does
not use a network stack today, so the relevant attack surface is primarily:

* USB descriptor and HID report handling (TinyUSB)
* I2C input handling from the TMAG5273 hall-effect sensor
* The calibration console protocol, parsed from frames received over the USB
  vendor (WebUSB) and CDC (serial) interfaces
* The firmware update path: images uploaded through the console are validated
  but not signed (secure boot and image signing are deferred, see D22 in
  [docs/decisions.md](docs/decisions.md)), and the console's "Check for latest
  firmware" button trusts a release feed served from the project's GitHub Pages
  site, verifying only the size and SHA-256 listed in that feed
* The GitHub Pages site and release workflow, which publish the console, the
  first-flash page and the firmware files
* Buffer handling and parsing elsewhere in the firmware

Physical-access attacks against the device itself (someone with the device
in hand, opening it, or connecting to its exposed pins or debug interfaces)
are considered out of scope, as is generally true for small embedded
accessibility hardware.

## Reporting a Vulnerability

Please do not open a public issue for a suspected security vulnerability.

Instead, use GitHub's private vulnerability reporting form for this
repository:

https://github.com/ATDev-Inc/omega-stick/security/advisories/new

This allows maintainers to review and address the issue before any details
are made public.

## What to Include

To help us assess and reproduce the issue quickly, please include as much of
the following as you can:

* A description of the vulnerability and its potential impact
* Steps to reproduce, including hardware setup if relevant
* The firmware version and build you tested against: use the calibration
  console's Copy button and paste the line (version, build, ELF sha, running
  partition). See [docs/firmware-updates.md](docs/firmware-updates.md)
* The ESP-IDF version in use
* Any relevant logs, serial console output, or USB captures
* Whether the issue requires physical access to the device

## Response Expectations

Omega Stick is a small, early-stage, open-source project. We will make a
best-effort attempt to acknowledge reports within about a week, but we cannot
commit to a formal SLA. We appreciate your patience and will keep you updated
as we investigate and work on a fix.
