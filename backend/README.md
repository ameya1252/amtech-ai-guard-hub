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
- `POST /shop`
- `GET /shop/<shop_id>`
- `GET /me/shops`
- `POST /shop/<shop_id>/arm`
- `POST /shop/<shop_id>/disarm`
- `GET /shop/<shop_id>/status`
- `GET /alerts/<shop_id>`

These routes use JWT auth. Set:

```sh
JWT_SECRET=...
```

Auth routes have basic rate limits.

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
