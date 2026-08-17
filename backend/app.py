import os
import hashlib
import hmac
import secrets
import threading
import time
from datetime import datetime, timedelta, timezone
from functools import wraps
from uuid import uuid4

import bcrypt
import boto3
import jwt
import requests
from flask import Flask, g, jsonify, request
from flask_limiter import Limiter
from flask_limiter.util import get_remote_address
from sqlalchemy import text
from sqlalchemy.exc import IntegrityError

from database import (
    Alert,
    Camera,
    CameraInventory,
    Device,
    PasswordResetOtp,
    PushToken,
    Shop,
    ShopDeviceSchedule,
    ShopEmergencyContact,
    SessionLocal,
    User,
    init_db,
)
from sms_provider import normalize_india_phone, send_password_reset_otp


app = Flask(__name__)
JWT_SECRET = os.environ["JWT_SECRET"]
R2_PUBLIC_BASE_URL = os.getenv("R2_PUBLIC_BASE_URL", "https://pub-9585184cf02549f0a6e3e31090670c37.r2.dev")
UPLOAD_URL_EXPIRES_SECONDS = 900
DATABASE_KEEPALIVE_INTERVAL_SECONDS = int(os.getenv("DATABASE_KEEPALIVE_INTERVAL_SECONDS", "240"))
EXPO_PUSH_SEND_URL = "https://exp.host/--/api/v2/push/send"
PUSH_ALERT_EVENT_TYPES = {"intrusion", "intrusion-front", "intrusion-parking"}
DEFAULT_DEVICE_CONFIG_CONTACTS = [
    {"slot": 1, "name": "", "phone": "+918550991121"},
    {"slot": 2, "name": "", "phone": "+919922434811"},
    {"slot": 3, "name": "", "phone": "+919922435710"},
]
PASSWORD_RESET_OTP_EXPIRY_MINUTES = int(os.getenv("PASSWORD_RESET_OTP_EXPIRY_MINUTES", "10"))


def rate_limit_key():
    forwarded_for = request.headers.get("X-Forwarded-For", "")
    if forwarded_for:
        return forwarded_for.split(",", 1)[0].strip()
    return get_remote_address()


# In-memory limits are fine for the current single-instance Railway pilot.
# Move to a shared store such as Redis before running multiple backend instances.
limiter = Limiter(rate_limit_key, app=app)


@app.errorhandler(429)
def rate_limit_exceeded(_error):
    return jsonify({
        "ok": False,
        "error": "Too many requests. Please wait a minute and try again.",
    }), 429


def env_bool(name, default=False):
    value = os.getenv(name)
    if value is None:
        return default
    return value.strip().lower() in ("1", "true", "yes", "on")


def database_keepalive_ping():
    db = SessionLocal()
    try:
        db.execute(text("SELECT 1"))
        print("Database keep-alive ping succeeded", flush=True)
    finally:
        db.close()


def database_keepalive_loop(interval_seconds):
    while True:
        time.sleep(interval_seconds)
        try:
            database_keepalive_ping()
        except Exception as exc:
            print(f"Database keep-alive ping failed: {exc}", flush=True)


def start_database_keepalive():
    if env_bool("DATABASE_KEEPALIVE_DISABLED", default=False):
        print("Database keep-alive disabled", flush=True)
        return

    thread = threading.Thread(
        target=database_keepalive_loop,
        args=(DATABASE_KEEPALIVE_INTERVAL_SECONDS,),
        daemon=True,
        name="database-keepalive",
    )
    thread.start()
    print(
        f"Database keep-alive scheduled every {DATABASE_KEEPALIVE_INTERVAL_SECONDS} seconds",
        flush=True,
    )


init_db()
start_database_keepalive()


def jwt_secret():
    return JWT_SECRET


def hash_password(password):
    return bcrypt.hashpw(password.encode("utf-8"), bcrypt.gensalt()).decode("utf-8")


def verify_password(password, password_hash):
    return bcrypt.checkpw(password.encode("utf-8"), password_hash.encode("utf-8"))


def otp_secret():
    return os.getenv("PASSWORD_RESET_OTP_SECRET", jwt_secret())


def hash_otp(phone_number, otp):
    message = f"{normalize_india_phone(phone_number)}:{otp}".encode("utf-8")
    return hmac.new(otp_secret().encode("utf-8"), message, hashlib.sha256).hexdigest()


def generate_otp():
    return f"{secrets.randbelow(1000000):06d}"


def now_utc():
    return datetime.now(timezone.utc)


def is_expired(expires_at):
    if expires_at.tzinfo is None:
        expires_at = expires_at.replace(tzinfo=timezone.utc)
    return expires_at <= now_utc()


def create_token(user):
    payload = {
        "sub": user.id,
        "email": user.email,
        "iat": int(datetime.now(timezone.utc).timestamp()),
    }
    return jwt.encode(payload, jwt_secret(), algorithm="HS256")


