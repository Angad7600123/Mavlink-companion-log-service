# Configuration

> Copyright (c) 2026 Angad Singh Bains. All rights reserved.
> [MAVLink Companion Service](https://github.com/Angad7600123/Mavlink-companion-log-service)
> · [README](../README.md) · [LICENSE](../LICENSE)

Configuration file: `/etc/mcls/config.toml` (override path via CLI argument).

A master reference with every key, default value, and inline comments lives at
[`config/sample_master_config.toml`](../config/sample_master_config.toml) (installed
to `/etc/mcls/sample_master_config.toml`). Use it when exploring options; copy
sections into your live config as needed.

The legacy `[mavlink]` table is still accepted as an alias for `[transport]`.

## `[transport]`

| Key | Default | Description |
|-----|---------|-------------|
| `transport` | `tcp` | Transport type: `tcp` or `udp` |
| `host` | `127.0.0.1` | Remote host for MAVLink endpoint |
| `port` | `5760` | Remote port for MAVLink endpoint |
| `bind_host` | `0.0.0.0` | UDP only: local bind address |
| `bind_port` | `0` | UDP only: local bind port (`0` = ephemeral) |
| `heartbeat_timeout_sec` | `5` | Link lost if no HEARTBEAT within this window |

### TCP example

```toml
[transport]
transport = "tcp"
host = "127.0.0.1"
port = 5760
```

### UDP example

```toml
[transport]
transport = "udp"
host = "127.0.0.1"
port = 14550
bind_host = "0.0.0.0"
bind_port = 0
```

## `[download]`

| Key | Default | Description |
|-----|---------|-------------|
| `delay_after_disarm` | `2` | Seconds to wait after disarm before enumerating |
| `download_timeout` | `5` | Seconds to wait for each LOG_DATA response |
| `retry_count` | `3` | Retries per gap on timeout |
| `retry_delay` | `2` | Seconds between retries |
| `probe_bytes` | `51200` | Bytes fetched for dedup probe (clamped to log size) |
| `verify_after_download` | `true` | Verify received byte count matches LOG_ENTRY.size (always-on completeness) |
| `erase_after_success` | `true` | Send LOG_ERASE after full cycle succeeds |
| `stall_abort_attempts` | `3` | Abort a log if the byte offset does not advance after this many attempts |
| `max_queued_log_data` | `2048` | Max queued LOG_DATA chunks before dropping oldest (bounds memory) |
| `gap_fill_idle_ms` | `500` | Idle time before merged gap-fill re-requests during streaming |
| `verify_dataflash_parse` | `true` | Run standalone DataFlash structural validation before persist |
| `verify_max_bad_header_ratio` | `0.001` | Max resync-bytes / file-size ratio during parse |
| `detect_overlap_conflict` | `true` | Fail on conflicting duplicate chunk payloads |
| `verify_fc_reread` | `sample` | FC cross-check: `none`, `sample`, or `full` double-download |
| `verify_fc_reread_sample_count` | `64` | Sample re-read count (7 anchors + random, deduped) |
| `benchmark_download` | `false` | Periodic throughput/profiling logs during download |
| `reconnect_on_transport_failure` | `true` | Reconnect transport on transport-class failures (send fail, closed, link timeout) |
| `reconnect_after_consecutive_failures` | `3` | Reconnect after N consecutive failed cycles (`0` = disabled) |

### Recovery behavior

- A failed download always sends `LOG_REQUEST_END` and clears queued state before
  returning to idle, so the next disarm can retry without a service restart.
- Zero-length / empty `LOG_DATA` is rejected and never counts as progress.
- Re-arm or a new disarm during a download **cancels** the in-flight transfer; a new
  disarm schedules a fresh cycle.
- Transport reconnect is **conditional**: it fires on transport-class failures, or after
  `reconnect_after_consecutive_failures` failures — not on every download failure.

## `[storage]`

| Key | Default | Description |
|-----|---------|-------------|
| `directory` | `/var/lib/mcls` | Root state directory |
| `max_size_gb` | `1` | Max verified archive size before deleting oldest |

## `[logging]`

| Key | Default | Description |
|-----|---------|-------------|
| `verbose` | `true` | Enable debug logging |
| `file` | *(empty)* | Optional log file path |

## `[companion]`

Optional JSON control API over localhost UDP for Android / wfb-ng. See
[companion-wfb.md](companion-wfb.md).

| Key | Default | Description |
|-----|---------|-------------|
| `enabled` | `false` | Enable companion UDP JSON API |
| `bind_host` | `127.0.0.1` | Local address mcls binds for inbound requests |
| `bind_port` | `14541` | Local port mcls binds (not 14540 — owned by wfb-ng) |
| `send_host` | `127.0.0.1` | Address for outbound responses |
| `send_port` | `14540` | Port for outbound responses (wfb-ng inject) |
| `token` | *(empty)* | Auth token for `archive.start` / `archive.cancel` (empty = no auth) |
| `max_request_bytes` | `2048` | Drop requests larger than this |
| `max_response_bytes` | `1200` | Cap response size for WFB MTU headroom |
| `max_fc_logs_per_response` | `8` | Max entries per `fc.logs` page |

---

See also: [architecture.md](architecture.md) · [README](../README.md)

Distributed under the [Angad Singh Personal & Non-Commercial Source Available License](../LICENSE).
