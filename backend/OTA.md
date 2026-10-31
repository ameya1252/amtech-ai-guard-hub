# NETTRA Base signed OTA (protocol/storage schema 1)

OTA is admin-authorized and application-only. It retains 16 MB flash, both 4 MB
slots, existing OTA metadata, security-state NVS and factory identity NVS. It never
changes fuses, bootloader, partition table, contacts, camera settings or alarm rules
through OTA. Hardware Secure Boot and flash encryption are not enabled by this work.

## Signing station and release

The trusted station requires Python, cryptography and `esptool==5.6.0`. The private
RSA-3072 signing key is held outside Git with mode 0600; only its public PEM is
committed here and in backend/ota_trust. Keep an offline/secret-manager recovery
copy before a production rollout. Losing the key prevents signing compatible future
updates; replacing it needs a planned trust migration or USB service. Never put the
private key in the app, portal, backend, firmware bundle or object storage.

Build with ESP-IDF 6.0.2, a unique PROJECT_VER (31 characters maximum), rollback
enabled and signed-image-on-update verification enabled. The SDK's unsigned build
is **not bootable** with this configuration. Sign it before any USB installation:

```sh
python tools/ota/release.py --build /private/build --output /private/release \
  --key /private/signing-key.pem --release-id RELEASE_ID \
  --source-commit FULL_40_CHARACTER_COMMIT --target-serial HUB_SERIAL
```

The build checks forbid hardware security-fuse configuration. `release.json` binds
version, exact bytes, source commit, storage schema and one target serial. Current
images include site configuration, so cross-site/broadcast deployment is forbidden.
A compiled legacy serial must match the target. Keep the output private.

## One-time USB baseline

Back up and checksum the full chip. Confirm the installation is disarmed, siren off
and smoke normal. Install the signed application, rollback-enabled bootloader,
matching table and initial OTA metadata through the trusted USB helper:

```sh
python tools/ota/flash_baseline.py --directory /private/release \
  --backup /private/before.bin --checksum /private/before.sha256 --port USB_PORT
# Inspect the verified dry run, then add --write for the authorized installation.
```

Only 0x0, 0x8000, 0x10000 and 0x810000 are written. Base NVS at 0x9000 and factory
identity NVS at 0x812000 are excluded. OTA_1 is left for the first OTA candidate;
that update retains the running signed OTA_0 as its rollback image. Do not use the
previous factory browser release as proof that the new OTA baseline is installed.
The manufacturing release should be updated after qualification, using signed bytes.

## Publish and authorize

Deploy the additive backend routes on esp32. Existing R2 credentials/bucket are
reused, but images are encrypted with the existing AMTECH_LABEL_ENCRYPTION_KEY
before storage (including when the media bucket has a public URL). Authenticated
downloads decrypt and check stored bytes before serving them; the board additionally
checks SHA256, size, embedded version and native RSA signature. No public image URL
or device credential is forwarded to another host.

```sh
python tools/ota/admin.py --backend https://YOUR_BACKEND upload /private/release
python tools/ota/admin.py --backend https://YOUR_BACKEND schedule HUB_SERIAL RELEASE_ID
```

Management credentials come from a hidden prompt or AMTECH_MANAGEMENT_KEY, never
command-line values. Release IDs and per-hub versions are immutable. The Vercel
portal selects approved releases and tracks deployments. Its OTA proxy separately
requires an admin unlock: configure a random NETTRA_OTA_ADMIN_PASSWORD of at least
32 characters. The browser receives a signed, 30-minute, HttpOnly, SameSite=Strict
cookie limited to OTA routes. Management and signing keys stay server-side/local.
Changing the password or management key invalidates sessions. Other portal workflows
and the customer app are unchanged.

## Device admission, interruption and recovery

The hub must be disarmed, siren off, qualified smoke normal, raw panic/smoke clear,
input task fresh, alert outbox persisted and empty, and command handling idle.
Recent authenticated cloud contact is required. ARM, DISARM, STOP or physical alarm
activity interrupts the update; existing handling still runs. Download/authentication
failures abort the inactive slot without changing the boot selection. Offers expire
after at most ten minutes and are not automatically replayed after a reset.

After native signature verification, the hub sends its staged receipt and requires
a fresh server authorization before reboot. Revocation, cancellation or queued
commands can stop this barrier. Alarm Manager coordinates the final handoff, so an
ARM transition is not processed between the final checks and planned restart. A
physical event arriving during the unavoidable reboot interval is handled after
startup; this is planned disarmed maintenance, not uninterrupted protection.

New OTA images remain unconfirmed until at least 45 seconds of startup and local
input qualification, a fresh input heartbeat and an Alarm Manager probe reply. A
90-second health deadline resets/rolls back unconfirmed candidates. Cloud, camera
and cellular connectivity alone do not decide local firmware health. Real alarms
are not mistaken for firmware failure. Existing GPIO/relay initialization and
persisted ARM/force state remain in force. Shared NVS formats must stay backward
compatible; rollback does not restore an older copy of shared data.

## Qualification before production

Host tests inject redirect, truncation, checksum/signature failures, security activity,
storage faults, busy reboot handoff and health deadlines. Backend tests cover real
RSA verification, admin/device scope, per-hub releases, retry identity, encryption,
cancellation, expiry and confirmation. Portal tests cover signed sessions and access.

Device qualification must separately exercise a successful inactive-slot update,
health-failure rollback, interruption before/after boot selection, network loss,
power loss, invalid signatures, retained settings/security state, and heap/stack
margins under alarm/camera/cellular load. Keep USB recovery available. Software
checks and a successful USB flash alone do not establish production OTA readiness.
