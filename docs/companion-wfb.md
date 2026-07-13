# Companion Control API over WFB

> Copyright (c) 2026 Angad Singh Bains. All rights reserved.
> [MAVLink Companion Service](https://github.com/Angad7600123/Mavlink-companion-log-service)
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

`archive.cancel` takes effect **immediately** on the companion UDP thread — it does
not wait for the state machine to be between steps. A running job (archive,
manual download, enumeration) checks the cancel flag on every internal loop
iteration and unwinds within roughly a second; back-off waits (FC-busy,
enumeration retry) are interruptible too, so cancel is never delayed by more
than one ~100ms tick.
| `logs.refresh` | token required if set | Idempotent job ack; re-enumerates the FC log list |
| `logs.download` | token required if set | Download ack (`queued`, `not_found[]`); archives `sel.ids[]` or `sel.all` to the Pi — **no FC erase** |
| `logs.erase` | token required if set | Idempotent job ack; **super-delete** — unconditional full DataFlash wipe, cancels any in-flight job first |
| `rec.start` | token required if set | `{"active":true}`; starts onboard Pi video recording (see below) |
| `rec.stop` | token required if set | `{"active":false}`; stops onboard Pi video recording, no-op when idle |

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
  "job": {"active": false, "type": null},
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
  "storage": {"used_bytes": 52428800, "limit_bytes": 1073741824, "archived_count": 47},
  "recording": {"enabled": true, "active": false, "duration_sec": 0,
                "free_bytes": 12884901888, "error": null}
}
```

`job.type` is one of `archive`/`refresh`/`download`/`erase`, or `null` when idle — it
drives the `archive` block's byte-level progress regardless of which job is running.
`recording` is **independent** of `job`/`archive` — see [Onboard video
recording](#onboard-video-recording) below; it reflects the current state of
`rec.start`/`rec.stop` regardless of whatever archive job may also be running.

`recording.error` is `null` unless the recorder process exited **on its own**
(i.e. nobody called `rec.stop`) — the operator flying this remotely has no
other way to learn recording silently died. One of:

| Value | Meaning |
|-------|---------|
| `"media_lost"` | The recorder died and, at the moment of detection, `mount_path` was no longer writable/mounted (e.g. the USB drive was pulled) |
| `"recorder_crashed"` | The recorder died for some other reason; media was fine |

Sticky until the next successful `rec.start` clears it — so it stays visible
even if the app wasn't polling at the exact moment of failure.

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
| `not_found` | `logs.download` rejected — none of the requested ids exist on the FC |
| `recording_disabled` | `rec.start` rejected — `[recording] enabled = false` |
| `no_media` | `rec.start` rejected — `mount_path` missing, not writable, or not an actual mount point |
| `internal` | Serialization or internal error |

(An already-running cycle is **not** an error — `archive.start` returns
`ok:true` with `already_running:true`. See Operations above.)

---

## Onboard video recording

`[recording]` lets the app record the FPV feed **on the Pi** instead of on the
phone — pristine, since it taps the encoded stream before it crosses the lossy
WFB radio — as a single continuous file for the whole start/stop session,
written as MPEG-TS (a truncated `.ts` still plays everything up to the cut;
a truncated MP4 does not, since its index is written last).

MPEG-TS alone only protects the *format*; it does nothing about data that
never reached the physical drive at all. Two mechanisms close that gap:
periodic forced sync (bounds how much of the tail can be lost to a hard power
cut — see `fsync_interval_sec` below) and recommending ext4 for the media
(journaled, unlike FAT32/exFAT — see "Removable media" below). This is a
deliberate design point for a drone you cannot physically walk up to: the
recording must be recoverable after an uncontrolled power loss, not just after
a clean stop.

Fully independent of the archive/download state machine and
`CompanionCommandQueue` on purpose — `rec.start`/`rec.stop` must work
regardless of whatever archive job is running, so (like `archive.cancel`) they
are handled directly, synchronously, on the companion UDP thread rather than
being queued for the main loop to drain between state-machine steps.

### Camera-side setup (once, outside mcls)

Add a second `tee` branch to the camera's `gst-launch-1.0` pipeline: one branch
keeps going to wfb-ng (unchanged — live FPV), the other feeds a **local-only**
UDP port mcls reads from:

```bash
rpicam-vid -n --width 1920 --height 1080 --framerate 30 --bitrate 6000000 \
  -t 0 --inline --profile high --low-latency --intra 30 -o - \
| gst-launch-1.0 -v \
    fdsrc ! h264parse ! tee name=t \
    t. ! queue ! rtph264pay config-interval=1 pt=35 ! udpsink sync=false host=127.0.0.1 port=5602 \
    t. ! queue ! rtph264pay config-interval=1 pt=35 ! udpsink sync=false host=127.0.0.1 port=5603
