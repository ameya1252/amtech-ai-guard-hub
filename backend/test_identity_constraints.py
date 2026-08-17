import os
import tempfile
from uuid import uuid4


db_file = tempfile.NamedTemporaryFile(prefix="amtech_identity_constraints_", suffix=".sqlite", delete=False)
db_file.close()

os.environ["DATABASE_URL"] = f"sqlite:///{db_file.name}"
os.environ["JWT_SECRET"] = "test-secret"
os.environ.setdefault("SIMULATE_SMS", "1")

from app import app  # noqa: E402
from database import Camera, Device, SessionLocal  # noqa: E402
from sqlalchemy.exc import IntegrityError  # noqa: E402


def expect(condition, message):
    print(f"{message}: {'PASS' if condition else 'FAIL'}")
    if not condition:
        raise AssertionError(message)


def create_user_and_shop(client, suffix):
    signup = client.post(
        "/auth/signup",
        json={
            "email": f"identity-{suffix}@example.com",
            "password": "correct-horse-battery-staple",
            "phone_number": "+918550991121",
        },
    )
    print(f"signup {suffix}: HTTP {signup.status_code}: {signup.get_data(as_text=True)}")
    expect(signup.status_code == 201, f"signup {suffix} succeeds")
    headers = {"Authorization": f"Bearer {signup.get_json()['token']}"}

    shop = client.post(
        "/shop",
        headers=headers,
        json={
            "shop_name": f"Identity Shop {suffix}",
            "owner_name": "Owner",
            "address": "Test address",
            "device_serial": f"AMT-IDENTITY-{suffix}",
        },
    )
    print(f"create shop {suffix}: HTTP {shop.status_code}: {shop.get_data(as_text=True)}")
    expect(shop.status_code == 201, f"shop {suffix} creation succeeds")
    return headers, shop.get_json()["shop_id"]


def create_additional_shop(client, headers, suffix):
    shop = client.post(
        "/shop",
        headers=headers,
        json={
            "shop_name": f"Identity Shop {suffix}",
            "owner_name": "Owner",
            "address": "Test address",
            "device_serial": f"AMT-IDENTITY-{suffix}",
        },
    )
    print(f"create additional shop {suffix}: HTTP {shop.status_code}: {shop.get_data(as_text=True)}")
    expect(shop.status_code == 201, f"additional shop {suffix} creation succeeds")
    return shop.get_json()["shop_id"]


def duplicate_device_for_shop_is_rejected(shop_id):
    db = SessionLocal()
    try:
        db.add(Device(
            id=str(uuid4()),
            shop_id=shop_id,
            device_serial=f"AMT-DUP-{uuid4().hex[:8]}",
            status="offline",
        ))
        try:
            db.commit()
        except IntegrityError:
            db.rollback()
            print("duplicate device for same shop rejected by DB: PASS")
            return
        raise AssertionError("duplicate device for same shop was not rejected")
    finally:
        db.close()


def duplicate_camera_serial_is_rejected(client, headers, first_shop_id, second_shop_id):
    first = client.post(
        f"/shop/{first_shop_id}/camera",
        headers=headers,
        json={"camera_serial": "CAM-0001", "slot_number": 1},
    )
    print(f"register first camera: HTTP {first.status_code}: {first.get_data(as_text=True)}")
    expect(first.status_code == 201, "first camera registration succeeds")

    duplicate_api = client.post(
        f"/shop/{second_shop_id}/camera",
        headers=headers,
        json={"camera_serial": "CAM-0001", "slot_number": 1},
    )
    print(f"register duplicate camera via API: HTTP {duplicate_api.status_code}: {duplicate_api.get_data(as_text=True)}")
    expect(duplicate_api.status_code == 409, "duplicate camera serial is rejected by API")

    db = SessionLocal()
    try:
        db.add(Camera(
            id=str(uuid4()),
            shop_id=second_shop_id,
            camera_serial="CAM-0001",
            slot_number=1,
            enabled=True,
        ))
        try:
            db.commit()
        except IntegrityError:
            db.rollback()
            print("duplicate camera serial for another shop rejected by DB: PASS")
            return
        raise AssertionError("duplicate camera serial was not rejected")
    finally:
        db.close()


def main():
    client = app.test_client()
    headers, first_shop_id = create_user_and_shop(client, "one")
    second_shop_id = create_additional_shop(client, headers, "two")

    duplicate_device_for_shop_is_rejected(first_shop_id)
    duplicate_camera_serial_is_rejected(client, headers, first_shop_id, second_shop_id)

    print("Identity ownership constraint tests completed: PASS")


if __name__ == "__main__":
    main()
