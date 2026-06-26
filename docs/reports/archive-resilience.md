# Archive Resilience Implementation Report

> Copyright (c) 2026 Angad Singh Bains. All rights reserved.
> [MAVLink Companion Log Service](https://github.com/Angad7600123/Mavlink-companion-log-service)

**Date:** 2026-06-26  
**Scope:** Phases 1–3 — clean exit & state reset, cooperative cancellation, conditional transport reconnect  
**Follow-up to:** [`log-download-robustness.md`](log-download-robustness.md)

---

## 1. Executive summary

This change set makes the post-disarm archive cycle **self-recovering**. Before, a stuck
download could hang for minutes, a re-arm during download was ignored, and the only
reliable recovery was a `systemctl restart`.

After this work:

- Every archive exit path releases the FC log session (`LOG_REQUEST_END`) and clears
  queued state — guaranteed via RAII.
- Failures are **bounded in time** by timeout + retry + a new forward-progress abort.
- Re-arm / new disarm during a download **cancels** the transfer cooperatively.
- The transport is reconnected **only** when evidence points to a link fault, or after a
  configurable number of consecutive failures — not on every failure (per design review).
- Structured, single-line stall logs make field debugging straightforward.

No flight-control surface is touched: mcls still only sends log-protocol messages.

---

## 2. Phases delivered

### Phase 1 — Clean exit and state reset

| Item | Implementation |
|------|----------------|
| `LOG_REQUEST_END` on every exit | `archiveAll()` wraps the cycle in an RAII `ScopeExit` that always runs `clearDataChunks()` + `requestLogEnd()` |
| Clear queued download state | `clearDataChunks()` on cycle end, between retries, and at download start |
| Return to idle after failure | All download paths return promptly; `Cleanup` always follows `Archive` |
| Forward-progress detection | Per-attempt `no_progress_attempts`; abort when offset does not advance after `stall_abort_attempts` |
| Bounded queue | `max_queued_log_data` (default 256); oldest dropped on overflow |
| Better logging | `logStall()` emits `log= offset= expected= received= attempt= reason=` |

### Phase 2 — Cooperative cancellation

| Item | Implementation |
|------|----------------|
| Cancel token | `std::atomic<bool> cancel_requested_` in `LogDownloader` + `requestCancel()/resetCancel()/cancelled()` |
| Poll points | Top of attempt loop, inside gap loop, inside `waitForLogData()` wait (≤50 ms response) |
| Re-arm during archive | `onFlightEvent(VehicleArmed)` → `requestCancel()`; after cleanup → `WaitDisarm` |
| New disarm during archive | `onFlightEvent(VehicleDisarmed)` → `requestCancel()` + `archive_requested_`; after cleanup → fresh cycle |
| Shutdown | `stop()` / end of `run()` calls `requestCancel()` |
| Erase safety | `EraseAll` checks `cancelled()`; cancelled cycles never send `LOG_ERASE` |
| State race | `state_` is now `std::atomic<State>` (read on RX thread, written on main thread) |

### Phase 3 — Conditional transport reconnect (revised per review)

| Item | Implementation |
|------|----------------|
| Failure taxonomy | `ArchiveFailureReason` enum + `classifyTimeout()` distinguishes `LogDataTimeout` vs `LinkTimeout`/`TransportClosed`/`TransportSendFailed` |
| Rule B — transport class | Reconnect immediately when `isTransportFailure(reason)` and `reconnect_on_transport_failure` |
| Rule A — streak | Reconnect after `reconnect_after_consecutive_failures` failed cycles (`0` disables) |
| Never on benign | `LogDataTimeout`, `IncompleteDownload`, `VerificationFailed`, `Cancelled` do **not** force reconnect on their own |
| Streak reset | Reset to 0 on a fully successful cycle or after a reconnect attempt |
| Cancellation is not a failure | Cancelled cycles do not increment the streak or trigger reconnect |

---

## 3. Failure reasons

`ArchiveFailureReason` (in `Types.hpp`):

| Reason | Meaning | Transport-class? |
|--------|---------|------------------|
| `LogDataTimeout` | No `LOG_DATA` for offset, link still alive | No |
| `EmptyPayload` | Matching `LOG_DATA` had no usable bytes | No |
| `NoProgress` | Offset failed to advance after N attempts | No |
| `IncompleteDownload` | Retries exhausted, gaps remain | No |
| `VerificationFailed` | Byte count ≠ `LOG_ENTRY.size` | No |
| `ParseFailed` | DataFlash structural validation failed | No |
| `RereadMismatch` | FC sample re-read disagrees with local file | No |
| `OverlapConflict` | Conflicting payload at same chunk offset | No |
| `StorageError` | Local file/storage failure | No |
| `Cancelled` | Aborted by arm/disarm/shutdown | No |
| `TransportSendFailed` | `sendMessage`/`sendto` failed | **Yes** |
| `TransportClosed` | Transport not connected | **Yes** |
| `LinkTimeout` | No inbound MAVLink within heartbeat window | **Yes** |

`classifyTimeout()` maps a `LOG_DATA` wait timeout to one of `TransportClosed`,
`LinkTimeout`, or `LogDataTimeout` so a stalled transfer with a healthy link does **not**
masquerade as a transport fault.

---

## 4. Configuration added (`[download]`)

```toml
stall_abort_attempts = 3
max_queued_log_data = 256
reconnect_on_transport_failure = true
reconnect_after_consecutive_failures = 3   # 0 = disabled
```

Existing keys unchanged: `download_timeout`, `retry_count`, `retry_delay`,
`delay_after_disarm`, `probe_bytes`, `verify_after_download`, `erase_after_success`.

---

## 5. Control-flow summary

```text
Disarm
  → Delay (resetCancel, consume archive_requested_)
  → Enumerate
  → Archive: enumerateLogs() + archiveAll()
        archiveAll: RAII session guard (LOG_REQUEST_END on exit)
          per log: archiveOne → downloadLogData
            cancel checks, stall/no-progress abort, reason set
  → (cancelled?  → Cleanup, no erase)
    (all archived && erase enabled? → EraseAll → Cleanup)
    (failed? → Cleanup)
  → Cleanup:
        storage limit, stats
        evaluateArchiveOutcome():
          success → reset streak
          cancelled → no-op
          transport-class failure → reconnect
          streak >= threshold → reconnect
          else → skip reconnect (log reason + counter)
        next: archive_requested_ → Delay (re-run)
              else armed → WaitDisarm
              else → WaitArm
```

---

## 6. Worst-case failure time (bounded)

For a stuck offset with a live link (`LogDataTimeout`):

```
attempts = retry_count + 1
per_attempt ≈ download_timeout + retry_delay
```

| Config | Bound |
|--------|-------|
| defaults (5 s / 3 / 2 s) | ≈ 4 × (5 + 2) = 28 s |
| 15 s / 5 / 2 s | ≈ 6 × (15 + 2) = 102 s |

Forward-progress abort (`stall_abort_attempts`) can cut this shorter when no bytes arrive
at all. This replaces the previous unbounded ~10-minute stall.

---

## 7. Files changed

| File | Change |
|------|--------|
| `include/mcls/Types.hpp` | `ArchiveResult::Cancelled`, `ArchiveFailureReason`, `toString`, `isTransportFailure`, extended `ArchiveCycleResult` |
| `include/mcls/Config.hpp` | 4 new `DownloadSettings` fields |
| `src/Config.cpp` | Parse the 4 new keys |
| `include/mcls/LogDownloader.hpp` | Cancel API, `classifyTimeout`, `logStall`, `requestLogData` returns `bool`, members |
| `src/LogDownloader.cpp` | RAII session guard, cancel polling, stall/no-progress abort, reason classification, structured logs |
| `include/mcls/DroneLogService.hpp` | `std::atomic<State>`, `isArchiveBusy`, `evaluateArchiveOutcome`, cycle/streak members |
| `src/DroneLogService.cpp` | Cancel on arm/disarm, re-run scheduling, conditional reconnect, cancelled-aware erase |
| `config/config.toml`, `docs/configuration.md`, `CHANGELOG.md` | Document new keys and recovery behavior |

---

## 8. Behavior matrix

| Scenario | Before | After |
|----------|--------|-------|
| Zero-length `LOG_DATA` loop | Infinite, 0-byte partial, ~10 min | Rejected; bounded failure |
| `LOG_DATA` timeout | Eventual timeout, restart often needed | Bounded retries → abort → `WaitArm` |
| Re-arm during download | Ignored; stuck busy | Cancelled; back to `WaitDisarm` |
| New disarm during download | Queued behind stuck transfer | Cancel + fresh cycle |
| Service stop during download | Waited for loop | Immediate cancel + clean exit |
| Transport send failure | Kept retrying | Reconnect (Rule B) |
| 3 consecutive failed cycles | No action | Reconnect (Rule A) |
| Pure FC/protocol stall | (n/a) | No reconnect; logged with reason |

---

## 9. Verification status

- **Static review:** Logic reviewed; remaining linter messages on this dev host are
  include-path resolution only (no CMake include dirs for `mcls/*` and `ardupilotmega`),
  not code errors.
- **Build:** Not compiled on the Windows dev host (CMake unavailable). Build and run on
  the Pi:
  ```bash
  cmake -S . -B build -DMCLS_BUILD_TESTS=ON
  cmake --build build --parallel
  ctest --test-dir build --output-on-failure
  ```

### Suggested bench acceptance tests

1. Block `LOG_DATA` on 14660, keep heartbeats → fail within bound; reason `log_data_timeout`; **no** reconnect; `WaitArm`.
2. Three failed cycles → reconnect on the 3rd (Rule A).
3. Force socket error → reconnect on 1st (Rule B).
4. Re-arm during download → `cancelled`, no `LOG_ERASE`, `WaitDisarm`.
5. Disarm again during download → fresh cycle after cleanup.
6. `systemctl stop` during download → clean exit; `LOG_REQUEST_END` on 14661 (tcpdump).

---

## 10. Remaining (optional) future work

- **True non-blocking archive:** archive still runs synchronously on the main loop;
  cancellation is cooperative (≤ one wait/timeout window of latency). A worker-thread
  model would make cancellation instant but adds threading complexity around
  `MavlinkClient`. Deferred unless cooperative latency proves insufficient.
- **Empirical wire confirmation:** capture `LOG_DATA` during a real failed cycle
  (`tcpdump -i lo -n udp port 14660 -X`) to attribute root cause to FC vs forwarder.

---

## 11. Deploy

```bash
cd ~/Mavlink-companion-log-service
git pull
./scripts/update.sh
ls -la /usr/local/bin/mcls   # must not be 0 bytes
sudo systemctl restart mavlink-companion-log-service
sudo journalctl -u mavlink-companion-log-service -f
```

Config in `/etc/mcls/config.toml` is preserved; new keys are optional and default safely.
