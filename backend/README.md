# AMTECH WhatsApp Alert Backend

This backend keeps the permanent Meta WhatsApp Cloud API credentials off the physical hub device.

The hub sends alerts to this backend:

```http
POST /alert
Content-Type: application/json

{
  "shop_id": "amtech-demo-shop",
  "event_type": "intrusion",
  "timestamp": "2026-07-18T12:00:00Z"
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