def auth_required(view):
    @wraps(view)
    def wrapped(*args, **kwargs):
        header = request.headers.get("Authorization", "")
        if not header.startswith("Bearer "):
            return jsonify({"ok": False, "error": "authorization token is required"}), 401

        token = header.removeprefix("Bearer ").strip()
        try:
            payload = jwt.decode(token, jwt_secret(), algorithms=["HS256"])
        except KeyError as exc:
            return jsonify({"ok": False, "error": f"missing environment variable: {exc.args[0]}"}), 500
        except jwt.PyJWTError:
            return jsonify({"ok": False, "error": "invalid authorization token"}), 401

        user_id = payload.get("sub")
        if not user_id:
            return jsonify({"ok": False, "error": "invalid authorization token"}), 401

        db = SessionLocal()
        try:
            user = db.get(User, user_id)
            if user is None:
                return jsonify({"ok": False, "error": "invalid authorization token"}), 401
            g.user_id = user.id
            g.user_email = user.email
            g.user_phone = user.phone_number
        finally:
            db.close()

        return view(*args, **kwargs)

    return wrapped


def find_user_by_phone(db, phone_number):
    normalized_target = normalize_india_phone(phone_number)
    users = db.query(User).filter(User.phone_number.isnot(None)).all()
    for user in users:
        if normalize_india_phone(user.phone_number) == normalized_target:
            return user
    return None


def r2_client():
    account_id = os.environ["R2_ACCOUNT_ID"]
    return boto3.client(
        "s3",
        endpoint_url=f"https://{account_id}.r2.cloudflarestorage.com",
        aws_access_key_id=os.environ["R2_ACCESS_KEY_ID"],
        aws_secret_access_key=os.environ["R2_SECRET_ACCESS_KEY"],
        region_name="auto",
    )


def extension_for_content_type(content_type):
    extensions = {
        "image/jpeg": "jpg",
        "image/png": "png",
        "video/mp4": "mp4",
    }
    return extensions.get(content_type)


def parse_media_upload_request(payload):
    if not isinstance(payload, dict):
        raise ValueError("request body must be a JSON object")

    content_type = payload.get("content_type")
    if not content_type:
        raise ValueError("content_type is required")

    normalized = str(content_type).strip().lower()
    extension = extension_for_content_type(normalized)
    if extension is None:
        raise ValueError("content_type must be one of image/jpeg, image/png, video/mp4")

    return {
        "content_type": normalized,
        "extension": extension,
    }


def parse_alert(payload):
    if not isinstance(payload, dict):
        raise ValueError("request body must be a JSON object")

    shop_id = payload.get("shop_id")
    event_type = payload.get("event_type")
    timestamp = payload.get("timestamp")
    media_url = payload.get("media_url")

    if not shop_id:
        raise ValueError("shop_id is required")

    if event_type not in (
        "intrusion",
        "intrusion-front",
        "intrusion-parking",
        "shutter",
        "shutter-1",
        "shutter-2",
        "panic",
        "smoke",
        "test",
    ):
        raise ValueError(
            "event_type must be one of intrusion, intrusion-front, intrusion-parking, shutter, shutter-1, shutter-2, panic, smoke, test"
        )

    if not timestamp:
        timestamp = datetime.now(timezone.utc).isoformat()

    return {
        "shop_id": str(shop_id),
        "event_type": event_type,
        "timestamp": str(timestamp),
        "media_url": str(media_url) if media_url else None,
    }


def parse_push_token(payload):
    if not isinstance(payload, dict):
        raise ValueError("request body must be a JSON object")

    token = str(payload.get("expo_push_token", "")).strip()
    platform = payload.get("platform")

    if not token:
        raise ValueError("expo_push_token is required")

    if not (token.startswith("ExponentPushToken[") or token.startswith("ExpoPushToken[")):
        raise ValueError("expo_push_token must be an Expo push token")

    return {
        "expo_push_token": token,
        "platform": str(platform).strip()[:32] if platform else None,
    }


def parse_shop_registration(payload):
    if not isinstance(payload, dict):
        raise ValueError("request body must be a JSON object")

    shop_name = payload.get("shop_name")
    owner_name = payload.get("owner_name")
    address = payload.get("address")
    device_serial = payload.get("device_serial")

    if not shop_name:
        raise ValueError("shop_name is required")
    if not owner_name:
        raise ValueError("owner_name is required")
    if not address:
        raise ValueError("address is required")
    if not device_serial:
        raise ValueError("device_serial is required")

    return {
        "shop_name": str(shop_name),
        "owner_name": str(owner_name),
        "address": str(address),
        "device_serial": str(device_serial),
    }


