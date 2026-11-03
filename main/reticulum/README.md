# Reticulum extension boundary

No device-side client is linked in v0.1. The navigation page is intentionally an
unavailable/module-information state. `../../bridge/bridge.py` is independent and
can be deployed/tested without the ESP32.

A future client must use a dedicated HTTP worker, certificate-verified HTTPS and
the configured Bearer token; cap JSON responses at 16 KiB (bridge supports up to
32 interfaces), validate schema 1 and publish typed snapshots over a bounded
queue. Never perform requests in LVGL callbacks. Expire snapshots after 3 missed
polls; derive rates only from consecutive real counters, handling resets.

Do not equate interface clients, known destinations or LXMF peers. Null means
unsupported/unavailable, not zero. LXMF requires a separately authorized source.
