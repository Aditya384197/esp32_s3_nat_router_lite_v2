# esp32_s3_nat_router_lite

High-performance Wi-Fi NAT router firmware for ESP32-S3 (N16R8: 16MB flash,
8MB PSRAM), ESP-IDF v5.5.4. Connects to an existing Wi-Fi network as a
station (STA) and re-shares it over its own access point (AP) with NAPT,
tuned for throughput on the S3's radio and dual cores.

## Build

Canonical build: GitHub Actions (`.github/workflows/build-esp32s3.yml`),
which pins ESP-IDF v5.5.4. Locally:

```
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

PlatformIO also works (`platformio.ini`) for local iteration, but see the
comment in that file about version parity with CI.

## First boot

1. Flash the firmware. The device comes up as an open AP named
   `ESP32_NAT_Router` at `192.168.4.1`.
2. Connect to that AP and open `http://192.168.4.1/`.
3. **Set an admin password first** (the dashboard will prompt for this).
   Until an admin password exists, the uplink/AP save endpoints are
   locked to prevent anyone else who joins the AP from hijacking your
   Wi-Fi credentials before you do.
4. Scan and connect to your real Wi-Fi network under "Internet Uplink".
   Optionally rename the AP and set an AP password under "Access Point".

All settings persist in NVS across reboots and OTA updates.

## Security notes

- Config-changing endpoints (`/api/connect`, `/api/ap`, `/api/scan`,
  and `/api/setup` once configured) require HTTP Basic Auth against the
  admin credentials set on first boot. Read-only status (`/api/status`)
  does not, so the dashboard can show live stats before you sign in.
- Credentials are stored in NVS on flash, unencrypted by default (same
  as the Wi-Fi credentials themselves). If this router will run
  somewhere you don't physically trust, enable ESP-IDF flash encryption
  (`CONFIG_SECURE_FLASH_ENC_ENABLED`) — not enabled here by default
  since it involves burning eFuses and is effectively irreversible.
- The STA interface accepts open/weak-security uplinks by design
  (`threshold.authmode = WIFI_AUTH_OPEN`), so it can join open hotspots.
  Don't rely on it to refuse a spoofed open AP with the same SSID as a
  secured one you intended to join.

## Throughput tuning (`sdkconfig.defaults`)

The two changes with the largest real-world impact:

- **`esp_wifi_set_ps(WIFI_PS_NONE)`** (in `esp32_nat_router.c`) — modem
  sleep is on by default and adds latency to every RX/TX. A
  mains-powered router has no reason to save power at the cost of
  forwarding speed.
- **`CONFIG_ESP_WIFI_RX_BA_WIN=32`** — previously only TX aggregation
  was tuned (`TX_BA_WIN=32`) while RX sat at the default of 6, an
  asymmetric config that specifically capped download-side throughput
  (uplink → AP clients). Matched to TX, using the S3's 8MB PSRAM.

Secondary: `CONFIG_LWIP_TCPIP_RECVMBOX_SIZE` / `UDP_RECVMBOX_SIZE`
raised to 64 (every forwarded packet passes through the tcpip-thread
input queue, so this isn't just a local-socket setting), and
`CONFIG_LWIP_TCP_SND/WND_BUF_DEFAULT` raised for the router's own local
sockets (HTTP UI) — this does *not* affect NAT-forwarded traffic, since
NAPT is IP-layer forwarding rather than a local socket. WiFi and
app/httpd tasks are pinned to opposite cores so a busy config-page
request can't stall packet forwarding.

`CONFIG_LWIP_L2_TO_L3_COPY=y` is required for NAPT to function
correctly on ESP-IDF's lwIP fork — it is not a stability nicety and
should not be disabled to "save" the copy.

## Known limitations

- No port forwarding / port mapping UI yet (`LWIP_IPV4_NAPT_PORTMAP` is
  available in lwIP but not exposed in the API).
- Byte counters (`sta_bytes_sent/received`) are not lock-protected
  against torn reads on the display page — deliberate: they sit in the
  per-packet forwarding hot path, and locking there would cost real
  throughput to protect a cosmetic number that self-corrects on the
  next 3s status poll.

## Changelog notes (second pass)

- **CI merge-bin fix**: `idf.py merge-bin -o <path>` resolves `<path>`
  relative to the directory idf.py was invoked from (project root),
  *not* automatically inside `build/`, even though the tool's own
  default (no `-o`) output does land in `build/`. The workflow now
  passes `-o build/esp32s3_nat_router_merged.bin` explicitly so the
  subsequent `cp` step finds it.
- **Config mutex hardening**: the ssid/passwd/ap_ssid/ap_passwd/admin
  mutex is now created deterministically by `wifi_config_init()`,
  called as the first line of `app_main()` before any other task
  exists, instead of being lazily created on first use (which was safe
  in practice but not race-free by construction).
- **JSON escaping**: `json_escape()` now covers the full JSON
  control-character range (RFC 8259 §7), not just `"` and `\`. A
  crafted/malformed SSID containing a raw newline or other control
  byte could previously have produced invalid JSON in `/api/status`
  and `/api/scan`.

## Final 24x7 review fix
This revision adds serialization for deferred Access Point reconfiguration. Rapid repeated Save AP requests can no longer create multiple concurrent FreeRTOS tasks that race each other while applying `esp_wifi_set_config(WIFI_IF_AP, ...)`. The latest settings remain persisted in NVS, while at most one apply operation is active at a time.
