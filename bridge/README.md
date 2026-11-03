# Read-only Reticulum bridge

`bridge.py` is standalone Python>=3.10 with **no pip dependencies**. Reticulum
itself must already be installed/running on the Linux machine. The ESP32 does
**not** run RNS. The firmware-side bridge client is a future milestone; this
service can be deployed and inspected independently now.

## Source/API choice

The supported `rnstatus --config <directory> -j` command returns interface status
and genuine counters from an **existing shared instance**. The upstream command
requests `require_shared_instance=True`, rather than silently creating a new
transport instance. No copied/private RNS implementation, network management
commands, identity imports or Python `eval`/shell commands are used.

Versions exposing this JSON form are supported; the service fails closed to an
`available:false` payload when the command/schema is unavailable. **No installed
RNS daemon/version has been integration-tested here.** Test with your installed
`rnstatus --version` and `rnstatus --config ... -j`. Upgrade compatibility is
checked by projection tests, not assumed from the latest upstream branch.

## Setup

Run as an unprivileged account authorized to query the local shared RNS instance.
Use the correct existing config directory; a nonexistent path is rejected.
Never use root just to work around an inaccessible RNS socket. The bridge requires
no write access to the Reticulum configuration/identity files.

```sh
umask 077
python3 -c 'import secrets; print(secrets.token_hex(32))' > /secure/path/bridge.token
python3 bridge/bridge.py \
  --bind 127.0.0.1 --port 8086 \
  --allow 127.0.0.1/32 \
  --token-file /secure/path/bridge.token \
  --rnstatus /absolute/path/to/rnstatus \
  --rns-config /absolute/path/to/existing/.reticulum
```

Token must be an owner-only regular file (0600 or0400), owned by the running user,
32–128 printable non-space ASCII chars. Keep it out of Git, command-line tokens,
logs and screenshots. Rotate the file and restart the service to revoke clients.
Paths above are deliberate examples, not hardcoded credentials.

For direct LAN access, binding a non-loopback address **requires TLS**:

```sh
python3 bridge/bridge.py \
  --bind 192.168.1.10 --port 8086 --allow 192.168.1.42/32 \
  --cert /secure/path/fullchain.pem --key /secure/path/privkey.pem \
  --token-file /secure/path/bridge.token \
  --rnstatus /absolute/path/to/rnstatus \
  --rns-config /absolute/path/to/existing/.reticulum
```

Use a certificate chain trusted by your client, enforce a firewall allowlist, and
never expose this service publicly. Avoid broad CIDRs; authorize individual
clients where possible. Minimum TLS1.2. Loopback HTTP is intended only for local
testing or an authenticated TLS reverse proxy. The bridge uses the actual socket
peer IP and **does not trust X-Forwarded-For**. If proxying, authorize only the
proxy locally and enforce the real-client allowlist at that proxy.

`examples/sensecap-bridge.service` is a hardening template; edit paths/user to
match your installation. Token rotation needs a restart. A compromised bridge
account could access whatever that account can access: OS permissions and
read-only bind paths matter, not just HTTP authentication.

## REST contract

Only authenticated `GET /v1/status`; every endpoint requires allowed source IP
and `Authorization: Bearer <token>`. No query-string tokens, CORS, write routes,
remote path overrides, RPC proxying or administrative operations. Other paths
return404; write methods return405. Unauthorized clients get401/403. Responses
have `Cache-Control: no-store`. No requests execute a subprocess.

Background collector polls every5 seconds; subprocess timeout4s, output parse cap
256KiB, exposed interfaces cap32, interface names cap96 chars. Fixed absolute
executable + fixed argv; no `shell=True`. HTTP connections/handshakes time out in2s,
request backlog8, requests are sequential to keep resources bounded. Deploy a
rate-limiting reverse proxy if greater DoS resistance/concurrency is needed.

Schema1 exposes:

* `available`: whether a valid shared-instance status was collected, **not a
  systemd service-health claim**. `collected_at_unix` is real collection time.
* `interfaces`: name, reported online status, genuine RX/TX bytes and
  `interface_clients` when supported. These are interface-specific
  clients; shared/local connections are not remote Reticulum/LXMF peers.
* Global RX/TX bytes only if rnstatus provides them; no double-summing interfaces.
* `transport_uptime_seconds`, if available.
* `public_transport_hash`: **null by default**. Optional
  `--expose-public-transport-hash` exposes only the public32-hex hash. No identity
  object, identity file, private key, IFAC secret or full raw output is exported.
* `known_destinations`, `reticulum_peers`, `lxmf_propagation`,
  `lxmf_message_queue`, `last_announce_unix`: **null**. This command does not
  reliably expose those semantics; they must not be invented or equated.

An unavailable collection **replaces** the previous result, preventing a stale
healthy payload from being silently served forever. A future UI should also age
`collected_at_unix` and never show a missing metric as zero.

Rates are deliberately omitted from this initial contract: derive them from
successive byte counters and collection times, detecting resets, rather than
assuming that every rnstatus version's speed fields have identical units.
The status command itself can appear as a local shared-instance client while it
collects. Counts are exposed as reported, not converted into a global peer count.

Tests:

```sh
python3 -m unittest discover -s bridge/tests -v
```
