import os
from datetime import datetime, timezone
from uuid import uuid4

import requests
from flask import Flask, jsonify, request

from database import Alert, Shop, SessionLocal, init_db


app = Flask(__name__)
init_db()


def env_bool(name, default=False):
    value = os.getenv(name)
    if value is None:
        return default
    return value.strip().lower() in ("1", "true", "yes", "on")


def build_template_payload(alert):
    return {
        "messaging_product": "whatsapp",
        "recipient_type": "individual",
        "to": os.environ["WHATSAPP_TO"],
        "type": "template",
        "template": {
            "name": os.environ["WHATSAPP_TEMPLATE_NAME"],
            "language": {
                "code": os.getenv("WHATSAPP_TEMPLATE_LANG", "en_US")
            },
            "components": [
                {
                    "type": "body",
                    "parameters": [
                        {"type": "text", "text": alert["shop_id"]},
                        {"type": "text", "text": alert["event_type"]},
                        {"type": "text", "text": alert["timestamp"]},
                    ],
                }
            ],
        },
    }


def send_whatsapp_alert(alert):
    if env_bool("SIMULATE_WHATSAPP", default=True):
        print(
            "Would send WhatsApp alert: "
            f"{alert['shop_id']} {alert['event_type']} {alert['timestamp']}",
            flush=True,
        )
        return {"simulated": True}

    graph_version = os.getenv("META_GRAPH_API_VERSION", "v20.0")
    phone_number_id = os.environ["META_PHONE_NUMBER_ID"]
    access_token = os.environ["META_ACCESS_TOKEN"]
    url = f"https://graph.facebook.com/{graph_version}/{phone_number_id}/messages"

    headers = {
        "Authorization": f"Bearer {access_token}",
        "Content-Type": "application/json",
    }
    payload = build_template_payload(alert)

    # Real Meta WhatsApp Cloud API call. Requires:
    # - META_ACCESS_TOKEN with whatsapp_business_messaging permission
    # - META_PHONE_NUMBER_ID
    # - WHATSAPP_TO recipient phone number / WhatsApp ID
    # - WHATSAPP_TEMPLATE_NAME for a pre-approved utility template
    response = requests.post(url, headers=headers, json=payload, timeout=10)
    response.raise_for_status()
    return response.json()


def parse_alert(payload):
    if not isinstance(payload, dict):
        raise ValueError("request body must be a JSON object")

    shop_id = payload.get("shop_id")
    event_type = payload.get("event_type")
    timestamp = payload.get("timestamp")

    if not shop_id:
        raise ValueError("shop_id is required")

    if event_type not in ("intrusion", "shutter", "test"):
        raise ValueError("event_type must be one of intrusion, shutter, test")

    if not timestamp:
        timestamp = datetime.now(timezone.utc).isoformat()

    return {
        "shop_id": str(shop_id),
        "event_type": event_type,
        "timestamp": str(timestamp),
    }


def parse_timestamp(value):
    normalized = value
    if normalized.endswith("Z"):
        normalized = f"{normalized[:-1]}+00:00"

    parsed = datetime.fromisoformat(normalized)
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed


def record_alert(alert_payload):
    db = SessionLocal()
    try:
        shop = db.get(Shop, alert_payload["shop_id"])
        if shop is None:
            print(
                f"Warning: shop_id {alert_payload['shop_id']} not found; "
                "creating placeholder shop record",
                flush=True,
            )
            shop = Shop(
                id=alert_payload["shop_id"],
                shop_name=f"Unregistered shop {alert_payload['shop_id']}",
            )
            db.add(shop)

        alert_row = Alert(
            id=str(uuid4()),
            shop_id=alert_payload["shop_id"],
            event_type=alert_payload["event_type"],
            timestamp=parse_timestamp(alert_payload["timestamp"]),
            whatsapp_sent=False,
        )
        db.add(alert_row)
        db.commit()
        db.refresh(alert_row)
        return alert_row
    except Exception:
        db.rollback()
        raise
    finally:
        db.close()


def alert_to_dict(alert_row):
    timestamp = alert_row.timestamp
    if timestamp.tzinfo is None:
        timestamp = timestamp.replace(tzinfo=timezone.utc)

    return {
        "id": alert_row.id,
        "shop_id": alert_row.shop_id,
        "event_type": alert_row.event_type,
        "timestamp": timestamp.isoformat(),
        "whatsapp_sent": alert_row.whatsapp_sent,
    }


@app.post("/alert")
def alert():
    try:
        alert_payload = parse_alert(request.get_json(silent=True))
        alert_row = record_alert(alert_payload)
        provider_response = send_whatsapp_alert(alert_payload)
        whatsapp_sent = not bool(provider_response.get("simulated"))
        if alert_row.whatsapp_sent != whatsapp_sent:
            db = SessionLocal()
            try:
                stored_alert = db.get(Alert, alert_row.id)
                stored_alert.whatsapp_sent = whatsapp_sent
                db.commit()
                db.refresh(stored_alert)
                alert_row = stored_alert
            finally:
                db.close()
    except ValueError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400
    except KeyError as exc:
        return jsonify({"ok": False, "error": f"missing environment variable: {exc.args[0]}"}), 500
    except requests.RequestException as exc:
        return jsonify({"ok": False, "error": f"whatsapp api call failed: {exc}"}), 502

    return jsonify({"ok": True, "alert": alert_to_dict(alert_row), "provider_response": provider_response})


@app.get("/alerts/<shop_id>")
def alerts(shop_id):
    db = SessionLocal()
    try:
        alert_rows = (
            db.query(Alert)
            .filter(Alert.shop_id == shop_id)
            .order_by(Alert.timestamp.desc())
            .all()
        )
        return jsonify({"ok": True, "shop_id": shop_id, "alerts": [alert_to_dict(row) for row in alert_rows]})
    finally:
        db.close()


@app.get("/health")
def health():
    return jsonify({"status": "ok"})


if __name__ == "__main__":
    port = int(os.environ.get("PORT", "8000"))
    app.run(host="0.0.0.0", port=port)
