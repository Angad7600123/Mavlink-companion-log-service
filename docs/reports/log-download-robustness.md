# Log Download Robustness Report

> Copyright (c) 2026 Angad Singh Bains. All rights reserved.
> [MAVLink Companion Service](https://github.com/Angad7600123/Mavlink-companion-log-service)

**Date:** 2026-06-26  
**Scope:** Stalled log download after disarm (0-byte `.partial`, ~10 minute delay before first timeout)

---

## Executive summary

A post-disarm archive cycle could stall indefinitely at `Downloading log N` with a
0-byte partial file and no timeout logs for many minutes. Code inspection identified
a **client-side infinite loop** when `waitForLogData()` accepted zero-length `LOG_DATA`
chunks as successful progress.

This patch hardens `LogDownloader` so zero-length payloads cannot advance the gap
loop, failed transfers send `LOG_REQUEST_END` to release the FC log session, and
retries follow predictable timing bounded by `download_timeout` and `retry_count`.

---

## Incident timeline (field observation)

| Time | Event |
|------|--------|
| Disarm | Archive cycle starts |
| +2 s | Enumerate → `Found 1 log(s)` |
| | `Downloading log 1 (1427996 bytes)` |
| ~10 min | First `Timeout waiting for LOG_DATA at offset 0` |
| | 0-byte `.partial` in `/var/lib/mcls/tmp/` |
| Manual restart | Download succeeded on next cycle |

**Key anomaly:** First timeout ~10 minutes after download start, not after
`download_timeout` (5–15 s). That implies the code was **not** in the timeout-failure
path during those minutes.

---

## Root cause analysis

### Proven from source (before patch)

1. **`waitForLogData()`** returned `true` on matching `id` + `offset` with **no check**
   on payload size.

2. **`onMessage()`** queued `LOG_DATA` even when `count == 0`, producing empty chunks.

3. **`downloadLogData()` inner loop** advanced `gap_current` / `gap_remaining` only by
   `chunk.size()`. Zero-size chunks → **no progress**, no timeout log, tight loop:
   `requestLogData` → `waitForLogData` (instant success) → repeat.

4. **`ReceivedRanges::add(..., 0)`** is explicitly a no-op.

### Not required to prove for the patch

Whether ArduPilot or the companion forwarder **sent** `count == 0` frames on the wire
was not captured during the stall. The client bug was real regardless: any source of
matching empty chunks produced the same hang.

### Why restart “fixed” it

- Aborted the hung in-process archive (main thread blocked in `downloadLogData`)
- Cleared UDP/parser/chunk state
- `cleanupPartials()` removed stale `.partial` on startup
- Fresh disarm cycle + `LOG_REQUEST_END` after enumerate reset FC log session

---

## Changes implemented

### 1. Reject zero-length `LOG_DATA` at ingress (`onMessage`)

- Drop packets with `count == 0` or empty payload
- Log a warning with log id and offset
- Cap queued chunks at 256 (drop oldest) to limit mismatched-traffic buildup

### 2. Harden `waitForLogData()`

- On id/offset match with empty payload: discard chunk, **keep waiting** until deadline
- Only return `true` for non-empty payloads

### 3. Defensive checks in download loops

- `downloadRange()` and `downloadLogData()` treat empty post-wait chunks as failure
  (same path as timeout)

### 4. `abortLogTransfer(reason)`

- Clears queued chunks
- Sends **`LOG_REQUEST_END`** to release FC log transfer state
- Called on:
  - Incomplete full log download
  - Verification failure
  - Failed probe/range download

### 5. Retry hygiene

Between retry attempts during full log download:

- `clearDataChunks()`
- `LOG_REQUEST_END`
- `retry_delay` sleep

Ensures each attempt starts from a clean client and FC session state.

### 6. Related fixes already in tree (same release)

| Area | Fix |
|------|-----|
| UDP transport | Full-datagram buffering (not 1-byte `recvfrom`) |
| Link liveness | Any inbound MAVLink frame refreshes link timer |
| Archive state | No transport disconnect during archive |
| Install | Preserve `/etc/mcls/config.toml`; `scripts/update.sh` for binary-only updates |
| Install safety | Refuse to install empty `build/mcls` binary |

---

## Expected behavior after patch

### Successful download

```text
Downloading log 1 (1427996 bytes)
Log 1 progress: 65536/1427996 bytes
...
Downloaded 1427996 bytes for log 1
Verification successful for log 1
Archive cycle: downloaded=1, skipped=0, failed=0
```

### Failed download (bounded time)

With defaults `download_timeout = 5`, `retry_count = 3`, `retry_delay = 2`:

- First timeout log within **~5 s** of each failed chunk request (not ~10 min)
- After all retries: `Incomplete download for log N`
- `Aborting FC log transfer: incomplete download for log N`
- `Download failed for log N`
- `Archive cycle: downloaded=0, skipped=0, failed=1`
- Service returns to **WaitArm** (no manual restart required)

Worst-case time to give up on offset 0 (no data at all):

```text
4 attempts × (download_timeout + retry_delay) ≈ 4 × (5 + 2) = 28 s   (defaults)
```

With `download_timeout = 15`, `retry_count = 5`:

```text
6 × (15 + 2) ≈ 102 s
```

---

## Operational recommendations

### Config (Pi / UDP 14660–14661)

```toml
[transport]
transport = "udp"
host = "127.0.0.1"
port = 14661
bind_host = "127.0.0.1"
bind_port = 14660

[download]
delay_after_disarm = 5
download_timeout = 15
retry_count = 5
retry_delay = 2
erase_after_success = false   # until archives verified
```

### Forwarder

Ensure **MclsForwarder** duplicates **`LOG_DATA`** to 14660, not only `LOG_ENTRY` /
`HEARTBEAT`.

### Verify stall on wire

```bash
sudo tcpdump -i lo -n udp port 14660 -X -c 5
```

During download, expect MAVLink `LOG_DATA` frames with non-zero payload length.

### Update on Pi

```bash
cd ~/Mavlink-companion-log-service
git pull
./scripts/update.sh
ls -la /usr/local/bin/mcls    # must not be 0 bytes
sudo systemctl restart mavlink-companion-log-service
```

---

## Files modified (this patch)

| File | Change |
|------|--------|
| `include/mcls/LogDownloader.hpp` | `abortLogTransfer()`, `clearDataChunks()` |
| `src/LogDownloader.cpp` | Zero-length rejection, abort/end-on-failure, retry hygiene |

---

## Remaining limitations (future work)

> **Update:** The cancellation and reconnect items below were subsequently implemented.
> See [`archive-resilience.md`](archive-resilience.md) (Phases 1–3).

- ~~Archive runs synchronously; re-arm/disarm does not cancel an in-flight transfer.~~
  Now cancelled cooperatively (Phase 2).
- ~~No automatic transport reconnect after repeated failures.~~ Now conditional on
  transport-class failures or N consecutive failures (Phase 3).
- Archive still runs on the main thread; cancellation latency is at most one
  timeout/wait window. A worker-thread model remains optional future work.
- Empirical confirmation of `count == 0` on the wire during the original stall was not
  captured; tcpdump during a failed cycle would close that gap.

---

## Conclusion

The ~10-minute silent stall was consistent with a **non-timeout code path** (zero-progress
success loop), not with misconfigured `download_timeout`. The patch closes that loop,
bounds failure time, and releases the FC log session on abort so the **next disarm cycle
can recover without systemd restart**.
