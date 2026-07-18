from datetime import datetime, timezone

import requests


BACKEND_URL = "http://127.0.0.1:8000/alert"
ALERT_HISTORY_URL = "http://127.0.0.1:8000/alerts/amtech-demo-shop"


def send_alert(shop_id, event_type):
    payload = {
        "shop_id": shop_id,
        "event_type": event_type,
        "timestamp": datetime.now(timezone.utc).isoformat(),
    }
    response = requests.post(BACKEND_URL, json=payload, timeout=5)
    print(f"POST {payload}")
    print(f"HTTP {response.status_code}: {response.text}")
    response.raise_for_status()


def main():
    send_alert("amtech-demo-shop", "test")
    send_alert("amtech-demo-shop", "intrusion")
    send_alert("amtech-demo-shop", "shutter")

    response = requests.get(ALERT_HISTORY_URL, timeout=5)
    print(f"GET {ALERT_HISTORY_URL}")
    print(f"HTTP {response.status_code}: {response.text}")
    response.raise_for_status()


if __name__ == "__main__":
    main()
