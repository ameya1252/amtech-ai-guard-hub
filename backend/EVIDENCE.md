# ESP32 incident evidence

## ESP32 alarm photos (snapshot protocol 1)

New firmware can request a camera JPEG locally and upload it independently of an
external video recorder. Accepted alarm events enqueue `hub_alarm_photos` rows
when the hub reports `snapshot_protocol_version:1`. This additive table requires
no changes to existing account/site data or recorder leases. Old firmware remains
compatible. Alarm ingestion does not contact object storage or decode images.

`POST /esp32/device/photos/<event_id>/<camera_id>` accepts at most one MiB of JPEG,
using existing `X-AMTECH-DEVICE-SERIAL` / `X-AMTECH-DEVICE-TOKEN` authentication and
an integer Unix `X-NETTRA-CAPTURED-AT` header. The event must already have been
accepted from this hub and still belong to its current paired site. Only an
enqueued source camera is accepted. Capture must be within five seconds before
to thirty seconds after the alarm, and upload within ten minutes. Identical
retries do not replace the photo. The sibling `/failure` endpoint accepts a
bounded failure reason; absent reports become unavailable after ten minutes.

JPEG decode has a twelve-megapixel limit. Server processing strips metadata,
limits photos to 1920x1080 and thumbnails to 320x180, and avoids inflating an
already compressed image by retaining sanitized JPEG data where appropriate.
Objects use `evidence/v1/<serial>/snapshots/`, the same private R2 credential and
30-day lifecycle policy. R2 credentials are never sent to the hub. Owner-only
evidence/thumbnail/list APIs merge photos and recorder footage by camera, so
Nettra 1.5.0 displays photos without a new app binary. Photo-only success is
reported `ready`; it does not claim that a pre-alarm video exists.

`POST /esp32/device/photo-test` is a device-authenticated, disarmed-only USB
commissioning helper. It creates a silent `test` alert and photo row, never
commands, SMS, calls or alarm pushes. The hub engineering console exposes PHOTO1
and PHOTO2. New tests are limited to five per hub per hour; retries are idempotent.

The device's bounded PSRAM upload spool does not survive reboot. Capture/network
outages can still make footage unavailable; durable offline evidence requires
camera/NVR or gateway storage. Snapshot support does not implement RTSP recording
or recorded-clip retrieval. Validate those separately before production video.

## External incident video recorder

This feature does not record video on the ESP32. An explicitly enrolled external
recorder buffers enabled cameras on the site LAN. Accepted ESP32 intrusion,
shutter, panic, smoke and camera-interference alerts enqueue per-camera jobs in
the same transaction as alert history. Push delivery never waits for media.
Intrusion/advisory jobs target their source camera; other incidents target the
enrolled, enabled cameras. Duplicate device events do not create duplicate jobs.

Additive tables: evidence_recorders and alert_evidence. Startup create_all creates
them; no existing table, alarm policy or RV1106 upload route is replaced.

## Storage

Backend-only variables:
- NETTRA_MEDIA_BUCKET: a dedicated PRIVATE R2 bucket; no public domain/r2.dev.
- NETTRA_MEDIA_ACCESS_KEY_ID / NETTRA_MEDIA_SECRET_ACCESS_KEY: object read/write
  credential scoped to that bucket.
- R2_ACCOUNT_ID: existing Cloudflare account.

Configure a 30-day expiry lifecycle on evidence/v1/. Keep legacy R2 variables
unchanged. API reads also refuse expired evidence after 30 days from the event;
R2 deletion follows object age and its lifecycle processing schedule. Alert
metadata remains after media expiration. Interrupted/unreferenced uploads also
expire under the same prefix.

The recorder receives object-specific signed PUTs, never storage credentials.
PUTs bind MD5, content type and metadata. Completion checks size, content type,
checksum metadata and single-part ETag. Reads require site ownership and issue
signed GETs valid for at most five minutes and never beyond the evidence expiry.
No public media URL is stored for this feature. List responses contain summaries;
thumbnail redirects and detail responses are authenticated, private/no-store.

## Recorder enrollment and API

GET /shop/<shop>/evidence-recorder returns enrollment/health without secrets.
Owner-authenticated PUT with {enabled:true,camera_ids:[1]} enrolls and returns a
new dedicated recorder token once. Every PUT rotates it. A false enabled value
disables access and pending jobs. Camera 2 may be included in enrollment, but is
only recorded/enqueued while enabled on the hub. Changing the hub's paired site
invalidates the recorder's access.

Recorder requests use X-NETTRA-RECORDER-SERIAL / X-NETTRA-RECORDER-TOKEN.
POST /evidence/recorder/poll heartbeats, returns a leased job, and supplies Camera
2 credentials only for the configuration actually applied by the hub. Active
leases are re-delivered after a lost poll reply. Expired leases get new immutable
upload paths. Prepare/complete/fail are POSTs under
/evidence/recorder/jobs/<id>/<action>, requiring the current lease_id. Completed
jobs are idempotent. Failed transfers can retry from local spool, up to six leases
and 24 hours; missing footage is reported honestly, never reconstructed.

Owner read: GET /alerts/<shop>/<alert>/evidence. A thumbnail is available through
GET /alerts/<shop>/<alert>/thumbnail only after successful attachment. Existing
/alerts/<shop> and media_url remain backwards compatible.

## Validation

Run the explicit backend test modules, including test_evidence. Do not blanket
import every test_*.py: legacy standalone scripts set process-global fixtures.
Qualify real camera capture and app playback before enabling for a customer.
The temporary laptop recorder is not permanent installation hardware.
