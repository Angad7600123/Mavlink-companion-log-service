# MAVLink Log Protocol

> Copyright (c) 2026 Angad Singh Bains. All rights reserved.
> [MAVLink Companion Log Service](https://github.com/Angad7600123/Mavlink-companion-log-service)
> · [README](../README.md) · [LICENSE](../LICENSE)

The service implements the ArduPilot DataFlash log transfer protocol over MAVLink.

## Messages Used

| Message | Direction | Purpose |
|---------|-----------|---------|
| `LOG_REQUEST_LIST` | GCS → FC | Enumerate available logs |
| `LOG_ENTRY` | FC → GCS | Log metadata (id, size, time_utc) |
| `LOG_REQUEST_DATA` | GCS → FC | Request byte range |
| `LOG_DATA` | FC → GCS | Data chunk (max `kLogChunkSize` bytes) |
| `LOG_REQUEST_END` | GCS → FC | Stop streaming |
| `LOG_ERASE` | GCS → FC | Erase **entire** DataFlash |

Chunk size is `MAVLINK_MSG_LOG_DATA_FIELD_DATA_LEN` (see `MavlinkLogProtocol.hpp`).

## Enumeration

After disarm + delay, send `LOG_REQUEST_LIST(start=0, end=0xFFFF)` and collect all `LOG_ENTRY` messages until idle (~1s with no new entries).

## Download (streaming)

For each log not confirmed as duplicate:

1. Send `LOG_REQUEST_DATA(id, 0, 0xFFFFFFFF)` and drain incoming `LOG_DATA`
2. Write by offset; track `ReceivedRanges` and chunk slot bitmap
3. On idle, merged gap-fill re-requests for missing ranges
4. Complete when ranges cover `[0, LOG_ENTRY.size)` — **not** from filesystem size
5. Verify: SHA-256, optional DataFlash parse, optional FC sample re-read
6. Emit `ArchiveSummary` log line

Target throughput: comparable to Mission Planner over the same telemetry path.

## Deduplication Probe

When `(fc_log_id, fc_log_size)` matches a catalog row:

1. Stream first `min(probe_bytes, log_size)` bytes
2. SHA-256 and compare to stored `probe_sha256`
3. Skip full download on match

## Erase

`LOG_ERASE` is sent **once** after **all** logs in the enumeration are archived. It has no per-log ID — it clears all FC logs.

---

See also: [durability.md](durability.md) · [architecture.md](architecture.md) · [reports/streaming-download-integrity.md](reports/streaming-download-integrity.md)

Distributed under the [Angad Singh Personal & Non-Commercial Source Available License](../LICENSE).
