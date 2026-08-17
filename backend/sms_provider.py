import os

import requests


LAST_SIMULATED_SMS = None


def env_bool(name, default=False):
    value = os.getenv(name)
    if value is None:
        return default
    return value.strip().lower() in ("1", "true", "yes", "on")


def normalize_india_phone(phone_number):
    digits = "".join(ch for ch in str(phone_number or "") if ch.isdigit())
    if len(digits) == 10:
        return f"+91{digits}"
    if len(digits) == 12 and digits.startswith("91"):
        return f"+{digits}"
    if str(phone_number or "").strip().startswith("+"):
        return f"+{digits}"
    return f"+{digits}" if digits else ""


def send_password_reset_otp(phone_number, otp):
    normalized_phone = normalize_india_phone(phone_number)
    if env_bool("SIMULATE_SMS", default=True):
        return simulate_sms(normalized_phone, otp)

    provider = os.getenv("SMS_PROVIDER", "msg91").strip().lower()
    if provider != "msg91":
        raise RuntimeError(f"unsupported SMS_PROVIDER: {provider}")

    return send_msg91_password_reset_otp(normalized_phone, otp)


def simulate_sms(phone_number, otp):
    global LAST_SIMULATED_SMS
    LAST_SIMULATED_SMS = {
        "phone_number": phone_number,
        "otp": str(otp),
        "message": f"AMTECH password reset OTP: {otp}. It expires in 10 minutes.",
    }
    print(
        f"SIMULATE_SMS: Would send password reset OTP {otp} to {phone_number}",
        flush=True,
    )
    return {"ok": True, "simulated": True}


def send_msg91_password_reset_otp(phone_number, otp):
    auth_key = os.environ["MSG91_AUTH_KEY"]
    sender_id = os.environ.get("MSG91_SENDER_ID", "AMTECH")
    otp_expiry_minutes = int(os.environ.get("PASSWORD_RESET_OTP_EXPIRY_MINUTES", "10"))
    message_template = os.environ.get(
        "MSG91_PASSWORD_RESET_MESSAGE",
        "Your AMTECH password reset OTP is ##OTP##. It expires in 10 minutes.",
    )
    message = message_template.replace("##OTP##", str(otp))

    # MSG91's OTP API expects the mobile number in international format without
    # the leading plus sign. Production use requires the sender/template to be
    # approved under India's DLT rules before SIMULATE_SMS is disabled.
    response = requests.get(
        "https://api.msg91.com/api/sendotp.php",
        params={
            "authkey": auth_key,
            "mobile": phone_number.removeprefix("+"),
            "message": message,
            "sender": sender_id,
            "otp": otp,
            "otp_expiry": otp_expiry_minutes,
        },
        timeout=10,
    )
    response.raise_for_status()
    body = response.json()
    if body.get("type") != "success":
        raise RuntimeError(f"MSG91 OTP send failed: {body}")
    return {"ok": True, "provider": "msg91"}


def get_last_simulated_sms():
    return LAST_SIMULATED_SMS


def clear_last_simulated_sms():
    global LAST_SIMULATED_SMS
    LAST_SIMULATED_SMS = None
