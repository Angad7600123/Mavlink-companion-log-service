# MAVLink Companion Log Service

Automatic, verifiable archival of ArduPilot DataFlash logs from a flight
controller to a companion computer, communicating exclusively over MAVLink.

**Repository:** [github.com/Angad7600123/Mavlink-companion-log-service](https://github.com/Angad7600123/Mavlink-companion-log-service)

> Copyright (c) 2026 Angad Singh Bains. All rights reserved.
> Distributed under the **Angad Singh Personal & Non-Commercial Source Available
> License**. See [LICENSE](LICENSE).

---

## Table of Contents

- [Introduction](#introduction)
- [Why This Project Exists](#why-this-project-exists)
- [The Problem It Solves](#the-problem-it-solves)
- [Features](#features)
- [Architecture](#architecture)
- [Workflow](#workflow)
- [Durability Guarantees](#durability-guarantees)
- [Duplicate Detection](#duplicate-detection)
- [Storage Management](#storage-management)
- [Configuration](#configuration)
- [Installation](#installation)
- [Updating](#updating)
- [Running](#running)
- [Building from Source](#building-from-source)
- [Administration](#administration)
- [Troubleshooting](#troubleshooting)
- [FAQ](#faq)
- [Security Model](#security-model)
- [Performance](#performance)
- [Design Philosophy](#design-philosophy)
- [Documentation Index](#documentation-index)
- [Roadmap](#roadmap)
- [License Summary](#license-summary)
- [Disclaimer](#disclaimer)

---

## Introduction

**MAVLink Companion Log Service** (`mcls`) is a small, robust background service
for companion computers such as the Raspberry Pi. After every flight it connects
to the flight controller through [mavlink-router](https://github.com/mavlink-router/mavlink-router),
detects when the vehicle has disarmed, downloads every flight log that is not
already archived, verifies each download byte-for-byte, records it in a local
SQLite catalog, and only then erases the logs from the flight controller.

It is written in modern C++ with a focus on correctness, crash safety, and a
small runtime footprint. It speaks MAVLink and nothing else.

## Why This Project Exists

Flight controllers store high-rate DataFlash logs in limited onboard storage.
Those logs are essential for tuning, incident analysis, and maintenance, yet
they are easy to lose: onboard storage fills up, logs are overwritten, or they
are never retrieved because pulling them manually over a ground station is slow
and error-prone.

Existing retrieval workflows typically depend on a ground control station being
connected, an operator remembering to download logs, and a reliable link for the
duration of the transfer. None of that is guaranteed in the field, and none of
it is automatic.

This project exists to make log retention **automatic, unattended, and
trustworthy**. Once installed, it requires no operator action: power on, fly,
land, and the logs are safely archived on the companion computer.

## The Problem It Solves

- **Lost logs.** Onboard storage is finite and gets overwritten. Logs that are
  never downloaded are gone.
- **Manual effort.** Downloading logs by hand after every flight does not scale
  and is easily forgotten.
- **Unreliable transfers.** Links drop. A naive download has no way to recover
  from gaps or verify integrity.
- **Unsafe cleanup.** Erasing the flight controller before a verified copy
  exists risks permanent data loss.
- **Storage exhaustion on the companion.** Without a retention policy, an
  archive directory grows without bound.

`mcls` addresses each of these directly, as described in the sections below.

## Features

- **Automatic flight detection** from MAVLink HEARTBEAT, tracking arm/disarm
  transitions and vehicle identity (system id, component id, vehicle type,
  autopilot type).
- **Complete log synchronization.** Every log absent from the local catalog is
  downloaded, not merely the most recent one. A companion that was offline for
  several flights catches up on all of them.
- **Verifiable downloads** with byte-count verification and full-file SHA-256
  hashing.
- **Probe-confirmed duplicate detection** so logs are never downloaded twice and
  never skipped incorrectly.
- **Gap recovery** that re-requests only the missing byte ranges during a
  transfer, rather than restarting on packet loss.
- **Crash-safe storage pipeline** using staged writes, `fsync`, atomic rename,
  and a committed database row before any destructive action.
- **Conservative cleanup.** The flight controller is erased only after every log
  in the cycle has been durably archived.
- **Configurable retention** that evicts only the oldest verified archives when
  a size limit is exceeded.
- **SQLite catalog and statistics** for a permanent record of what has been
  archived.
- **systemd integration** with a hardened unit that starts on boot.

## Architecture

```
            ┌────────────────────┐
            │  Flight Controller │
            │     (ArduPilot)    │
            └─────────┬──────────┘
                      │ UART / serial
                      ▼
            ┌────────────────────┐
            │   mavlink-router   │
            └─────────┬──────────┘
                      │ MAVLink over TCP (default 5760)
                      ▼
        ┌──────────────────────────────┐
        │   MAVLink Companion Log       │
        │   Service  (mcls)             │
        │                               │
        │  MavlinkClient ── FlightMonitor
        │        │              │       │
        │        ▼              ▼       │
        │  LogDownloader ── DroneLogService
        │        │              │       │
        │        ▼              ▼       │
        │  StorageManager     Database  │
        └────────┬──────────────┬───────┘
                 ▼              ▼
          /var/lib/mcls/   database.sqlite
             logs/  tmp/
```

| Component | Responsibility |
|-----------|----------------|
| `MavlinkClient` | TCP connection to mavlink-router, frame parsing, thread-safe send, link monitoring |
| `FlightMonitor` | Arm/disarm detection, vehicle identity, link up/down events |
| `LogDownloader` | MAVLink log protocol, deduplication, gap recovery, verification |
| `StorageManager` | Durable file pipeline and retention enforcement |
| `Database` | SQLite catalog keyed by SHA-256, plus persistent statistics |
| `DroneLogService` | State-machine orchestration and reconnect handling |

A deeper description is available in [docs/architecture.md](docs/architecture.md).

## Workflow

1. The service starts on boot and connects to mavlink-router.
2. It waits for a HEARTBEAT and identifies the vehicle.
3. It observes an arm, then waits for the corresponding disarm.
4. After disarm it waits `delay_after_disarm` seconds (default 2) so the flight
   controller can finish writing its log.
5. It enumerates all logs via `LOG_REQUEST_LIST`.
6. For each log: it performs duplicate detection, downloads when required,
   verifies the result, writes it durably, and commits a catalog row.
7. If, and only if, every enumerated log archived successfully, it issues a
   single `LOG_ERASE`.
8. It enforces the storage limit and returns to waiting for the next flight.

The MAVLink message-level details are documented in
[docs/protocol.md](docs/protocol.md).

## Durability Guarantees

Data safety is the central design constraint. Each log is written through a
strict pipeline before anything is erased:

```
write .partial → flush → fsync(file) → atomic rename → fsync(parent dir)
→ SQLite COMMIT → (only after all logs) LOG_ERASE
```

Key guarantees:

- A partially downloaded file never replaces a verified archive, because the
  final name appears only after a successful atomic rename.
- The flight controller copy is the backup of record until a verified archive
  exists and its catalog row is committed.
- `LOG_ERASE` clears the **entire** DataFlash and therefore is sent at most once
  per cycle, only after every enumerated log is safely archived. A single
  failure keeps all flight controller logs for the next attempt.
- Interrupted downloads are not resumed across restarts. Any leftover `.partial`
  file is removed on startup and the log is re-downloaded from the flight
  controller, which still holds it.
- The SQLite catalog is opened in write-ahead logging mode with full
  synchronous writes for resilience against sudden power loss.

The complete crash-safety analysis is in [docs/durability.md](docs/durability.md).

## Duplicate Detection

A log is never downloaded twice, and a different log is never skipped by
accident:

1. The catalog is queried by `(flight_controller_log_id, log_size)`.
2. If no candidate exists, the log is downloaded in full.
3. If a candidate exists, the service downloads only the first
   `min(probe_bytes, log_size)` bytes, hashes them, and compares the result to
   the stored probe hash.
4. On a match the log is treated as a confirmed duplicate and skipped; on a
   mismatch it is downloaded in full.

The full-file SHA-256 is the permanent identity of each archived log. This
approach is reliable even when log ids restart after an erase, because identity
is content-based rather than id-based.

## Storage Management

- Archived logs are written under `/var/lib/mcls/logs/` in date-based folders.
- Filenames are derived from the companion computer's local date and time at the
  moment of archival, never from flight-controller-provided identifiers.
- When the total size of verified archives exceeds `max_size_gb`, the oldest
  verified archives are deleted first until the directory is back under the
  limit.
- In-progress and partial files are never deleted by the retention policy; they
  are managed separately and cleaned on startup.

## Configuration

The configuration file lives at `/etc/mcls/config.toml`. The default
configuration is:

```toml
[mavlink]
host = "127.0.0.1"
port = 5760
heartbeat_timeout_sec = 5

[download]
delay_after_disarm = 2
download_timeout = 5
retry_count = 3
retry_delay = 2
probe_bytes = 51200
verify_after_download = true
erase_after_success = true

[storage]
directory = "/var/lib/mcls"
max_size_gb = 1

[logging]
verbose = true
# file = "/var/log/mcls/mcls.log"
```

Every key is documented in [docs/configuration.md](docs/configuration.md). After
changing the configuration, restart the service (see [Administration](#administration)).

## Installation

### Prerequisites

- A Linux companion computer (Raspberry Pi OS Bookworm or compatible)
- A C++20 toolchain (GCC 12 or newer) and CMake 3.16 or newer
- SQLite development headers (`libsqlite3-dev`)
- A running, configured `mavlink-router` exposing a MAVLink TCP endpoint
- Network access on first build (dependencies are fetched during configuration)

### Install

```bash
git clone https://github.com/Angad7600123/Mavlink-companion-log-service.git
cd Mavlink-companion-log-service
chmod +x scripts/install.sh scripts/uninstall.sh
./scripts/install.sh
```

The installer builds the project in release mode, installs the `mcls` binary
(with an `mclsd` alias) to `/usr/local/bin`, installs the default configuration
to `/etc/mcls/config.toml`, creates the dedicated `mcls` system user and the
`/var/lib/mcls` state directory, and enables the systemd service.

## Updating

```bash
cd Mavlink-companion-log-service
git pull
./scripts/install.sh
sudo systemctl restart mavlink-companion-log-service
```

The state directory `/var/lib/mcls` and its SQLite catalog are preserved across
updates. Review [CHANGELOG.md](CHANGELOG.md) before updating to understand any
behavioral changes.

## Running

Start and inspect the service:

```bash
sudo systemctl start mavlink-companion-log-service
sudo systemctl status mavlink-companion-log-service
sudo journalctl -u mavlink-companion-log-service -f
```

Run manually for debugging, using a specific configuration file:

```bash
sudo -u mcls /usr/local/bin/mcls /etc/mcls/config.toml
```

## Building from Source

```bash
sudo apt install build-essential cmake libsqlite3-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Build and run the test suite:

```bash
cmake -S . -B build -DMCLS_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Administration

Common service operations:

```bash
sudo systemctl enable  mavlink-companion-log-service   # start on boot
sudo systemctl disable mavlink-companion-log-service
sudo systemctl restart mavlink-companion-log-service
sudo systemctl stop    mavlink-companion-log-service
```

Inspect archived data and the catalog:

```bash
ls -la /var/lib/mcls/logs/*/
sqlite3 /var/lib/mcls/database.sqlite "SELECT filename, fc_log_id, fc_log_size FROM archived_logs;"
```

Copy archives off the companion computer:

```bash
rsync -av /var/lib/mcls/logs/ user@backup-host:/backups/flight-logs/
```

Uninstall (state data is preserved):

```bash
./scripts/uninstall.sh
```

## Troubleshooting

**The service does not start.**
Check the journal: `sudo journalctl -u mavlink-companion-log-service -n 50 --no-pager`.
Confirm that mavlink-router is running and that the host and port in the
configuration match its TCP endpoint.

**No logs are archived after a flight.**
Confirm DataFlash logging is enabled on the flight controller, that HEARTBEAT
reaches mavlink-router, and that the vehicle was both armed and disarmed while
the service was running. If the controller needs more time to finalize its log,
increase `delay_after_disarm`.

**The flight controller logs are not erased.**
This is intentional whenever any log in the cycle failed to archive. The journal
will indicate that the erase was skipped. To disable erasing entirely, set
`erase_after_success = false`.

**The archive directory is full.**
The oldest verified archives are deleted automatically once `max_size_gb` is
exceeded. Increase the limit or copy archives off the device.

**Permission errors on the state directory.**
Ensure ownership is correct: `sudo chown -R mcls:mcls /var/lib/mcls`.

## FAQ

**Does this require a ground control station?**
No. The service is fully autonomous and needs only a MAVLink endpoint from
mavlink-router.

**Will it download logs from several missed flights?**
Yes. Every log not present in the catalog is downloaded, not only the latest.

**What happens if power is lost mid-download?**
The partial file is discarded on the next start and the log is downloaded again.
The flight controller still holds the log because it is erased only after a
verified archive exists.

**Can it accidentally erase logs it has not saved?**
No. A single `LOG_ERASE` is sent per cycle, and only after every enumerated log
is verified, written durably, and committed to the catalog.

**Why are filenames based on the companion clock instead of the log id?**
Log ids are reused after an erase and are not stable identities. Local
timestamps produce stable, human-readable names, while the catalog retains the
flight controller id and size for traceability.

**Is this Open Source?**
It is Source Available. See [License Summary](#license-summary) and
[LICENSE](LICENSE).

## Security Model

- The service communicates only with the configured MAVLink endpoint and writes
  only within its state directory `/var/lib/mcls`.
- The provided systemd unit runs as a dedicated unprivileged `mcls` user with
  system protection, no new privileges, an empty capability set, and write
  access restricted to the state directory.
- mavlink-router should be bound to localhost when the service runs on the same
  device, and must not be exposed to untrusted networks without appropriate
  protection.
- Archived flight logs may contain sensitive operational data and should be
  protected with appropriate filesystem permissions and backups.

Vulnerability reporting and supported versions are described in
[SECURITY.md](SECURITY.md).

## Performance

- Log data is streamed directly to disk in small chunks; a complete log is never
  buffered entirely in memory, keeping the resident footprint small.
- The receive path uses a dedicated thread with blocking I/O and condition
  variables rather than busy-waiting, minimizing idle CPU usage.
- SHA-256 is computed in a single pass while data is written; the probe hash is
  produced by cloning the hash state at the probe boundary, so the leading bytes
  are hashed only once.
- Throughput is ultimately bounded by the flight controller and the MAVLink link
  rather than by the service itself.

## Design Philosophy

- **Never lose a log.** Every design decision favors data safety over speed or
  convenience.
- **Be unattended and predictable.** The service behaves the same on every
  flight with no operator interaction.
- **Fail safe.** When in doubt, keep the flight controller copy and retry later.
- **Stay small and focused.** The service does one thing well and depends on as
  little as possible.
- **MAVLink only.** The service makes no assumptions about ground stations or
  external applications.

## Documentation Index

| Document | Description |
|----------|-------------|
| [docs/architecture.md](docs/architecture.md) | Components and state machine |
| [docs/configuration.md](docs/configuration.md) | Complete configuration reference |
| [docs/protocol.md](docs/protocol.md) | MAVLink log protocol usage |
| [docs/durability.md](docs/durability.md) | Crash safety and erase ordering |
| [LICENSE](LICENSE) | License terms |
| [NOTICE](NOTICE) | Copyright and attribution |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Contribution terms and guidelines |
| [SECURITY.md](SECURITY.md) | Vulnerability reporting |
| [CHANGELOG.md](CHANGELOG.md) | Version history |
| [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) | Community expectations |

## Roadmap

Planned directions for future releases. None of the following are implemented
yet, and all are subject to change at the discretion of the project owner.

- Optional compression of archived logs
- Configurable post-archive hooks (for example, copying to external storage)
- A read-only local status and statistics interface
- Optional integrity re-verification of existing archives
- Expanded vehicle-identity records in the catalog

## License Summary

This project is distributed under the **Angad Singh Personal & Non-Commercial
Source Available License**. It is Source Available, not Open Source under the OSI
definition.

Permitted: personal use, educational use, research, hobby projects, private
modifications, private forks, and studying the source.

Prohibited without a separate written commercial license: commercial use of any
kind, selling the software or modified versions, hosting it as a paid service,
bundling it with commercial products or hardware, and using the project branding
commercially.

Copyright always remains with the author. Commercial licenses may be granted
only by the copyright holder. For inquiries, contact singh4anga@gmail.com.

The complete terms are in [LICENSE](LICENSE), with attribution in [NOTICE](NOTICE).

## Disclaimer

This software interacts with flight hardware and handles flight data. It is
provided without warranty of any kind. You are solely responsible for verifying
correct operation on your platform and for complying with all applicable laws
and regulations. Maintain independent backups where logs are required for
regulatory or operational purposes. See [LICENSE](LICENSE) for the full warranty
and liability disclaimers.
