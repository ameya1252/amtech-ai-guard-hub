import os
from datetime import datetime, timezone
from uuid import uuid4

import requests


BASE_URL = os.getenv("API_BASE_URL", "http://127.0.0.1:8000").rstrip("/")


def print_response(label, response):
    print(f"{label}: HTTP {response.status_code}: {response.text}")


def create_test_shop():
    suffix = uuid4().hex[:12]
    email = f"alert-test-{suffix}@example.com"
    password = "correct-horse-battery-staple"
    device_serial = f"ALERT-TEST-{suffix}"

    signup = requests.post(
        f"{BASE_URL}/auth/signup",
        json={
            "email": email,
            "password": password,
            "phone_number": "+15555550123",
        },
        timeout=5,
    )
    print_response("signup", signup)
    signup.raise_for_status()

    headers = {"Authorization": f"Bearer {signup.json()['token']}"}
    shop = requests.post(
        f"{BASE_URL}/shop",
        headers=headers,
        json={
            "shop_name": "Alert Test Shop",
            "owner_name": "Alert Test Owner",
            "address": "123 Alert Test Road",
            "device_serial": device_serial,
        },
        timeout=5,
    )
    print_response("create shop", shop)
    shop.raise_for_status()
    return shop.json()["shop_id"], headers


def send_alert(shop_id, event_type):
    payload = {
        "shop_id": shop_id,
        "event_type": event_type,
        "timestamp": datetime.now(timezone.utc).isoformat(),
    }
    response = requests.post(f"{BASE_URL}/alert", json=payload, timeout=5)
    print(f"POST {payload}")
    print(f"HTTP {response.status_code}: {response.text}")
    response.raise_for_status()


def main():
    shop_id, headers = create_test_shop()

    send_alert(shop_id, "test")
    send_alert(shop_id, "intrusion")
    send_alert(shop_id, "shutter")
    send_alert(shop_id, "shutter-1")
    send_alert(shop_id, "shutter-2")
    send_alert(shop_id, "panic")
    send_alert(shop_id, "smoke")

    alert_history_url = f"{BASE_URL}/alerts/{shop_id}"
    response = requests.get(alert_history_url, headers=headers, timeout=5)
    print(f"GET {alert_history_url}")
    print(f"HTTP {response.status_code}: {response.text}")
    response.raise_for_status()


if __name__ == "__main__":
    main()
