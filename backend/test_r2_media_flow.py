import base64
import os
import time
from uuid import uuid4

import requests


BASE_URL = os.getenv("API_BASE_URL", "http://127.0.0.1:8000").rstrip("/")


TEST_JPEG = base64.b64decode(
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAP//////////////////////////////////////////////////////////////////////////////////////"
    "2wBDAf//////////////////////////////////////////////////////////////////////////////////////wAARCAABAAEDASIAAhEBAxEB/8QAFQABAQAAAAAAAA"
    "AAAAAAAAAAAAX/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oADAMBAAIQAxAAAAH/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oACAEBAAEFAqf/xAAUEQEAAAAAAAAAAAAAAAAAAAAA/"
    "9oACAEDAQE/ASP/xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oACAECAQE/ASP/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oACAEBAAY/Al//xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9o"
    "ACAEBAAE/IV//2gAMAwEAAgADAAAAEP/EABQRAQAAAAAAAAAAAAAAAAAAABD/2gAIAQMBAT8QH//EABQRAQAAAAAAAAAAAAAAAAAAABD/2gAIAQIBAT8QH//EABQQAQAAAAAA"
    "AAAAAAAAAAAAABD/2gAIAQEAAT8QH//Z"
)


def print_result(label, response):
    try:
        body = response.json()
    except ValueError:
        body = response.text[:200]
    print(f"{label}: HTTP {response.status_code} {body}")


def require_ok(label, response, expected_status=200):
    print_result(label, response)
    if response.status_code != expected_status:
        raise RuntimeError(f"{label} returned HTTP {response.status_code}, expected {expected_status}")
    return response.json()


def fetch_public_url(public_url, expected_body, attempts=6):
    last_response = None
    for attempt in range(1, attempts + 1):
        response = requests.get(public_url, timeout=20)
        last_response = response
        if response.status_code == 200 and response.content == expected_body:
            print(f"public media fetch attempt {attempt}: HTTP 200, {len(response.content)} bytes, body matches upload")
            return
        print(f"public media fetch attempt {attempt}: HTTP {response.status_code}, {len(response.content)} bytes")
        time.sleep(2)

    raise RuntimeError(f"public media was not retrievable or did not match upload: HTTP {last_response.status_code}")


def main():
    suffix = uuid4().hex[:12]
    email = f"r2-media-{suffix}@example.com"
    password = "correct-horse-battery-staple"
    device_serial = f"R2-MEDIA-{suffix}"

    signup = require_ok(
        "signup",
        requests.post(
            f"{BASE_URL}/auth/signup",
            json={"email": email, "password": password, "phone_number": "+15555550123"},
            timeout=20,
        ),
        expected_status=201,
    )
    token = signup["token"]
    headers = {"Authorization": f"Bearer {token}"}

    shop = require_ok(
        "create shop",
        requests.post(
            f"{BASE_URL}/shop",
            headers=headers,
            json={
                "shop_name": "R2 Media Test Shop",
                "owner_phone": "+15555550123",
                "owner_email": email,
                "device_serial": device_serial,
            },
            timeout=20,
        ),
        expected_status=201,
    )
    shop_id = shop["shop_id"]

    upload = require_ok(
        "request upload URL",
        requests.post(
            f"{BASE_URL}/shop/{shop_id}/media/upload-url",
            headers=headers,
            json={"content_type": "image/jpeg"},
            timeout=20,
        ),
    )

    put_response = requests.put(
        upload["upload_url"],
        data=TEST_JPEG,
        headers={"Content-Type": "image/jpeg"},
        timeout=30,
    )
    print(f"R2 PUT upload: HTTP {put_response.status_code}, uploaded {len(TEST_JPEG)} bytes")
    if put_response.status_code not in (200, 201):
        raise RuntimeError(f"R2 PUT failed: HTTP {put_response.status_code} {put_response.text[:200]}")

    timestamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    alert = require_ok(
        "post alert with media_url",
        requests.post(
            f"{BASE_URL}/alert",
            json={
                "shop_id": shop_id,
                "event_type": "test",
                "timestamp": timestamp,
                "media_url": upload["public_url"],
            },
            timeout=20,
        ),
    )
    returned_media_url = alert["alert"]["media_url"]
    print(f"posted alert media_url matches: {returned_media_url == upload['public_url']}")
    if returned_media_url != upload["public_url"]:
        raise RuntimeError("POST /alert response did not include the expected media_url")

    alerts = require_ok(
        "fetch alerts",
        requests.get(f"{BASE_URL}/alerts/{shop_id}", headers=headers, timeout=20),
    )
    fetched_media_url = alerts["alerts"][0]["media_url"]
    print(f"fetched alert media_url matches: {fetched_media_url == upload['public_url']}")
    if fetched_media_url != upload["public_url"]:
        raise RuntimeError("GET /alerts response did not include the expected media_url")

    fetch_public_url(upload["public_url"], TEST_JPEG)
    print(f"public_url: {upload['public_url']}")


if __name__ == "__main__":
    main()
