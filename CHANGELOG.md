# Changelog

All notable changes to **MAVLink Companion Service** are documented in this
file.

**Repository:** [github.com/Angad7600123/Mavlink-companion-log-service](https://github.com/Angad7600123/Mavlink-companion-log-service)

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

> Copyright (c) 2026 Angad Singh Bains. All rights reserved.
> Distributed under the **Angad Singh Personal & Non-Commercial Source Available
> License**. See [LICENSE](LICENSE).

## [Unreleased]

### Added

- **Streaming log download** — Mission Planner-style `LOG_REQUEST_DATA` streaming with merged gap-fill (`StreamDownloadSession`)
- **Layered integrity** — DataFlash structural parse, chunk overlap detection, hybrid FC sample re-read (7 anchors + random)
- **`DataFlashValidator`** — standalone path-in validation API for reuse by CLI/tests
- **`ArchiveSummary`** — structured per-archive performance log line (always on)
- **`benchmark_download`** — optional profiling metrics during transfer
- **Companion `udp_proxy` keepalive** — opaque 1-byte transport beacon (`udp_proxy_keepalive_ms`, default 5000) that keeps wfb-ng's `listen://` reply address registered so GS→Pi uplink is deliverable despite mcls being purely reactive
- **`archive.start` idempotency** — preconditions evaluated at request time via an archive-start gate; a retry while a cycle is in flight returns idempotent success (`ok:true`, `data.already_running:true`) instead of queuing a second cycle; genuine failures still return `armed`/`not_connected`
- **Companion `caps` op** — advertises protocol version, supported `ops`, and `limits` for client feature detection
- **Companion manual jobs** — `logs.refresh` (re-enumerate), `logs.download` (`sel.ids[]`/`sel.all` → archive to Pi, no FC erase), `logs.erase` (super-delete: unconditional DataFlash wipe, overrides in-flight jobs); all reuse the existing `LogDownloader` pipeline via new `ManualRefresh`/`ManualDownload`/`ManualErase` states
- **Companion `client` echo** — optional request `client` field echoed verbatim in every response (future multi-GS filtering)
- **Companion `status.job`** — job descriptor (`type`: archive/refresh/download/erase) derived from the state machine
- **Companion `archive.cancel`** now returns the uniform job-ack shape (`data.accepted`) like the other job ops
- **Companion `fc.logs` entries** now carry `t` (`LOG_ENTRY.time_utc`) and `dl` (present in Pi archive catalog)
- **Companion `status` download progress** — live `percent` (0–100) and `bytes_per_sec` throughput surfaced from the download path
- **`[recording]` — onboard Pi video recording** (`VideoRecorder`, `rec.start`/`rec.stop`, `status.recording`) — records the FPV feed on the Pi (pristine, pre-radio) instead of on the phone. Reads a local RTP/H264 tap from a second `tee` branch added to the camera's gst-launch-1.0 pipeline (loopback-only, zero WFB bandwidth cost) and writes MPEG-TS files to a user-configured `mount_path` (survives abrupt truncation on crash/power loss, unlike MP4). Spawns/kills a `gst-launch-1.0` subprocess via `posix_spawn` (multi-threaded-process safe, unlike `fork()`); `rec.stop` returns immediately and hands off graceful-then-forced shutdown to a detached watcher rather than blocking the companion UDP thread. Fully independent of the archive state machine and `CompanionCommandQueue` — handled directly on the UDP thread like `archive.cancel`, so it works regardless of any running job. New error codes `recording_disabled`, `no_media`.
- `MavlinkLogProtocol.hpp` — shared `kLogChunkSize` constant (no magic 90)
- Unit tests: chunk coverage, FC sample offsets, DataFlash validator, MAVLink protocol helpers
- `docs/reports/streaming-download-integrity.md` — implementation report

### Fixed

- **`archive.cancel` never actually stopped a running job** — cancellation was routed through `CompanionCommandQueue`, which the main loop only drains *between* state-machine steps. A running job (archive/download/refresh) executes entirely inside a single `processState()` call, so a queued cancel sat unprocessed until the job finished on its own — a no-op for the one case it exists for. `CompanionUdpServer` now takes a `CancelFn` invoked immediately on the UDP thread (`LogDownloader::requestCancel()` is an atomic-bool store, safe cross-thread), so cancellation is no longer coupled to the main loop's scheduling. Also made the FC-busy and enumeration-retry back-off waits interruptible (`LogDownloader::interruptibleSleep`) so a cancel during one of those windows isn't delayed by the full 3s.
- **Download aborted when FC answered only zero-length `LOG_DATA`** — right after disarm the FC can acknowledge `LOG_REQUEST_DATA` with zero-length `LOG_DATA` (the just-closed log is not servable yet); the session burned its retry/stall budget in ~19s and failed the cycle (`no forward progress`). The zero-length signature is now counted and, when seen with zero bytes received, the session backs off and re-requests (`fc_busy_retry_attempts`, default 8 × `fc_busy_retry_delay_sec` 3s) without consuming normal retry budgets. Zero-length log lines are also rate-limited (1 in 10) instead of spamming the journal.
- **Enumeration returned 0 logs right after disarm** — the automatic archive cycle enumerated only `delay_after_disarm` (was 2s) after disarm with a single, non-retried `LOG_REQUEST_LIST`, so it found nothing while the FC was still finalizing the flight log (a later manual refresh saw the logs). `enumerateLogs()` now re-issues the request (`enumerate_attempts`, default 6 × `enumerate_retry_delay_sec` 3s) until logs appear; `delay_after_disarm` default raised 2→5.
- **Enumeration always ran the full 15s deadline** — idle-completion checked `!pending_entries_.empty()` (never drained mid-enumeration) instead of *new* entries since the last tick, so every successful enumeration ignored the 1s settle and blocked for 15s (slow app Refresh). Now completes ~1s after the last entry.

### Changed

- `max_queued_log_data` default raised from `256` to `2048`
- Partial files grow by seek/write; not pre-sized to `LOG_ENTRY.size`
- `docs/reports/log-download-robustness.md` — incident analysis and fix report
- `scripts/update.sh` — binary-only updates without touching `/etc/mcls/config.toml`

### Changed

- Install scripts preserve existing config; write reference copy to `config.toml.example`
- Install/update scripts refuse to install an empty `build/mcls` binary

### Fixed

- **DataFlash FMT parser:** read `type` and `length` at offsets +3/+4 per ArduPilot
  `log_Format` (was incorrectly skipping `name[4]`, corrupting the format table and
  rejecting valid logs when `verify_dataflash_parse = true`)
- **Log download stall:** reject zero-length `LOG_DATA`; discard empty matches in
  `waitForLogData()`; prevent infinite gap loop with 0-byte partial files
- **FC session cleanup:** guaranteed `LOG_REQUEST_END` on every archive exit path
  (success, failure, cancel, shutdown) via RAII session guard
- **Forward-progress abort:** abort a log if the byte offset does not advance after
  `stall_abort_attempts`, bounding worst-case failure time
- **Cooperative cancellation:** re-arm or a new disarm during a download cancels the
  in-flight transfer; a new disarm schedules a fresh cycle without a restart
- **Conditional transport reconnect:** reconnect only on transport-class failures
  (send failed, closed, link timeout) or after N consecutive failures — not on every
  failure
- **Structured stall logging:** one line per failed chunk with log id, offset, expected
  vs received bytes, attempt, and reason
- **UDP transport:** buffer full datagrams before MAVLink parsing (split TX/RX ports)
- **Link handling:** treat any inbound MAVLink frame as link activity; do not disconnect
  transport during archive
- **Config on reinstall:** `sudo test -f` guard so `/etc/mcls/config.toml` is not overwritten

### Changed (config)

- New `[download]` keys: `stall_abort_attempts`, `max_queued_log_data`,
  `reconnect_on_transport_failure`, `reconnect_after_consecutive_failures`

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
