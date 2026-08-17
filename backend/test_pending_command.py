import os
import tempfile


db_file = tempfile.NamedTemporaryFile(prefix="amtech_pending_command_", suffix=".sqlite", delete=False)
db_file.close()

os.environ["DATABASE_URL"] = f"sqlite:///{db_file.name}"
os.environ["JWT_SECRET"] = "test-secret"
os.environ["DEVICE_CONFIG_SYNC_TOKEN"] = "device-token-test"
os.environ.setdefault("SIMULATE_SMS", "1")

from app import app  # noqa: E402


def expect(condition, message):
    print(f"{message}: {'PASS' if condition else 'FAIL'}")
    if not condition:
        raise AssertionError(message)


def main():
    client = app.test_client()

    signup = client.post(
        "/auth/signup",
        json={
            "email": "pending-command@example.com",
            "password": "correct-horse-battery-staple",
            "phone_number": "+918550991121",
        },
    )
    print(f"signup: HTTP {signup.status_code}: {signup.get_data(as_text=True)}")
    expect(signup.status_code == 201, "signup succeeds")
    token = signup.get_json()["token"]
    auth_headers = {"Authorization": f"Bearer {token}"}

    shop = client.post(
        "/shop",
        headers=auth_headers,
        json={
            "shop_name": "Pending Command Shop",
            "owner_name": "Owner",
            "address": "Test address",
            "device_serial": "PENDING-CMD-0001",
        },
    )
    print(f"create shop: HTTP {shop.status_code}: {shop.get_data(as_text=True)}")
    expect(shop.status_code == 201, "shop creation succeeds")
    shop_id = shop.get_json()["shop_id"]

    unauth = client.get(f"/shop/{shop_id}/pending-command")
    print(f"unauth pending-command: HTTP {unauth.status_code}: {unauth.get_data(as_text=True)}")
    expect(unauth.status_code == 401, "pending command rejects unauthenticated request")

    arm = client.post(f"/shop/{shop_id}/arm", headers=auth_headers)
    print(f"arm: HTTP {arm.status_code}: {arm.get_data(as_text=True)}")
    arm_json = arm.get_json()
    expect(arm.status_code == 200, "arm endpoint succeeds")
    expect(arm_json["armed"] is True, "arm endpoint updates intended armed state")
    expect(arm_json["pending_command"] == "arm", "arm endpoint sets pending command")
    expect(bool(arm_json["pending_command_id"]), "arm endpoint sets pending command id")

    device_headers = {"X-AMTECH-DEVICE-CONFIG-TOKEN": "device-token-test"}
    pending = client.get(f"/shop/{shop_id}/pending-command", headers=device_headers)
    print(f"device pending-command: HTTP {pending.status_code}: {pending.get_data(as_text=True)}")
    pending_json = pending.get_json()
    expect(pending.status_code == 200, "device token reads pending command")
    expect(pending_json["pending_command"] == "arm", "device sees arm pending command")
    expect(pending_json["pending_command_id"] == arm_json["pending_command_id"], "device sees matching command id")

    stale_ack = client.post(
        f"/shop/{shop_id}/pending-command/ack",
        headers=device_headers,
        json={"pending_command_id": "wrong-id"},
    )
    print(f"stale ack: HTTP {stale_ack.status_code}: {stale_ack.get_data(as_text=True)}")
    expect(stale_ack.status_code == 409, "stale ack is rejected")

    ack = client.post(
        f"/shop/{shop_id}/pending-command/ack",
        headers=device_headers,
        json={"pending_command_id": arm_json["pending_command_id"]},
    )
    print(f"ack: HTTP {ack.status_code}: {ack.get_data(as_text=True)}")
    ack_json = ack.get_json()
    expect(ack.status_code == 200, "matching ack succeeds")
    expect(ack_json["pending_command"] is None, "ack clears pending command")
    expect(ack_json["pending_command_id"] is None, "ack clears pending command id")

    disarm = client.post(f"/shop/{shop_id}/disarm", headers=auth_headers)
    print(f"disarm: HTTP {disarm.status_code}: {disarm.get_data(as_text=True)}")
    disarm_json = disarm.get_json()
    expect(disarm.status_code == 200, "disarm endpoint succeeds")
    expect(disarm_json["armed"] is False, "disarm endpoint updates intended disarmed state")
    expect(disarm_json["pending_command"] == "disarm", "disarm endpoint sets pending command")

    status = client.get(f"/shop/{shop_id}/status", headers=auth_headers)
    print(f"status: HTTP {status.status_code}: {status.get_data(as_text=True)}")
    expect(status.status_code == 200, "status endpoint succeeds")
    expect(status.get_json()["pending_command"] == "disarm", "status exposes pending command for app UI")

    print("PASS: pending-command backend lifecycle behaved as expected")


if __name__ == "__main__":
    try:
        main()
    finally:
        try:
            os.unlink(db_file.name)
        except OSError:
            pass
