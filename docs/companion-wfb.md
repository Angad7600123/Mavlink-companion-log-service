# Companion Control API over WFB

> Copyright (c) 2026 Angad Singh Bains. All rights reserved.
> [MAVLink Companion Log Service](https://github.com/Angad7600123/Mavlink-companion-log-service)
> · [README](../README.md) · [LICENSE](../LICENSE)

mcls exposes a **bidirectional JSON control API** over localhost UDP.
On the Pi, `wfb-ng` bridges that UDP port to a dedicated WFB radio stream,
so an Android ground station (running the companion WFB transport layer)
can poll status and send commands without any TCP/IP stack or tunnel.

---

## Architecture

```
Android                          Pi (air unit)
──────────────────────────────   ──────────────────────────────────────
CompanionTransport               wfb-ng companion stream
  send stream 0xc0 ──────────►  stream_rx 0xc0 → sendto 127.0.0.1:14540
  recv stream 0x40 ◄──────────  stream_tx 0x40 ← recvfrom 127.0.0.1:14540
                                         ↑↓
                                 CompanionUdpServer (mcls)
                                   binds 127.0.0.1:14541
                                   sends to 127.0.0.1:14540
                                         ↕
                                 DroneLogService (state + LogDownloader)
```

mcls does **not** decode WFB frames. It speaks plain JSON to localhost:14541,
the same way it speaks MAVLink to localhost:14660/14661. wfb-ng owns the radio
layer entirely.

---

## Radio port allocation

| Direction | Radio port | mcls role |
|-----------|------------|-----------|
| Pi → Android (downlink) | `0x40` | Response source |
| Android → Pi (uplink) | `0xc0` | Request source |

These ports sit outside the default video (`0x00`), MAVLink (`0x10`/`0x90`),
and tunnel (`0x20`/`0xa0`) streams so they will not collide with a stock
wfb-ng config that still runs IP tunnel.

---

## Pi wfb-ng configuration

Add the following stream to your existing `[drone]` `streams[]` list in
`/etc/wifibroadcast.cfg`:

```python
{'name': 'companion',
 'stream_rx': 0xc0,
 'stream_tx': 0x40,
 'service_type': 'udp_proxy',
 'profiles': ['base', 'drone_base', 'radio_base'],
 'peer': 'listen://127.0.0.1:14540'}
```

After editing, restart the wfb-ng service:

```bash
sudo systemctl restart wifibroadcast@drone
```

Requirements unchanged from the rest of your wfb setup:
- Same `wifi_channel` (default 165 / 5825 MHz)
- `link_domain = "default"`
- Matching `drone.key` / `gs.key` keypair

---

## mcls configuration

Enable the companion API in `/etc/mcls/config.toml`:

```toml
[companion]
enabled = true
bind_host = "127.0.0.1"
bind_port = 14541           # mcls binds here — NOT 14540 (owned by wfb-ng)
send_host = "127.0.0.1"
send_port = 14540           # wfb-ng listens here; mcls sends responses here
token = "your-secret"       # leave empty to disable auth (dev only)
max_request_bytes = 2048
max_response_bytes = 1200
max_fc_logs_per_response = 8
```

Restart the service after editing:

```bash
sudo systemctl restart mavlink-companion-log-service
```

---

## JSON protocol

One WFB datagram = one JSON message. Hard budget: `max_response_bytes = 1200`
(headroom under wfb-ng `radio_mtu` 1445). Requests and responses are opaque
UTF-8 JSON bytes from wfb-ng's perspective.

### Request envelope

```json
{"v":1,"id":42,"op":"status"}
{"v":1,"id":43,"op":"fc.logs","offset":0,"limit":8,"token":"your-secret"}
{"v":1,"id":44,"op":"archive.start","token":"your-secret"}
{"v":1,"id":45,"op":"archive.cancel","token":"your-secret"}
```

### Response envelope

```json
{"v":1,"id":42,"ok":true,"truncated":false,"err":null,"data":{...}}
```

### Operations

| `op` | Auth | Returns |
|------|------|---------|
| `status` | optional | Tier 1 snapshot (service state, link, vehicle, `job` descriptor, archive progress + `percent`/`bytes_per_sec`, storage) |
| `fc.logs` | optional | Paginated FC log list (`id`, `size`, `t`=time_utc, `dl`=downloaded) |
| `caps` | optional | Protocol version, supported `ops`, and `limits` (for client feature detection) |
| `archive.start` | token required if set | Idempotent job ack; queues a full archive cycle (download un-archived + erase) |
| `archive.cancel` | token required if set | Job ack (`accepted:true`); cancels whatever job is running (archive, download, transfer) — no-op when idle |
| `logs.refresh` | token required if set | Idempotent job ack; re-enumerates the FC log list |
| `logs.download` | token required if set | Download ack (`queued`, `not_found[]`); archives `sel.ids[]` or `sel.all` to the Pi — **no FC erase** |
| `logs.erase` | token required if set | Idempotent job ack; **super-delete** — unconditional full DataFlash wipe, cancels any in-flight job first |

Requests may carry an optional `"client"` string; when present it is echoed
verbatim in the response (future multi-GS response filtering).

`logs.download` request shapes: `{"sel":{"ids":[17,18]}}` (≤ 32 ids) or
`{"sel":{"all":true}}`. Job progress is polled via `status` (`job.type` =
`archive`/`refresh`/`download`/`erase`, plus `archive.percent` /
`archive.bytes_per_sec` for the byte-level progress of the current log).

`archive.start` is **idempotent by state**. Preconditions are evaluated when the
request is received and the cycle is queued only if accepted:

- **accepted** → `ok:true`, `data:{"accepted":true,"already_running":false}`
- **already running** → `ok:true`, `data:{"accepted":true,"already_running":true}` — a
  retry after a lost ack reads as **success**, never a second queued cycle
- **armed** → `err: armed`  ·  **not connected** → `err: not_connected`

There is no `busy` error: an already-running cycle is the desired state, so it is
reported as idempotent success rather than a failure.

### Tier 1 — `status` response (always fits in budget)

```json
{
  "service": {"state": "wait_arm", "version": "1.0.0"},
  "link": {"transport_connected": true, "heartbeat_fresh": true},
  "vehicle": {"detected": true, "armed": false},
  "fc_logs": {"count": 3, "stale": false},
  "archive": {
    "active": false,
    "current_log_id": 0,
    "progress_bytes": 0,
    "progress_total_bytes": 0,
    "percent": 0,
    "bytes_per_sec": 0,
    "last_cycle": {"downloaded": 2, "skipped": 1, "failed": 0,
                   "cancelled": 0, "all_archived": true}
  },
  "storage": {"used_bytes": 52428800, "limit_bytes": 1073741824, "archived_count": 47}
}
```

The `fc_logs.count` field is enough for most UI polls. Use `fc.logs` when you
need the actual id/size list (e.g. when the user opens a logs panel).

### Tier 2 — `fc.logs` paginated response

```json
{
  "count": 23,
  "offset": 0,
  "stale": false,
  "entries": [{"id": 17, "size": 4200000, "t": 1719950400, "dl": true},
              {"id": 18, "size": 1100000, "t": 1719954000, "dl": false}]
}
```

Each entry carries `t` (`LOG_ENTRY.time_utc`, 0 if unknown) and `dl` (`true` when the
log is already in the Pi archive catalog). Request with `offset`/`limit` to page through large lists.
`truncated: true` in the response means the page was reduced to fit the budget;
follow up with `fc.logs?offset=N` for the remainder.

### Error codes

| `err` | Meaning |
|-------|---------|
| `bad_token` | Token required but absent or wrong |
| `bad_request` | Unknown `op` or malformed JSON |
| `armed` | `archive.start` rejected — vehicle is armed |
| `not_connected` | `archive.start` rejected — MAVLink transport not connected |
| `internal` | Serialization or internal error |

(An already-running cycle is **not** an error — `archive.start` returns
`ok:true` with `already_running:true`. See Operations above.)

---

## Phase 0: validate before enabling mcls

Verify the wfb-ng UDP bridge is working with `socat` and `tcpdump`
before touching the mcls config:

**On the Pi (terminal 1) — watch traffic:**
```bash
sudo tcpdump -i lo -n udp port 14540 or udp port 14541 -X
```

**On the Pi (terminal 2) — simulate mcls sending to wfb-ng:**
```bash
# Send a fake JSON status response to wfb-ng inject port
echo -n '{"v":1,"id":1,"ok":true,"data":{"service":{"state":"wait_arm"}}}' \
    | socat - UDP:127.0.0.1:14540
```

**On the Pi (terminal 3) — simulate mcls receiving a request:**
```bash
# Listen on mcls bind port and print anything that arrives
socat UDP-RECVFROM:14541,fork STDOUT
```

If the tcpdump shows traffic on both ports, the wfb-ng bridge is working and
mcls will be able to send/receive over it.

---

## Phase 1: end-to-end with mcls enabled

1. Enable `[companion] enabled = true` in **`/etc/mcls/config.toml`** (not
   `sample_master_config.toml`) and restart `mclsd`.
2. Confirm diagnostic lines in the journal:

```bash
sudo journalctl -u mavlink-companion-log-service -n 40 --no-pager | grep -iE 'companion|mcls:'
```

Expected when working:

```
mcls: loading config from /etc/mcls/config.toml
mcls: [companion] enabled=true table_present=true
Companion config from /etc/mcls/config.toml: [companion] table present
Companion enabled=true bind=127.0.0.1:14541 send=127.0.0.1:14540 ...
Starting companion UDP server (bind 127.0.0.1:14541)
Companion UDP server listening on 127.0.0.1:14541, sending responses to ...
CompanionUdpServer: RX thread started, waiting for datagrams ...
```

If you only see `Companion API disabled` or `table absent`, the live config
file is wrong or missing `[companion] enabled = true`.

3. Send a status request from the Pi:
   ```bash
   echo -n '{"v":1,"id":1,"op":"status"}' | socat - UDP:127.0.0.1:14540
   ```
3. Check the journal for the companion server log line:
   ```bash
   sudo journalctl -u mavlink-companion-log-service -n 20 --no-pager | grep Companion
   ```
4. With Android WFB transport active, `CompanionTransport.send(statusRequestBytes)`
   should produce a response in the `CompanionManager` callback.

---

## Bandwidth notes

The companion stream shares the ~7 Mbit/s half-duplex WFB budget with video
and MAVLink. Keep messages small and infrequent:

- Poll `status` every 1–2s during active archive; every 5s otherwise
- Call `fc.logs` only when the UI needs the list
- Keep individual JSON payloads well under 1200 bytes (enforced by mcls)

---

See also: [architecture.md](architecture.md) · [configuration.md](configuration.md) · [protocol.md](protocol.md)

Distributed under the [Angad Singh Personal & Non-Commercial Source Available License](../LICENSE).
