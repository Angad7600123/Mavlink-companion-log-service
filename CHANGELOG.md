# Changelog

All notable changes to **MAVLink Companion Log Service** are documented in this
file.

**Repository:** [github.com/Angad7600123/Mavlink-companion-log-service](https://github.com/Angad7600123/Mavlink-companion-log-service)

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

> Copyright (c) 2026 Angad Singh Bains. All rights reserved.
> Distributed under the **Angad Singh Personal & Non-Commercial Source Available
> License**. See [LICENSE](LICENSE).

## [Unreleased]

### Added

- `docs/reports/log-download-robustness.md` — incident analysis and fix report
- `scripts/update.sh` — binary-only updates without touching `/etc/mcls/config.toml`

### Changed

- Install scripts preserve existing config; write reference copy to `config.toml.example`
- Install/update scripts refuse to install an empty `build/mcls` binary

### Fixed

- **Log download stall:** reject zero-length `LOG_DATA`; discard empty matches in
  `waitForLogData()`; prevent infinite gap loop with 0-byte partial files
- **FC session cleanup:** send `LOG_REQUEST_END` on failed/aborted transfers and between
  retry attempts
- **UDP transport:** buffer full datagrams before MAVLink parsing (split TX/RX ports)
- **Link handling:** treat any inbound MAVLink frame as link activity; do not disconnect
  transport during archive
- **Config on reinstall:** `sudo test -f` guard so `/etc/mcls/config.toml` is not overwritten

## [1.0.0] - 2026-06-25

Initial release.

### Added

- Project creation and initial public source-available release.
- Modular C++20 architecture: `MavlinkClient`, `FlightMonitor`, `LogDownloader`,
  `StorageManager`, `Database`, and the `DroneLogService` state machine.
- Automatic flight detection from MAVLink HEARTBEAT, including arm/disarm
  transitions and vehicle identity tracking.
- Pluggable MAVLink transport (`TcpTransport`, `UdpTransport`) with configurable endpoint
  log that is not already present in the local catalog.
- Verifiable downloads with byte-count checks and full-file SHA-256 hashing.
- Probe-confirmed duplicate detection to avoid both re-downloading and incorrect
  skipping of logs.
- In-session gap recovery that re-requests only missing byte ranges over lossy
  links.
- SQLite catalog (write-ahead logging, full synchronous writes) recording each
  archived log and persistent statistics.
- Crash-safe storage pipeline: staged `.partial` write, flush, file `fsync`,
  atomic rename, parent-directory `fsync`, then committed catalog row.
- Conservative cleanup: a single `LOG_ERASE` per cycle, sent only after every
  enumerated log is durably archived.
- Storage management with a configurable size limit and eviction of the oldest
  verified archives only.
- Local-time, date-based archive filenames independent of flight controller
  identifiers.
- TOML configuration with sensible defaults.
- systemd service unit and installation and uninstallation scripts.
- Unit tests covering configuration parsing, the catalog database, received-range
  gap logic, and hashing.
- Documentation set: architecture, configuration, protocol, and durability.

[Unreleased]: https://github.com/Angad7600123/Mavlink-companion-log-service/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/Angad7600123/Mavlink-companion-log-service/releases/tag/v1.0.0
