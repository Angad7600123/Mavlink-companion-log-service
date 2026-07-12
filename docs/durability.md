# Durability & Crash Safety

> Copyright (c) 2026 Angad Singh Bains. All rights reserved.
> [MAVLink Companion Service](https://github.com/Angad7600123/Mavlink-companion-log-service)
> · [README](../README.md) · [LICENSE](../LICENSE)

## Write Ordering (per log)

```
.partial file
  → flush()
  → fsync(file)
  → rename → .bin
  → fsync(parent directory)
  → SQLite COMMIT
```

After **every** log in the cycle completes this pipeline:

```
LOG_ERASE  (single message, wipes entire FC DataFlash)
```

## Power Loss Scenarios

| When power lost | Result |
|-----------------|--------|
| During download | `.partial` deleted on next boot; FC still has log; re-download |
| After rename, before SQLite COMMIT | FC still has log; catalog row missing; re-download |
| After SQLite COMMIT, before LOG_ERASE | FC still has log; dedup skips re-download |
| After LOG_ERASE | All archives durable; nothing lost |

## No Cross-Reboot Resume

Partial downloads are **not** resumed. The FC is the backup until archive succeeds. Simpler and safe.

## SQLite

```sql
PRAGMA journal_mode=WAL;
PRAGMA synchronous=FULL;
```

## Storage Cleanup

Only verified `.bin` files under `logs/` are eligible for eviction. Never delete `.partial` files via eviction (leftover partials are removed on startup).

## SD Card Protection

All service writes go to `/var/lib/mcls/` only. Atomic rename + fsync minimizes corruption risk on sudden power loss.

---

See also: [protocol.md](protocol.md) · [architecture.md](architecture.md)

Distributed under the [Angad Singh Personal & Non-Commercial Source Available License](../LICENSE).
