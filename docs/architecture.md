# Architecture

> Copyright (c) 2026 Angad Singh Bains. All rights reserved.
> [MAVLink Companion Log Service](https://github.com/Angad7600123/Mavlink-companion-log-service)
> · [README](../README.md) · [LICENSE](../LICENSE)

## Overview

The service connects to `mavlink-router` over TCP, monitors flight state, and after disarm archives DataFlash logs using the MAVLink log protocol.

```
Flight Controller
       │ UART
       ▼
  mavlink-router
       │ TCP (default 5760)
       ▼
       mcls
   ┌───┴───┐
   ▼       ▼
Filesystem  SQLite
/var/lib/mcls
```

## Components

| Component | Responsibility |
|-----------|----------------|
| `MavlinkClient` | TCP I/O, frame parsing, thread-safe send |
| `FlightMonitor` | HEARTBEAT → arm/disarm, vehicle identity, link status |
| `LogDownloader` | LOG_REQUEST_LIST/DATA/ERASE, dedup, gap recovery |
| `StorageManager` | Durable file pipeline, storage limits |
| `Database` | SQLite catalog (SHA-256 identity) + statistics |
| `DroneLogService` | State machine orchestration |

## State Machine

1. Connect to mavlink-router
2. Wait for HEARTBEAT / vehicle detection
3. Wait for ARM → WAIT DISARM
4. On DISARM: delay (`delay_after_disarm`, default 2s)
5. Enumerate all FC logs
6. For each log: probe-confirmed dedup → download if needed → verify → durable persist → SQLite COMMIT
7. If **all** enumerated logs archived: single `LOG_ERASE`
8. Enforce storage limit → wait for next flight

Connection loss triggers reconnect; partial downloads are deleted and restarted (FC remains backup).

## Deduplication

1. Lookup `(fc_log_id, fc_log_size)` in SQLite
2. If candidate exists: download first `min(probe_bytes, size)` bytes, hash, compare to stored `probe_sha256`
3. Match → skip; mismatch → full download
4. Full-file SHA-256 is the permanent catalog key

## Erase Semantics

`LOG_ERASE` wipes the **entire** FC DataFlash. The service sends it **once per cycle**, only after every log in the current enumeration is archived.

---

See also: [protocol.md](protocol.md) · [durability.md](durability.md) · [configuration.md](configuration.md)

Distributed under the [Angad Singh Personal & Non-Commercial Source Available License](../LICENSE).