```

Port `5602` is unchanged (→ wfb-ng → phone). Port `5603` is the recording tap
— loopback only, it never touches the WFB radio or its bandwidth budget, so
recording costs zero airtime. If nothing is listening on `5603` (recording
off), the packets are simply dropped — no backpressure, no risk to the live
branch.

### mcls-side setup

```toml
[recording]
enabled = true
mount_path = "/mnt/usb"     # wherever YOUR removable media is mounted
source_port = 5603          # must match the tee branch above
rtp_payload_type = 35       # must match that branch's pt=
fsync_interval_sec = 3      # forced sync while recording; bounds power-loss data loss
```

`mount_path` must already exist, be writable, **and be an actual mount
point** — mcls does not mount/unmount anything; set up a persistent,
self-healing mount for your USB drive outside mcls (see "Removable media"
below). `rec.start` returns `err:no_media` if the path is missing, not
writable, or is just a plain directory on the root filesystem with nothing
mounted on it yet (e.g. the drive was unplugged or hadn't finished mounting at
boot) — this matters because a writable-but-unmounted `mount_path` would
otherwise silently "succeed" by writing the recording onto the Pi's own root
filesystem instead of the removable media, so pulling the drive afterward
finds nothing on it.

### Removable media: format ext4, auto-mount by label (recommended)

Since the Pi is on a drone you cannot walk up to, the media has to keep
working through a USB reseat, a mid-flight power blip, or the drive simply
not being present at boot — without any operator action.

**Format ext4, not FAT32/exFAT.** FAT/exFAT has no journal: the *directory
entry* recording a file's size is metadata written lazily, separately from
the actual data blocks. A hard power cut can leave that metadata at its last
committed value — **0 bytes** — even though video data was physically
written to the flash. ext4 journals metadata, so (combined with
`fsync_interval_sec`) a power cut costs at most the last few seconds, never
the whole recording. Trade-off: ext4 isn't natively readable by double-clicking
the drive in Windows — pull footage over the network (`scp`/`rsync`) instead,
or use a WinFsp/ext4 driver if you need direct plug-in access.

```bash
# on the Pi, one-time setup — replace /dev/sda1 with your actual device (lsblk)
sudo mkfs.ext4 -L MCLSREC /dev/sda1
```

**Auto-mount on insert/reappear, no `mount`/`umount` commands ever needed
in the field.** A systemd `.mount` unit keyed to the filesystem label, plus a
udev rule that starts it whenever the device node appears:

`/etc/systemd/system/mnt-usb.mount`:
```ini
[Unit]
Description=MCLS recording media

[Mount]
What=/dev/disk/by-label/MCLSREC
Where=/mnt/usb
Type=ext4
Options=nofail,noatime

[Install]
WantedBy=multi-user.target
```

`/etc/udev/rules.d/99-mcls-usb.rules`:
```
ACTION=="add", SUBSYSTEM=="block", ENV{ID_FS_LABEL}=="MCLSREC", RUN+="/bin/systemctl --no-block start mnt-usb.mount"
```

```bash
sudo systemctl daemon-reload
sudo udevadm control --reload-rules
```

Now: unplug the drive, plug it back in (even mid-flight, even after it was
never mounted at boot) — udev fires, systemd mounts it at `/mnt/usb` within a
second or two, no SSH session required. The next `rec.start` from the app
(over the radio link) succeeds as soon as the mount lands; until then it
correctly returns `err:no_media` instead of writing to the wrong filesystem
(see the mount-point check above).

### Mechanics

- `rec.start` spawns a short-lived `gst-launch-1.0` subprocess (`udpsrc port=<source_port> ! rtph264depay ! h264parse ! mpegtsmux ! filesink location=<mount_path>/<filename_prefix>_<timestamp>.ts`) via `posix_spawn` — not `fork()`, since mcls is multi-threaded and `fork()` in a multi-threaded process only duplicates the calling thread, a known hazard `posix_spawn` avoids. The pipeline deliberately omits `rtpjitterbuffer`: the recording tap is a loopback (127.0.0.1) source with no jitter or loss, and a standalone jitterbuffer there wedges after a few buffers on clock/latency negotiation (0-byte `.ts`) instead of helping.
- While active, a detached thread calls the global `sync()` every `fsync_interval_sec` seconds (default 3, `0` disables). The recording file is written by the gst-launch-1.0 subprocess, not mcls itself, but `sync()` flushes all dirty pages system-wide regardless of which process dirtied them — this is what actually bounds power-loss data loss; MPEG-TS's truncation tolerance only helps for bytes that made it to the physical device.
- `rec.stop` sends `SIGINT` and returns almost immediately; a detached background watcher confirms the exit (or escalates to `SIGKILL` after ~2s) without blocking the companion UDP thread, then runs a final `sync()` so the just-finalized file is flushed promptly rather than waiting for the next periodic tick.
- If the recorder process dies on its own (e.g. the USB drive is pulled mid-flight), the next `status` poll detects it, `recording.active` flips back to `false`, `recording.error` is set to `media_lost`/`recorder_crashed` (see the status format above), and the periodic sync thread stops. **No automatic restart** — the operator has to notice (now that the app surfaces `recording.error`) and send a fresh `rec.start`; a resume could only begin a new file anyway, since there's an unavoidable footage gap for however long the media was unreachable.
- A clean `mcls` shutdown stops any active recording (best-effort) and flushes it the same way. `recording.error` is only ever set for an *unrequested* exit — a normal `rec.stop` never sets it.

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
