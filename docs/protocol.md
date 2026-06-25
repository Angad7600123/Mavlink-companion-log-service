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
| `LOG_DATA` | FC → GCS | Data chunk (max 90 bytes) |
| `LOG_REQUEST_END` | GCS → FC | Stop streaming |
| `LOG_ERASE` | GCS → FC | Erase **entire** DataFlash |

## Enumeration

After disarm + delay, send `LOG_REQUEST_LIST(start=0, end=0xFFFF)` and collect all `LOG_ENTRY` messages until idle (~1s with no new entries).

## Download

For each log not confirmed as duplicate:

1. Stream `LOG_REQUEST_DATA` / `LOG_DATA` into a `.partial` file
2. Track `received_ranges`; on timeout, re-request only missing gaps
3. Verify total bytes == `LOG_ENTRY.size`

## Deduplication Probe

When `(fc_log_id, fc_log_size)` matches a catalog row:

1. Download first `min(probe_bytes, log_size)` bytes
2. SHA-256 and compare to stored `probe_sha256`
3. Skip full download on match

## Erase

`LOG_ERASE` is sent **once** after **all** logs in the enumeration are archived. It has no per-log ID — it clears all FC logs.

---

See also: [durability.md](durability.md) · [architecture.md](architecture.md)

Distributed under the [Angad Singh Personal & Non-Commercial Source Available License](../LICENSE).
