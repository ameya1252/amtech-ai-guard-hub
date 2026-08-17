# AMTECH Alert History Backend

This backend stores AMTECH Guard Hub alert history for the owner app. Emergency delivery is handled directly by the hub through SIM7672 voice calls and SMS.

The hub sends alerts to this backend:

```http
POST /alert
Content-Type: application/json

{
  "shop_id": "amtech-demo-shop",
  "event_type": "intrusion",
  "timestamp": "2026-07-18T12:00:00Z",
  "media_url": "https://example.com/optional-alert-image.jpg"
}
```

The backend records the alert in the database. It does not send WhatsApp messages.

## Running Locally

```sh
cd backend
PORT=8000 python3 app.py
```

The app binds to `0.0.0.0` and reads the runtime port from `PORT`, which is what Railway provides:

```sh
PORT=8000 python3 app.py
```

Railway can start it with the included `Procfile`:

```text
web: gunicorn app:app --bind 0.0.0.0:$PORT
```

Health check:

```sh
curl http://127.0.0.1:8000/health
```

Live Railway health check:

```sh
curl https://amtech-ai-guard-hub-production.up.railway.app/health
```

Test it from another terminal:

```sh
python3 test_alerts.py
```

Allowed alert event types:

- `intrusion`
- `shutter`
- `shutter-1`
- `shutter-2`
- `panic`
- `smoke`
- `test`

## Database

The backend reads its database connection from `DATABASE_URL`.

Railway should provide this from Neon, for example:

```text
DATABASE_URL=postgresql://...
```

For local testing without Neon, a temporary SQLite database can be used:

```sh
DATABASE_URL=sqlite:////tmp/amtech_alerts.db PORT=8000 python3 app.py
```

On startup, SQLAlchemy creates the initial tables if they do not exist:

- `users`
- `shops`
- `devices`
- `alerts`

Alert history endpoint requires a logged-in user token and shop ownership:

```sh
curl -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8000/alerts/amtech-demo-shop
```

## App/Auth Endpoints

The backend also has the first mobile-app-facing APIs:

- `POST /auth/signup`
- `POST /auth/login`
- `POST /auth/forgot-password`
- `POST /auth/verify-reset-otp`
- `POST /shop`
- `GET /shop/<shop_id>`
- `GET /me/shops`
- `POST /shop/<shop_id>/camera`
- `GET /shop/<shop_id>/cameras`
- `POST /shop/<shop_id>/arm`
- `POST /shop/<shop_id>/disarm`
- `GET /shop/<shop_id>/status`
- `GET /alerts/<shop_id>`

These routes use JWT auth. Set:

```sh
JWT_SECRET=...
```

Auth routes have basic rate limits.

Password reset uses backend-sent SMS OTP, not the physical hub modem. Local and safe deployments default to simulated SMS:

```sh
SIMULATE_SMS=1
```

In simulation, the backend logs the OTP that would be sent. For production SMS delivery, set:

```sh
SIMULATE_SMS=0
SMS_PROVIDER=msg91
MSG91_AUTH_KEY=...
MSG91_SENDER_ID=...
PASSWORD_RESET_OTP_SECRET=...
PASSWORD_RESET_OTP_EXPIRY_MINUTES=10
```

The MSG91 sender/template must be approved for India DLT before disabling simulation.

`POST /shop` registers a shop for the authenticated user. The owner phone and email are copied from the logged-in user account, so the app should not re-submit them during onboarding:

```http
POST /shop
Authorization: Bearer {token}
Content-Type: application/json

{
  "shop_name": "AMTECH Test Shop",
  "owner_name": "Shop Owner Name",
  "address": "Shop address",
  "device_serial": "AMT-0000"
}
```

The response includes `owner_name`, `address`, `owner_phone`, and `owner_email` along with the shop and device identifiers.

Camera setup uses AMTECH inventory serials. Owners enter only the sticker serial, such as `CAM-0001`; the backend resolves IP and credentials from `camera_inventory` and never returns those credentials to the app.

At startup, the backend seeds `camera_inventory` from `AMTECH_CAMERA_INVENTORY` if present. Format:

```text
AMTECH_CAMERA_INVENTORY=CAM-0001,192.168.0.4,Amtech,Amtech123;CAM-0002,192.168.0.7,Amtech1,Amtech1234
```

For the current pilot defaults, `CAM-0001` and `CAM-0002` are seeded from `AMTECH_CAMERA_1_*` and `AMTECH_CAMERA_2_*` environment variables, with the tested local camera values as fallbacks. Move these values fully into environment variables before broader deployment.

```http
POST /shop/{shop_id}/camera
Authorization: Bearer {token}
Content-Type: application/json

{
  "camera_serial": "CAM-0001",
  "slot_number": 1
}
```

For current test-scale/admin seeding, the same endpoint can create a missing inventory record if `camera_ip`, `camera_username`, and `camera_password` are also supplied. The normal owner app should not send those fields.

Camera assignment is currently unique per shop slot, not globally unique per serial, so repeated pilot/test onboarding can reuse the seeded `CAM-0001` and `CAM-0002` records without exposing camera credentials. Before production, add a stricter ownership/claiming flow if one physical camera must be locked to one shop.

```http
GET /shop/{shop_id}/cameras
Authorization: Bearer {token}
```

The response includes `camera_serial`, `slot_number`, and `enabled`, but not camera IP, username, or password.

## Media Upload URLs

The backend can generate Cloudflare R2 presigned upload URLs for future alert images/videos:

```http
POST /shop/<shop_id>/media/upload-url
Authorization: Bearer {token}
Content-Type: application/json

{
  "content_type": "image/jpeg"
}
```

Required environment variables:

```sh
R2_ACCOUNT_ID=...
R2_ACCESS_KEY_ID=...
R2_SECRET_ACCESS_KEY=...
R2_BUCKET_NAME=...
R2_PUBLIC_BASE_URL=...
```