def parse_camera_registration(payload):
    if not isinstance(payload, dict):
        raise ValueError("request body must be a JSON object")

    camera_serial = payload.get("camera_serial")
    slot_number = payload.get("slot_number")
    camera_ip = payload.get("camera_ip")
    camera_username = payload.get("camera_username")
    camera_password = payload.get("camera_password")

    if not camera_serial:
        raise ValueError("camera_serial is required")
    if slot_number is None:
        raise ValueError("slot_number is required")

    try:
        normalized_slot = int(slot_number)
    except (TypeError, ValueError):
        raise ValueError("slot_number must be 1 or 2")

    if normalized_slot not in (1, 2):
        raise ValueError("slot_number must be 1 or 2")

    return {
        "camera_serial": str(camera_serial).strip().upper(),
        "slot_number": normalized_slot,
        "camera_ip": str(camera_ip).strip() if camera_ip else None,
        "camera_username": str(camera_username).strip() if camera_username else None,
        "camera_password": str(camera_password) if camera_password else None,
    }


def parse_time_parts(value, field_name):
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        raise ValueError(f"{field_name} must be an integer")
    return parsed


def validate_hour_minute(hour, minute, label):
    if hour < 0 or hour > 23:
        raise ValueError(f"{label}_hour must be between 0 and 23")
    if minute < 0 or minute > 59:
        raise ValueError(f"{label}_minute must be between 0 and 59")


def parse_device_config_update(payload):
    if not isinstance(payload, dict):
        raise ValueError("request body must be a JSON object")

    schedule = payload.get("schedule", {})
    contacts = payload.get("emergency_contacts", [])
    if schedule is None:
        schedule = {}
    if not isinstance(schedule, dict):
        raise ValueError("schedule must be an object")
    if not isinstance(contacts, list):
        raise ValueError("emergency_contacts must be a list")
    if len(contacts) > 3:
        raise ValueError("emergency_contacts may contain at most 3 contacts")

    arm_hour = parse_time_parts(schedule.get("arm_hour", 23), "arm_hour")
    arm_minute = parse_time_parts(schedule.get("arm_minute", 0), "arm_minute")
    disarm_hour = parse_time_parts(schedule.get("disarm_hour", 6), "disarm_hour")
    disarm_minute = parse_time_parts(schedule.get("disarm_minute", 0), "disarm_minute")
    validate_hour_minute(arm_hour, arm_minute, "arm")
    validate_hour_minute(disarm_hour, disarm_minute, "disarm")

    normalized_contacts = []
    seen_slots = set()
    for contact in contacts:
        if not isinstance(contact, dict):
            raise ValueError("each emergency contact must be an object")
        slot = parse_time_parts(contact.get("slot"), "slot")
        if slot < 1 or slot > 3:
            raise ValueError("contact slot must be 1, 2, or 3")
        if slot in seen_slots:
            raise ValueError("duplicate contact slot")
        seen_slots.add(slot)

        phone = str(contact.get("phone", "")).strip()
        if not phone:
            raise ValueError("contact phone is required")

        normalized_contacts.append({
            "slot": slot,
            "name": str(contact.get("name", "")).strip(),
            "phone": phone,
        })

    return {
        "schedule": {
            "arm_hour": arm_hour,
            "arm_minute": arm_minute,
            "disarm_hour": disarm_hour,
            "disarm_minute": disarm_minute,
        },
        "emergency_contacts": normalized_contacts,
    }


def parse_signup(payload):
    if not isinstance(payload, dict):
        raise ValueError("request body must be a JSON object")

    email = payload.get("email")
    password = payload.get("password")
    phone_number = payload.get("phone_number")

    if not email:
        raise ValueError("email is required")
    if not password:
        raise ValueError("password is required")
    if not phone_number:
        raise ValueError("phone_number is required")

    return {
        "email": str(email).strip().lower(),
        "password": str(password),
        "phone_number": str(phone_number),
    }


def parse_login(payload):
    if not isinstance(payload, dict):
        raise ValueError("request body must be a JSON object")

    email = payload.get("email")
    password = payload.get("password")

    if not email:
        raise ValueError("email is required")
    if not password:
        raise ValueError("password is required")

    return {
        "email": str(email).strip().lower(),
        "password": str(password),
    }


def parse_forgot_password(payload):
    if not isinstance(payload, dict):
        raise ValueError("request body must be a JSON object")

    phone_number = normalize_india_phone(payload.get("phone_number"))
    if not phone_number:
        raise ValueError("phone_number is required")
    if not phone_number.startswith("+91") or len(phone_number) != 13:
        raise ValueError("phone_number must use +91XXXXXXXXXX format")

    return {"phone_number": phone_number}


