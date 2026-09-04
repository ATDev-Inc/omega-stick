# Security Policy

## Supported Versions

Omega Stick is in early development. There are no tagged releases yet, so
only the `main` branch is supported. Security fixes will land there first.

## Scope

Omega Stick is USB HID firmware for an embedded device (ESP32-S3). It does
not use a network stack today, so the relevant attack surface is primarily:

* USB descriptor and HID report handling (TinyUSB)
* I2C input handling from the TMAG5273 hall-effect sensor
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
* The firmware version or git commit hash you tested against
* The ESP-IDF version in use
* Any relevant logs, serial console output, or USB captures
* Whether the issue requires physical access to the device

## Response Expectations

Omega Stick is a small, early-stage, open-source project. We will make a
best-effort attempt to acknowledge reports within about a week, but we cannot
commit to a formal SLA. We appreciate your patience and will keep you updated
as we investigate and work on a fix.
