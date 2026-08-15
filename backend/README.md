# AMTECH WhatsApp Alert Backend

This backend keeps the permanent Meta WhatsApp Cloud API credentials off the physical hub device.

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

The backend then sends the WhatsApp message.

## Simulated Mode

Simulated mode is enabled by default:

```sh
cd backend
PORT=8000 SIMULATE_WHATSAPP=1 python3 app.py
```

The app binds to `0.0.0.0` and reads the runtime port from `PORT`, which is what Railway provides:

```sh
PORT=8000 SIMULATE_WHATSAPP=1 python3 app.py
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

In this mode it prints:

```text
Would send WhatsApp alert: {shop_id} {event_type} {timestamp}
```

The current Railway deployment is still running in simulated WhatsApp mode. It receives real HTTPS alert requests and returns `simulated:true`, but it does not send real Meta WhatsApp messages yet.

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
DATABASE_URL=sqlite:////tmp/amtech_alerts.db PORT=8000 SIMULATE_WHATSAPP=1 python3 app.py
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

## Real Meta WhatsApp Cloud API Mode

Set `SIMULATE_WHATSAPP=0` and provide:

```sh
export META_ACCESS_TOKEN="..."
export META_PHONE_NUMBER_ID="..."
export WHATSAPP_TO="..."
export WHATSAPP_TEMPLATE_NAME="..."
export WHATSAPP_TEMPLATE_LANG="en_US"
export META_GRAPH_API_VERSION="v20.0"
```

The real call goes to:

```text
https://graph.facebook.com/{META_GRAPH_API_VERSION}/{META_PHONE_NUMBER_ID}/messages
```

Headers:

```text
Authorization: Bearer {META_ACCESS_TOKEN}
Content-Type: application/json
```

The body uses a WhatsApp `template` message. The template must be created and approved in Meta Business Manager before real alerts can be sent.
