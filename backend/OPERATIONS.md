# Nettra operations, packaging and one-scan onboarding

This additive integration serves the AMTECH operations portal and the `esp32`
customer app. Existing RV1106 APIs, legacy ESP32 pairing and alarm policy remain
compatible. All new database tables use the `ops_` prefix and are created through
the existing `init_db()` path. No existing production records are seeded or renamed.

## Private transport, no portal accounts

The web app has no login screen or user-session implementation. Its server proxy
adds `X-AMTECH-OPERATIONS-KEY`; the key must not enter client code. Publish the
portal behind a private hosting/network boundary. Do not make the no-login
management site public. Local development is loopback-only by default.

Set these backend environment values:

- `AMTECH_MANAGEMENT_KEY`: random secret, at least 32 characters.
- `AMTECH_LABEL_ENCRYPTION_KEY`: a Fernet key, backed up separately and retained
  across deployments. This protects unclaimed activation codes and pre-release
  device credentials in the database.
- Optional `PUBLIC_BACKEND_URL`: the production HTTPS device API origin.

The private site server needs `AMTECH_MANAGEMENT_KEY` and `AMTECH_BACKEND_URL`.
The browser never receives either manufacturing API credentials or permanent device
secrets. Missing management/encryption configuration fails closed. Ops responses
are `no-store`; cross-origin writes to the site proxy are rejected; the proxy does
not expose `/provision`, which is reserved for the trusted USB tool.

## Process and endpoints

1. `POST /ops/batches`: kind, count (1–50), YYMM period and stable `request_id`.
   Per-prefix counters and database uniqueness allocate formatted serial numbers.
   Retrying the same request ID does not allocate a second batch.
2. `GET /ops/batches/:id/labels`: QR content for printing. Hub QRs contain the serial
   and random claim code; accessory QRs contain only inventory identity/type.
   Claimed hub activation codes can no longer be exported. Inventory/list/log APIs
   never return plaintext claim codes or device tokens.
3. Trusted `POST /ops/assets/:serial/provision`: verified board MAC and optional PCB
   and firmware revision. Credentials are idempotent for that board until release;
   another board cannot reuse the identity. MAC uniqueness also blocks assigning
   multiple labels to the same board. Already released/claimed units are blocked.
4. `POST /ops/assets/:serial/test`: explicit boolean results for identity, sensors,
   siren, LTE, Ethernet, Wi-Fi, camera and app. The reported board identity and
   heartbeat must match. Unchecked/failed tests prevent packing. UI checkboxes are
   technician attestations, not automatic proof of physical test completion.
5. `POST /ops/orders`, followed by `/ops/orders/:id/scan`: open a draft, scan hub
   first, then selected accessory types. Row locks and unique allocations prevent
   cross-package reuse, duplicate scans and excess quantities. Draft removal is
   supported; confirmed contents are immutable through these endpoints.
6. `POST /ops/orders/:id/pack` requires exact quantities and passed EOL;
   `/dispatch` follows packing. Both actions are idempotent.
7. Customer `POST /esp32/pair` retains its authenticated serial/code contract.
   Managed hubs require passed EOL and a confirmed package. Claiming binds the
   owner once; valid same-owner retries are safe after a lost HTTP response.
   Another account cannot reuse the QR. No preassigned customer is required.
8. Pairing returns the package and accepts optional numeric latitude/longitude.
   Owner-scoped `GET /shop/:id/package` loads it later without accessory rescans.
   Saved phone pins identify the installation location; they are not live GPS.

Base capabilities are one camera, two independent shutter inputs and one smoke
input. High-End numbers can be reserved, but provisioning/packing that profile is
blocked until its hardware and firmware capabilities are qualified.

## Monitoring and logs

`GET /ops/overview` joins fleet/site/location records, shows online status using the
existing 45-second freshness rule, and returns packing summaries in bounded queries.
No remote alarm-control operation is exposed by the leadership portal.
`GET /ops/logs?serial=...` merges bounded manufacturing/audit records, existing alarm
history and command outcomes. New firmware reports version, board ID, boot ID,
uptime, reset reason and heap size. State-change/boot records are captured inside
sync's existing transaction; unchanged heartbeats do not produce history rows.
Existing firmware continues to work without the optional metadata.

This is a structured event timeline, not a lossless serial logger. The existing
bounded firmware outbox and network outage limits still apply. Raw console logs,
long-term retention automation and physical sensor/notification qualification are
separate work. Camera coordinates/data remain private; diagnostic exports exclude
credentials. Production backup/restore policies remain an operational requirement.

## Verification

Run isolated suites from `backend`:

    python -m unittest test_esp32 test_camera_advisory test_alarm_push test_operations

The manufacturing tests exercise label allocation/retry, board binding, EOL gates,
scan ordering, wrong types, duplicates, capacity limits, cross-order isolation,
removal, complete claiming, customer isolation, location validation and state-log
coalescing. They use SQLite fixtures; live Postgres concurrency and fresh-board
writes still require deployment/hardware qualification.

## Factory-event isolation

Managed, unclaimed hubs acknowledge test alarms into the manufacturing timeline,
not customer alerts. Stable factory event IDs are deduplicated and stay excluded
from customer history even if an old acknowledgement is retried after claiming.
Legacy unclaimed hubs retain their prior outbox behavior. EOL release requires
new firmware to report an empty alert outbox, with the hub disarmed, siren off and
smoke input normal. This prevents stale factory smoke/panic tests becoming the
buyer's first notifications. Customer alarm rules are unchanged.
