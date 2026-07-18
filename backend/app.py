import os
from datetime import datetime, timezone

import requests
from flask import Flask, jsonify, request


app = Flask(__name__)


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


@app.post("/alert")
def alert():
    try:
        alert_payload = parse_alert(request.get_json(silent=True))
        provider_response = send_whatsapp_alert(alert_payload)
    except ValueError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400
    except KeyError as exc:
        return jsonify({"ok": False, "error": f"missing environment variable: {exc.args[0]}"}), 500
    except requests.RequestException as exc:
        return jsonify({"ok": False, "error": f"whatsapp api call failed: {exc}"}), 502

    return jsonify({"ok": True, "alert": alert_payload, "provider_response": provider_response})


if __name__ == "__main__":
    app.run(host=os.getenv("BACKEND_HOST", "127.0.0.1"), port=int(os.getenv("BACKEND_PORT", "8000")))
