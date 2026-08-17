import os
import tempfile
from datetime import timedelta
from uuid import uuid4


os.environ.setdefault("DATABASE_URL", f"sqlite:///{tempfile.gettempdir()}/amtech_forgot_password_test.db")
os.environ.setdefault("JWT_SECRET", "forgot-password-test-secret")
os.environ.setdefault("SIMULATE_SMS", "1")
os.environ.setdefault("RATELIMIT_STORAGE_URI", "memory://")

from app import app, now_utc  # noqa: E402
from database import PasswordResetOtp, SessionLocal  # noqa: E402
from sms_provider import clear_last_simulated_sms, get_last_simulated_sms  # noqa: E402


def print_response(label, response):
    print(f"{label}: HTTP {response.status_code}: {response.get_data(as_text=True)}")


def require_status(label, response, status_code):
    print_response(label, response)
    if response.status_code != status_code:
        raise RuntimeError(f"{label} returned HTTP {response.status_code}, expected {status_code}")
    return response.get_json()


def unique_phone():
    return f"+9198{uuid4().int % 100000000:08d}"


def signup(client, phone, password="old-password-123"):
    email = f"forgot-{uuid4().hex[:12]}@example.com"
    return require_status(
        "signup",
        client.post(
            "/auth/signup",
            json={"email": email, "password": password, "phone_number": phone},
            headers={"X-Forwarded-For": f"10.1.{uuid4().int % 200}.{uuid4().int % 200}"},
        ),
        201,
    ), email


def request_otp(client, phone, label="forgot password"):
    clear_last_simulated_sms()
    response = client.post(
        "/auth/forgot-password",
        json={"phone_number": phone},
        headers={"X-Forwarded-For": f"10.2.{uuid4().int % 200}.{uuid4().int % 200}"},
    )
    require_status(label, response, 200)
    simulated_sms = get_last_simulated_sms()
    if not simulated_sms or simulated_sms["phone_number"] != phone:
        raise RuntimeError(f"{label}: simulated SMS was not captured for {phone}")
    print(f"{label}: simulated OTP captured for {simulated_sms['phone_number']} otp={simulated_sms['otp']}")
    return simulated_sms["otp"]


def verify_reset(client, phone, otp, new_password, label, expected_status=200):
    return require_status(
        label,
        client.post(
            "/auth/verify-reset-otp",
            json={"phone_number": phone, "otp": otp, "new_password": new_password},
            headers={"X-Forwarded-For": f"10.3.{uuid4().int % 200}.{uuid4().int % 200}"},
        ),
        expected_status,
    )


def login(client, email, password, label, expected_status=200):
    return require_status(
        label,
        client.post(
            "/auth/login",
            json={"email": email, "password": password},
            headers={"X-Forwarded-For": f"10.4.{uuid4().int % 200}.{uuid4().int % 200}"},
        ),
        expected_status,
    )


def expire_latest_otp(phone):
    db = SessionLocal()
    try:
        reset_otp = (
            db.query(PasswordResetOtp)
            .filter(PasswordResetOtp.phone_number == phone, PasswordResetOtp.used_at.is_(None))
            .order_by(PasswordResetOtp.created_at.desc())
            .first()
        )
        if reset_otp is None:
            raise RuntimeError("no OTP row found to expire")
        reset_otp.expires_at = now_utc() - timedelta(seconds=1)
        db.commit()
    finally:
        db.close()


def test_valid_flow(client):
    phone = unique_phone()
    signup(client, phone)
    otp = request_otp(client, phone)
    verify_reset(client, phone, otp, "new-password-456", "verify valid reset OTP")
    _, email = signup(client, unique_phone(), password="throwaway-password")
    print(f"control signup created unrelated user {email}")


def test_password_changes(client):
    phone = unique_phone()
    _, email = signup(client, phone)
    otp = request_otp(client, phone)
    verify_reset(client, phone, otp, "new-password-789", "verify password change OTP")
    login(client, email, "old-password-123", "old password rejected after reset", expected_status=401)
    login(client, email, "new-password-789", "new password accepted after reset")


def test_wrong_otp(client):
    phone = unique_phone()
    signup(client, phone)
    request_otp(client, phone)
    verify_reset(client, phone, "000000", "new-password-456", "wrong OTP rejected", expected_status=400)


def test_expired_otp(client):
    phone = unique_phone()
    signup(client, phone)
    otp = request_otp(client, phone)
    expire_latest_otp(phone)
    verify_reset(client, phone, otp, "new-password-456", "expired OTP rejected", expected_status=400)


def test_rate_limiting(client):
    phone = unique_phone()
    ip = "10.250.1.1"
    for attempt in range(1, 5):
        response = client.post(
            "/auth/forgot-password",
            json={"phone_number": phone},
            headers={"X-Forwarded-For": ip},
        )
        expected = 429 if attempt == 4 else 200
        require_status(f"forgot-password rate limit attempt {attempt}", response, expected)

    verify_ip = "10.250.1.2"
    for attempt in range(1, 7):
        response = client.post(
            "/auth/verify-reset-otp",
            json={"phone_number": phone, "otp": "111111", "new_password": "new-password-456"},
            headers={"X-Forwarded-For": verify_ip},
        )
        expected = 429 if attempt == 6 else 400
        require_status(f"verify-reset-otp rate limit attempt {attempt}", response, expected)


def main():
    app.config.update(TESTING=True)
    client = app.test_client()

    test_valid_flow(client)
    test_password_changes(client)
    test_wrong_otp(client)
    test_expired_otp(client)
    test_rate_limiting(client)
    print("forgot password tests: PASS")


if __name__ == "__main__":
    main()