def parse_verify_reset_otp(payload):
    if not isinstance(payload, dict):
        raise ValueError("request body must be a JSON object")

    phone_number = normalize_india_phone(payload.get("phone_number"))
    otp = str(payload.get("otp", "")).strip()
    new_password = str(payload.get("new_password", ""))

    if not phone_number:
        raise ValueError("phone_number is required")
    if not phone_number.startswith("+91") or len(phone_number) != 13:
        raise ValueError("phone_number must use +91XXXXXXXXXX format")
    if not otp:
        raise ValueError("otp is required")
    if not otp.isdigit() or len(otp) != 6:
        raise ValueError("otp must be 6 digits")
    if not new_password:
        raise ValueError("new_password is required")
    if len(new_password) < 8:
        raise ValueError("new_password must be at least 8 characters")

    return {
        "phone_number": phone_number,
        "otp": otp,
        "new_password": new_password,
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
        get_or_create_shop(db, alert_payload["shop_id"])

        alert_row = Alert(
            id=str(uuid4()),
            shop_id=alert_payload["shop_id"],
            event_type=alert_payload["event_type"],
            timestamp=parse_timestamp(alert_payload["timestamp"]),
            media_url=alert_payload["media_url"],
        )
        db.add(alert_row)
        db.commit()
        db.refresh(alert_row)
        send_push_for_alert(db, alert_row)
        return alert_row
    except Exception:
        db.rollback()
        raise
    finally:
        db.close()


def push_title_for_event(event_type):
    if event_type == "intrusion-front":
        return "Intrusion Detected - Cam 1"
    if event_type == "intrusion-parking":
        return "Intrusion Detected - Cam 2"
    return "Intrusion Detected"


def push_body_for_event(shop, event_type):
    shop_name = shop.shop_name if shop else "your shop"
    if event_type == "intrusion-front":
        return f"Person detected by Cam 1 at {shop_name}."
    if event_type == "intrusion-parking":
        return f"Person detected by Cam 2 at {shop_name}."
    return f"Person detected at {shop_name}."


def send_expo_push_messages(messages):
    if not messages:
        return None

    if env_bool("SIMULATE_PUSH", default=False):
        print(f"SIMULATE_PUSH: would send Expo push messages: {messages}", flush=True)
        return {"simulated": True, "count": len(messages)}

    response = requests.post(
        EXPO_PUSH_SEND_URL,
        json=messages,
        headers={
            "Accept": "application/json",
            "Accept-Encoding": "gzip, deflate",
            "Content-Type": "application/json",
        },
        timeout=10,
    )
    print(f"Expo push response: HTTP {response.status_code} {response.text}", flush=True)
    response.raise_for_status()
    return response.json()


def send_push_for_alert(db, alert_row):
    if alert_row.event_type not in PUSH_ALERT_EVENT_TYPES:
        return

    shop = db.get(Shop, alert_row.shop_id)
    if shop is None or not shop.user_id:
        print(f"Push skipped: alert shop {alert_row.shop_id} has no owner user", flush=True)
        return

    token_rows = (
        db.query(PushToken)
        .filter(PushToken.user_id == shop.user_id)
        .all()
    )
    if not token_rows:
        print(f"Push skipped: owner for shop {alert_row.shop_id} has no registered push tokens", flush=True)
        return

    messages = [
        {
            "to": token_row.expo_push_token,
            "title": push_title_for_event(alert_row.event_type),
            "body": push_body_for_event(shop, alert_row.event_type),
            "sound": "default",
            "data": {
                "shop_id": alert_row.shop_id,
                "alert_id": alert_row.id,
                "event_type": alert_row.event_type,
            },
        }
        for token_row in token_rows
    ]

    try:
        send_expo_push_messages(messages)
    except Exception as exc:
        print(f"Warning: Expo push send failed for alert {alert_row.id}: {exc}", flush=True)


def get_or_create_shop(db, shop_id):
    shop = db.get(Shop, shop_id)
    if shop is not None:
        return shop

    print(
        f"Warning: shop_id {shop_id} not found; creating placeholder shop record",
        flush=True,
    )
    shop = Shop(
        id=shop_id,
        shop_name=f"Unregistered shop {shop_id}",
        armed=False,
    )
    db.add(shop)
    db.commit()
    db.refresh(shop)
    return shop


def alert_to_dict(alert_row):
    timestamp = alert_row.timestamp
    if timestamp.tzinfo is None:
        timestamp = timestamp.replace(tzinfo=timezone.utc)

    return {
        "id": alert_row.id,
        "shop_id": alert_row.shop_id,
        "event_type": alert_row.event_type,
        "timestamp": timestamp.isoformat(),
        "media_url": alert_row.media_url,
    }


def shop_status_to_dict(shop):
    return {
        "ok": True,
        "shop_id": shop.id,
        "armed": bool(shop.armed),
        "hub_online": True,
        "battery_level": 100,
    }


def camera_to_dict(camera):
    return {
        "id": camera.id,
        "shop_id": camera.shop_id,
        "camera_serial": camera.camera_serial,
        "slot_number": camera.slot_number,
        "enabled": bool(camera.enabled),
    }


def set_shop_armed(shop_id, armed):
    db = SessionLocal()
    try:
        shop, error_response = owned_shop_or_response(db, shop_id)
        if error_response:
            return error_response

        shop.armed = armed
        db.commit()
        db.refresh(shop)
        return jsonify(shop_status_to_dict(shop))
    except Exception:
        db.rollback()
        raise
    finally:
        db.close()


def shop_to_dict(shop):
    device = shop.devices[0] if shop.devices else None
    return {
        "ok": True,
        "shop_id": shop.id,
        "shop_name": shop.shop_name,
        "owner_name": shop.owner_name,
        "address": shop.address,
        "owner_phone": shop.owner_phone,
        "owner_email": shop.owner_email,
        "device_serial": device.device_serial if device else None,
        "cameras": [camera_to_dict(camera) for camera in sorted(shop.cameras, key=lambda row: row.slot_number)],
        "armed": bool(shop.armed),
    }


def owned_shop_or_response(db, shop_id):
    shop = db.get(Shop, shop_id)
    if shop is None:
        return None, (jsonify({"ok": False, "error": "shop not found", "shop_id": shop_id}), 404)

    if shop.user_id != g.user_id:
        return None, (jsonify({"ok": False, "error": "you do not have access to this shop", "shop_id": shop_id}), 403)

    return shop, None


def user_from_bearer_token(db):
    header = request.headers.get("Authorization", "")
    if not header.startswith("Bearer "):
        return None

    token = header.removeprefix("Bearer ").strip()
    try:
        payload = jwt.decode(token, jwt_secret(), algorithms=["HS256"])
    except jwt.PyJWTError:
        return None

    user_id = payload.get("sub")
    if not user_id:
        return None
    return db.get(User, user_id)


def device_config_shop_or_response(db, shop_id):
    shop = db.get(Shop, shop_id)
    if shop is None:
        return None, (jsonify({"ok": False, "error": "shop not found", "shop_id": shop_id}), 404)

    configured_token = os.getenv("DEVICE_CONFIG_SYNC_TOKEN", "").strip()
    provided_token = request.headers.get("X-AMTECH-DEVICE-CONFIG-TOKEN", "").strip()
    if configured_token and provided_token and provided_token == configured_token:
        return shop, None

    user = user_from_bearer_token(db)
    if user is None:
        return None, (jsonify({"ok": False, "error": "authorization token or device config token is required"}), 401)
    if shop.user_id != user.id:
        return None, (jsonify({"ok": False, "error": "you do not have access to this shop", "shop_id": shop_id}), 403)

    return shop, None


def shop_summary_to_dict(shop):
    device = shop.devices[0] if shop.devices else None
    return {
        "shop_id": shop.id,
        "shop_name": shop.shop_name,
        "owner_name": shop.owner_name,
        "address": shop.address,
        "owner_phone": shop.owner_phone,
        "owner_email": shop.owner_email,
        "device_serial": device.device_serial if device else None,
        "camera_count": len([camera for camera in shop.cameras if camera.enabled]),
        "armed": bool(shop.armed),
    }


def get_or_seed_camera_inventory(db, payload):
    inventory = db.get(CameraInventory, payload["camera_serial"])
    if inventory is not None:
        return inventory

    if payload["camera_ip"] and payload["camera_username"] and payload["camera_password"]:
        inventory = CameraInventory(
            camera_serial=payload["camera_serial"],
            camera_ip=payload["camera_ip"],
            camera_username=payload["camera_username"],
            camera_password=payload["camera_password"],
        )
        db.add(inventory)
        db.flush()
        return inventory

    return None


def default_device_schedule():
    return {
        "arm_hour": 23,
        "arm_minute": 0,
        "disarm_hour": 6,
        "disarm_minute": 0,
    }


def device_schedule_to_dict(schedule):
    if schedule is None:
        return default_device_schedule()

    return {
        "arm_hour": int(schedule.arm_hour),
        "arm_minute": int(schedule.arm_minute),
        "disarm_hour": int(schedule.disarm_hour),
        "disarm_minute": int(schedule.disarm_minute),
    }


def emergency_contacts_to_dict(contact_rows):
    by_slot = {
        contact.slot_number: {
            "slot": contact.slot_number,
            "name": contact.name or "",
            "phone": contact.phone,
        }
        for contact in contact_rows
    }

    return [
        by_slot.get(default_contact["slot"], default_contact)
        for default_contact in DEFAULT_DEVICE_CONFIG_CONTACTS
    ]


def device_config_to_dict(shop):
    return {
        "ok": True,
        "shop_id": shop.id,
        "schedule": device_schedule_to_dict(shop.device_schedule),
        "emergency_contacts": emergency_contacts_to_dict(
            sorted(shop.emergency_contacts, key=lambda row: row.slot_number)
        ),
    }


def upsert_device_config(db, shop, payload):
    now = datetime.now(timezone.utc)
    schedule_payload = payload["schedule"]
    schedule_row = shop.device_schedule
    if schedule_row is None:
        schedule_row = ShopDeviceSchedule(shop=shop)
        db.add(schedule_row)

    schedule_row.arm_hour = schedule_payload["arm_hour"]
    schedule_row.arm_minute = schedule_payload["arm_minute"]
    schedule_row.disarm_hour = schedule_payload["disarm_hour"]
    schedule_row.disarm_minute = schedule_payload["disarm_minute"]
    schedule_row.updated_at = now

    existing_contacts = {
        contact.slot_number: contact
        for contact in shop.emergency_contacts
    }
    for contact_payload in payload["emergency_contacts"]:
        slot = contact_payload["slot"]
        contact = existing_contacts.get(slot)
        if contact is None:
            contact = ShopEmergencyContact(
                id=str(uuid4()),
                shop=shop,
                slot_number=slot,
            )
            db.add(contact)
        contact.name = contact_payload["name"]
        contact.phone = contact_payload["phone"]
        contact.updated_at = now


@app.post("/me/push-token")
@auth_required
def register_push_token():
    try:
        payload = parse_push_token(request.get_json(silent=True))
    except ValueError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400

    db = SessionLocal()
    try:
        now = datetime.now(timezone.utc)
        token_row = (
            db.query(PushToken)
            .filter(PushToken.expo_push_token == payload["expo_push_token"])
            .first()
        )
        if token_row is None:
            token_row = PushToken(
                id=str(uuid4()),
                user_id=g.user_id,
                expo_push_token=payload["expo_push_token"],
                platform=payload["platform"],
                created_at=now,
                updated_at=now,
            )
            db.add(token_row)
        else:
            token_row.user_id = g.user_id
            token_row.platform = payload["platform"]
            token_row.updated_at = now

        db.commit()
        return jsonify({"ok": True})
    except IntegrityError:
        db.rollback()
        return jsonify({"ok": False, "error": "push token is already registered"}), 409
    finally:
        db.close()


@app.post("/auth/signup")
@limiter.limit("3 per minute")
def signup():
    try:
        payload = parse_signup(request.get_json(silent=True))
    except ValueError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400

    db = SessionLocal()
    try:
        existing_user = db.query(User).filter(User.email == payload["email"]).first()
        if existing_user is not None:
            return jsonify({"ok": False, "error": "email is already registered"}), 409

        user = User(
            id=str(uuid4()),
            email=payload["email"],
            password_hash=hash_password(payload["password"]),
            phone_number=payload["phone_number"],
        )
        db.add(user)
        db.commit()
        db.refresh(user)
        return jsonify({"ok": True, "token": create_token(user), "user_id": user.id, "email": user.email}), 201
    except IntegrityError:
        db.rollback()
        return jsonify({"ok": False, "error": "email is already registered"}), 409
    except KeyError as exc:
        db.rollback()
        return jsonify({"ok": False, "error": f"missing environment variable: {exc.args[0]}"}), 500
    except Exception:
        db.rollback()
        raise
    finally:
        db.close()


@app.post("/auth/login")
@limiter.limit("5 per minute")
def login():
    try:
        payload = parse_login(request.get_json(silent=True))
    except ValueError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400

    db = SessionLocal()
    try:
        user = db.query(User).filter(User.email == payload["email"]).first()
        if user is None or not verify_password(payload["password"], user.password_hash):
            return jsonify({"ok": False, "error": "invalid email or password"}), 401

        return jsonify({"ok": True, "token": create_token(user), "user_id": user.id, "email": user.email})
    except KeyError as exc:
        return jsonify({"ok": False, "error": f"missing environment variable: {exc.args[0]}"}), 500
    finally:
        db.close()


@app.post("/auth/forgot-password")
@limiter.limit("3 per minute")
def forgot_password():
    try:
        payload = parse_forgot_password(request.get_json(silent=True))
    except ValueError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400

    db = SessionLocal()
    try:
        user = find_user_by_phone(db, payload["phone_number"])
        if user is None:
            print(
                f"Password reset requested for unknown phone {payload['phone_number']}",
                flush=True,
            )
            return jsonify({
                "ok": True,
                "message": "If this number is registered, an OTP has been sent.",
            })

        issued_at = now_utc()
        otp = generate_otp()
        for existing_otp in (
            db.query(PasswordResetOtp)
            .filter(PasswordResetOtp.user_id == user.id, PasswordResetOtp.used_at.is_(None))
            .all()
        ):
            existing_otp.used_at = issued_at

        reset_otp = PasswordResetOtp(
            id=str(uuid4()),
            user_id=user.id,
            phone_number=payload["phone_number"],
            otp_hash=hash_otp(payload["phone_number"], otp),
            expires_at=issued_at + timedelta(minutes=PASSWORD_RESET_OTP_EXPIRY_MINUTES),
            created_at=issued_at,
        )
        db.add(reset_otp)
        send_password_reset_otp(payload["phone_number"], otp)
        db.commit()
        return jsonify({
            "ok": True,
            "message": "If this number is registered, an OTP has been sent.",
        })
    except KeyError as exc:
        db.rollback()
        return jsonify({"ok": False, "error": f"missing environment variable: {exc.args[0]}"}), 500
    except Exception:
        db.rollback()
        raise
    finally:
        db.close()


@app.post("/auth/verify-reset-otp")
@limiter.limit("5 per minute")
def verify_reset_otp():
    try:
        payload = parse_verify_reset_otp(request.get_json(silent=True))
    except ValueError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400

    db = SessionLocal()
    try:
        user = find_user_by_phone(db, payload["phone_number"])
        if user is None:
            return jsonify({"ok": False, "error": "invalid or expired OTP"}), 400

        reset_otp = (
            db.query(PasswordResetOtp)
            .filter(
                PasswordResetOtp.user_id == user.id,
                PasswordResetOtp.phone_number == payload["phone_number"],
                PasswordResetOtp.used_at.is_(None),
            )
            .order_by(PasswordResetOtp.created_at.desc())
            .first()
        )
        if reset_otp is None or is_expired(reset_otp.expires_at):
            return jsonify({"ok": False, "error": "invalid or expired OTP"}), 400

        if not hmac.compare_digest(reset_otp.otp_hash, hash_otp(payload["phone_number"], payload["otp"])):
            return jsonify({"ok": False, "error": "invalid or expired OTP"}), 400

        reset_otp.used_at = now_utc()
        user.password_hash = hash_password(payload["new_password"])
        db.commit()
        return jsonify({"ok": True, "message": "Password reset successful."})
    except Exception:
        db.rollback()
        raise
    finally:
        db.close()


@app.post("/alert")
def alert():
    try:
        alert_payload = parse_alert(request.get_json(silent=True))
        alert_row = record_alert(alert_payload)
    except ValueError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400
    except KeyError as exc:
        return jsonify({"ok": False, "error": f"missing environment variable: {exc.args[0]}"}), 500

    return jsonify({"ok": True, "alert": alert_to_dict(alert_row)})


@app.post("/shop")
@auth_required
def create_shop():
    try:
        payload = parse_shop_registration(request.get_json(silent=True))
    except ValueError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400

    db = SessionLocal()
    try:
        existing_device = (
            db.query(Device)
            .filter(Device.device_serial == payload["device_serial"])
            .first()
        )
        if existing_device is not None:
            return jsonify({
                "ok": False,
                "error": "device_serial is already registered",
                "device_serial": payload["device_serial"],
            }), 409

        shop = Shop(
            id=str(uuid4()),
            user_id=g.user_id,
            shop_name=payload["shop_name"],
            owner_name=payload["owner_name"],
            address=payload["address"],
            owner_phone=g.user_phone,
            owner_email=g.user_email,
            armed=False,
        )
        device = Device(
            id=str(uuid4()),
            shop=shop,
            device_serial=payload["device_serial"],
            status="offline",
            last_seen_at=None,
        )
        db.add(shop)
        db.add(device)
        db.commit()
        db.refresh(shop)
        db.refresh(device)

        return jsonify({
            "ok": True,
            "shop_id": shop.id,
            "shop_name": shop.shop_name,
            "owner_name": shop.owner_name,
            "address": shop.address,
            "owner_phone": shop.owner_phone,
            "owner_email": shop.owner_email,
            "device_serial": device.device_serial,
        }), 201
    except IntegrityError:
        db.rollback()
        return jsonify({
            "ok": False,
            "error": "device_serial is already registered",
            "device_serial": payload["device_serial"],
        }), 409
    except Exception:
        db.rollback()
        raise
    finally:
        db.close()


@app.get("/shop/<shop_id>")
@auth_required
def get_shop(shop_id):
    db = SessionLocal()
    try:
        shop, error_response = owned_shop_or_response(db, shop_id)
        if error_response:
            return error_response

        return jsonify(shop_to_dict(shop))
    finally:
        db.close()


@app.get("/me/shops")
@auth_required
def my_shops():
    db = SessionLocal()
    try:
        shop_rows = (
            db.query(Shop)
            .filter(Shop.user_id == g.user_id)
            .order_by(Shop.created_at.asc())
            .all()
        )
        return jsonify({"ok": True, "shops": [shop_summary_to_dict(shop) for shop in shop_rows]})
    finally:
        db.close()


@app.post("/shop/<shop_id>/camera")
@auth_required
def register_camera(shop_id):
    try:
        payload = parse_camera_registration(request.get_json(silent=True))
    except ValueError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400

    db = SessionLocal()
    try:
        shop, error_response = owned_shop_or_response(db, shop_id)
        if error_response:
            return error_response

        inventory = get_or_seed_camera_inventory(db, payload)
        if inventory is None:
            return jsonify({
                "ok": False,
                "error": "camera_serial is not in AMTECH camera inventory",
                "camera_serial": payload["camera_serial"],
            }), 404

        camera = (
            db.query(Camera)
            .filter(Camera.shop_id == shop.id, Camera.slot_number == payload["slot_number"])
            .first()
        )
        if camera is None:
            camera = Camera(
                id=str(uuid4()),
                shop=shop,
                camera_serial=payload["camera_serial"],
                slot_number=payload["slot_number"],
                enabled=True,
            )
            db.add(camera)
        else:
            camera.camera_serial = payload["camera_serial"]
            camera.enabled = True

        db.commit()
        db.refresh(camera)
        return jsonify({"ok": True, "camera": camera_to_dict(camera)}), 201
    except IntegrityError:
        db.rollback()
        return jsonify({
            "ok": False,
            "error": "slot_number is already registered",
            "camera_serial": payload["camera_serial"],
            "slot_number": payload["slot_number"],
        }), 409
    except Exception:
        db.rollback()
        raise
    finally:
        db.close()


@app.get("/shop/<shop_id>/cameras")
@auth_required
def get_shop_cameras(shop_id):
    db = SessionLocal()
    try:
        shop, error_response = owned_shop_or_response(db, shop_id)
        if error_response:
            return error_response

        camera_rows = (
            db.query(Camera)
            .filter(Camera.shop_id == shop.id)
            .order_by(Camera.slot_number.asc())
            .all()
        )
        return jsonify({"ok": True, "shop_id": shop.id, "cameras": [camera_to_dict(camera) for camera in camera_rows]})
    finally:
        db.close()


@app.post("/shop/<shop_id>/arm")
@auth_required
def arm_shop(shop_id):
    return set_shop_armed(shop_id, True)


@app.post("/shop/<shop_id>/disarm")
@auth_required
def disarm_shop(shop_id):
    return set_shop_armed(shop_id, False)


@app.get("/shop/<shop_id>/status")
@auth_required
def shop_status(shop_id):
    db = SessionLocal()
    try:
        shop, error_response = owned_shop_or_response(db, shop_id)
        if error_response:
            return error_response

        return jsonify(shop_status_to_dict(shop))
    except Exception:
        db.rollback()
        raise
    finally:
        db.close()


@app.get("/shop/<shop_id>/device-config")
def get_device_config(shop_id):
    db = SessionLocal()
    try:
        shop, error_response = device_config_shop_or_response(db, shop_id)
        if error_response:
            return error_response

        return jsonify(device_config_to_dict(shop))
    finally:
        db.close()


@app.put("/shop/<shop_id>/device-config")
@auth_required
def update_device_config(shop_id):
    try:
        payload = parse_device_config_update(request.get_json(silent=True))
    except ValueError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400

    db = SessionLocal()
    try:
        shop, error_response = owned_shop_or_response(db, shop_id)
        if error_response:
            return error_response

        upsert_device_config(db, shop, payload)
        db.commit()
        db.refresh(shop)
        return jsonify(device_config_to_dict(shop))
    except Exception:
        db.rollback()
        raise
    finally:
        db.close()


@app.post("/shop/<shop_id>/media/upload-url")
@auth_required
def media_upload_url(shop_id):
    try:
        payload = parse_media_upload_request(request.get_json(silent=True))
    except ValueError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400

    db = SessionLocal()
    try:
        _, error_response = owned_shop_or_response(db, shop_id)
        if error_response:
            return error_response

        object_key = f"alerts/{shop_id}/{uuid4()}.{payload['extension']}"
        bucket_name = os.environ["R2_BUCKET_NAME"]
        upload_url = r2_client().generate_presigned_url(
            ClientMethod="put_object",
            Params={
                "Bucket": bucket_name,
                "Key": object_key,
                "ContentType": payload["content_type"],
            },
            ExpiresIn=UPLOAD_URL_EXPIRES_SECONDS,
            HttpMethod="PUT",
        )
        public_url = f"{R2_PUBLIC_BASE_URL.rstrip('/')}/{object_key}"
        return jsonify({
            "ok": True,
            "upload_url": upload_url,
            "public_url": public_url,
            "object_key": object_key,
            "content_type": payload["content_type"],
            "expires_in": UPLOAD_URL_EXPIRES_SECONDS,
        })
    except KeyError as exc:
        return jsonify({"ok": False, "error": f"missing environment variable: {exc.args[0]}"}), 500
    finally:
        db.close()


@app.get("/alerts/<shop_id>")
@auth_required
def alerts(shop_id):
    db = SessionLocal()
    try:
        _, error_response = owned_shop_or_response(db, shop_id)
        if error_response:
            return error_response

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
