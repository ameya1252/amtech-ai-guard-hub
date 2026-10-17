# ESP32 app integration

This is additive to the existing RV1106 backend. Reuse the service's DATABASE_URL, JWT_SECRET, DEVICE_CONFIG_SYNC_TOKEN and R2 environment settings. Startup creates the additive ESP32 tables; existing RV1106 alarm/command contracts remain unchanged.

## Provisioning

On a trusted machine with the production DATABASE_URL in its environment:

```sh
python provision_esp32.py ESP32-AMEYA-0001 --output /private/path/hub.json
```

The target must not exist, and its parent must exist with private permissions. The CLI creates a mode-0600 JSON containing device_serial, device_token, pairing_code and backend_url, and stores only credential hashes in PostgreSQL. It refuses existing serials. Keep the token in the ESP32's ignored `main/app_credentials.h`; distribute only the pairing code/serial to the owner. Do not put infrastructure secrets on the hub or in the app.

The owner calls POST /esp32/pair with their normal Bearer JWT, serial, pairing_code, shop_name, owner_name and address. Repeating a pairing request by the same owner is idempotent; another owner cannot claim it. Legacy /shop serial-only registration cannot claim provisioned hubs.

## Device contract

POST /esp32/device/sync authenticates using X-AMTECH-DEVICE-SERIAL and X-AMTECH-DEVICE-TOKEN. Its JSON includes:

- state: arm_state (disarmed/arming/armed), force_armed, siren_active, siren_owner, shutter_1, shutter_2, smoke, camera_online, camera_protection_active, optional arm_block_reason.
- Optional ack: id, applied/rejected status, reason and arm_state at command completion. This snapshot may differ from current state after a later SMS/local change.
- Optional events (maximum 32): stable string id, event_type (intrusion/shutter-1/shutter-2/panic/smoke), optional ISO timestamp with timezone.

Response: paired, command (id/name/valid_for_ms), ack_received, accepted_event_ids. Retries keep event IDs stable and cannot replay completed commands. Firmware persists receipts before submitting commands and rejects unfinished receipts after a reboot. Alert receipts are deduplicated per device. The bounded firmware outbox may drop additional events after extended disconnection; local alarm/SMS handling continues.

Owner controls POST /shop/:id/arm, /disarm, /arm-force and /stop queue requests. An online heartbeat is required (45-second threshold). Commands expire after 120 seconds; firmware confirmation times out after 90 seconds. ARM/Force Arm can be superseded by DISARM/STOP. All alarm decisions remain in existing ESP32 alarm-manager APIs. STOP is distinct from DISARM, but follows core policy: intrusion STOP disarms; smoke/panic STOP retains the arm state. GET /shop/:id/status returns reported state, capabilities, pending_command and last_command; desired state never masquerades as hardware confirmation.

Legacy unauthenticated /alert and device-config/camera-registration routes do not accept ESP32 operations. ESP32 alerts use device-authenticated sync. Existing push sender and owner push-token storage are reused, without applying RV1106-specific auto-disarm messaging to ESP32 smoke/panic events.

## Alarm push retries and phone logout

Sync saves new alert history, its device event receipt and one `alarm_push_deliveries` job per registered owner phone in the same transaction. The event acknowledgement means the backend retained the event; it does not claim phone delivery. The background worker sends independently of heartbeats, so acknowledged events can still retry after a process restart or hub disconnection. Sync does not wait for Expo.

Jobs retry transient HTTP/network failures, rejected Expo tickets and malformed responses with exponential backoff starting at 30 seconds, capped at 15 minutes, up to eight attempts within one hour of ingestion. Each recipient has its own state, so an accepted phone is not resent when another fails. Nonempty successful Expo tickets mark acceptance, not confirmed phone display. `DeviceNotRegistered` removes the stale token; oversized messages fail without repeated sends. Simulation is recorded separately. Terminal failures remain visible in the database/logs, and alert history is retained.

