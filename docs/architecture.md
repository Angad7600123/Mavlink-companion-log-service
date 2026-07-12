# Architecture

> Copyright (c) 2026 Angad Singh Bains. All rights reserved.
> [MAVLink Companion Service](https://github.com/Angad7600123/Mavlink-companion-log-service)
> · [README](../README.md) · [LICENSE](../LICENSE)

## Overview

The service connects to a configured MAVLink transport endpoint (TCP or UDP),
monitors flight state, and after disarm archives DataFlash logs using the MAVLink
log protocol.

```
Flight Controller
       │ UART (outside mcls)
       ▼
  Companion MAVLink software
       │ TCP or UDP endpoint
       ▼
       mcls ◄─── localhost:14541 (JSON UDP) ◄─► wfb-ng companion stream ◄─► Android
   ┌───┴───┐
   ▼       ▼
Filesystem  SQLite
/var/lib/mcls
```

## Components

| Component | Responsibility |
|-----------|----------------|
| `Transport` | Pluggable byte stream (`TcpTransport`, `UdpTransport`) |
| `MavlinkClient` | Frame parsing, thread-safe send, link monitoring |
| `FlightMonitor` | HEARTBEAT → arm/disarm, vehicle identity, link status |
| `LogDownloader` | LOG_REQUEST_LIST/DATA/ERASE, dedup, verification orchestration |
| `StreamDownloadSession` | Streaming download, gap-fill, chunk tracking, session metrics |
| `DataFlashValidator` | Standalone DataFlash `.bin` structural validation (path in → result out) |
| `StorageManager` | Durable file pipeline, storage limits |
| `Database` | SQLite catalog (SHA-256 identity) + statistics |
| `DroneLogService` | State machine orchestration |
| `CompanionUdpServer` | Optional JSON control API over localhost UDP (wfb-ng bridge) |
| `CompanionProtocol` | JSON request parse + response serialize with size budget enforcement |

### MAVLink log chunk size

All download, verification, and gap logic derives from a single constant:

```cpp
constexpr std::size_t kLogChunkSize = MAVLINK_MSG_LOG_DATA_FIELD_DATA_LEN;
```

Defined in `MavlinkLogProtocol.hpp`. No hardcoded payload sizes elsewhere.

### Partial file writes

Partial downloads grow by seek-and-write at received offsets. Files are **not** pre-sized to `LOG_ENTRY.size`. Completion and verification use `ReceivedRanges` (and chunk slot coverage), never filesystem size alone.

### DataFlashValidator independence

`DataFlashValidator` has no dependencies on `LogDownloader`, SQLite, or configuration classes. Thresholds are passed by the caller. Intended for reuse by CLI tools, offline validation, and tests.

### Observability

Every archive attempt (success or failure) emits an `ArchiveSummary` log line with duration, throughput, verification outcomes, and erase decision. Optional `benchmark_download` enables periodic profiling metrics during transfer.

## State Machine

1. Connect to configured transport endpoint
2. Wait for HEARTBEAT / vehicle detection
3. Wait for ARM → WAIT DISARM
4. On DISARM: delay (`delay_after_disarm`, default 2s)
5. Enumerate all FC logs
6. For each log: probe-confirmed dedup → streaming download → layered verify (ranges, SHA-256, DataFlash parse, FC sample re-read) → durable persist → SQLite COMMIT → `ArchiveSummary`
7. If **all** enumerated logs archived: single `LOG_ERASE`
8. Enforce storage limit → wait for next flight

Connection loss triggers reconnect; partial downloads are deleted and restarted (FC remains backup).

## Transport abstraction

`mcls` is transport-agnostic. Configuration selects the implementation:

```toml
[transport]
transport = "tcp"   # or "udp"
host = "127.0.0.1"
port = 5760
```

Serial access, wfb-ng, and mavlink-router (if used) live **outside** this service.

## Companion control API (optional)

When `[companion] enabled = true`, `DroneLogService` starts a background `CompanionUdpServer`
thread that binds `127.0.0.1:14541`. Requests from Android arrive via wfb-ng
(stream `0xc0`) → `127.0.0.1:14540` → mcls, and responses go the reverse path.

mcls never reads WFB frames directly. It only sees plain JSON bytes on localhost UDP,
the same way it sees MAVLink bytes on its configured transport port.

See [companion-wfb.md](companion-wfb.md) for the full setup and protocol reference.

## Deduplication

1. Lookup `(fc_log_id, fc_log_size)` in SQLite
2. If candidate exists: download first `min(probe_bytes, size)` bytes, hash, compare to stored `probe_sha256`
3. Match → skip; mismatch → full download
4. Full-file SHA-256 is the permanent catalog key

## Erase Semantics

`LOG_ERASE` wipes the **entire** FC DataFlash. The service sends it **once per cycle**, only after every log in the current enumeration is archived.

---

See also: [protocol.md](protocol.md) · [durability.md](durability.md) · [configuration.md](configuration.md) · [companion-wfb.md](companion-wfb.md)

Distributed under the [Angad Singh Personal & Non-Commercial Source Available License](../LICENSE).