The worker starts with the existing non-preloaded Gunicorn process, polls every 15 seconds, wakes on sync and uses atomic 90-second leases to recover abandoned jobs. Set `ALARM_PUSH_WORKER_DISABLED=1` for isolated tests/maintenance. Delivery is at least once: if Expo accepts a push but its reply is lost, retry can duplicate that notification. Camera advisories retain their separate conservative cooldown/retry behavior.

Authenticated `DELETE /me/push-token` with `{"expo_push_token":"ExpoPushToken[...]"}` idempotently removes only that owner's specified phone token and cancels its queued alarm pushes. Another account cannot delete the registration. Dispatch rechecks token ownership, so queued messages do not follow an account switch. A request already in flight or accepted by Expo cannot be recalled. Deploy this endpoint before distributing the updated mobile app, whose logout awaits successful unregistration before clearing its login session.

## Deployment and checks

Deploy repository root to the existing Railway service, retaining its `backend` root directory. Select the `esp32` branch for GitHub autodeploys. /health includes esp32_api_version=1. All schema additions are created at startup via existing init_db.

Run `python test_esp32.py`, `python test_pending_command.py`, `python test_identity_constraints.py`, and `python test_forgot_password.py` from backend using test environment fixtures. These do not require real hub activity. AMTECH_APP_ORIGINS configures allowed web origins; the default includes local Expo/web preview ports. Native apps do not use browser CORS.

The firmware adapter uses the already-configured Wi-Fi transport. It does not add cellular HTTP/data behavior. A phone/board test and Expo/FCM notification credentials are needed to qualify the complete installation.

The single-process Gunicorn server uses four request threads. This allows app status/shop/alert polling and device heartbeats to overlap without queuing every request behind a synchronous worker. Database sessions remain per request, and hub/command writes remain serialized by the existing row locks. Keep one process for the current in-memory rate limiter and database keepalive thread. Live browser testing exposed eight-second client timeouts with the previous single synchronous worker.


## Camera 1 interference advisory

`POST /esp32/device/camera-advisory` uses the existing device serial/token headers and accepts `{"event_id":"STABLE_EPISODE_ID","camera_id":1}`. It is independent of sync alarm events and never queues a command, SMS or call. Only Camera 1 is enabled in this phase. The server retains a `camera-interference-1` history event and a durable record in the new `esp32_camera_advisory_receipts` table (created by the existing `init_db()`/`create_all` path). Existing tables/credentials are unchanged.

Responses contain `ok`, matching `event_id`, `status` and `cooldown_ms`. `accepted` requires at least one real Expo ticket with status `ok` and a nonempty ticket ID; simulation and missing push tokens never qualify. The 30-minute per-camera cooldown starts on service acceptance. A push ticket does not establish phone delivery. `retry` is bounded to three attempts with 30-second spacing and a 90-second episode lifetime; use the same ID. `failed` is terminal for that episode. `suppressed` episodes never become delayed notifications. `uncertain` means a possible submission cannot safely be repeated, so a conservative suppression window applies.

A durable `in_flight` reservation precedes each external send. Concurrent requests and process/network failure after submission cannot cause a blind resend. Only explicit provider rejection is retried. Advisory push uses channel `camera-advisories` and identifies Camera 1; normal intrusion/smoke/panic paths retain their existing behavior.

Local checks: run `python -m unittest test_esp32 test_camera_advisory test_alarm_push -v` with the test dependencies. Older test scripts set environment variables at import time; run their standalone entry points in separate processes with `ALARM_PUSH_WORKER_DISABLED=1 DATABASE_KEEPALIVE_DISABLED=1 SIMULATE_PUSH=1 SIMULATE_SMS=1`. Unit tests use isolated SQLite and fake Expo responses. Actual concurrent row locking requires PostgreSQL; no production migration or push was performed by these tests. Native phone push and camera-output isolation remain release checks.
