import os
import threading
import time
from datetime import datetime, timezone
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

from database import Alert, Camera, CameraInventory, Device, PushToken, Shop, SessionLocal, User, init_db


app = Flask(__name__)
JWT_SECRET = os.environ["JWT_SECRET"]
R2_PUBLIC_BASE_URL = os.getenv("R2_PUBLIC_BASE_URL", "https://pub-9585184cf02549f0a6e3e31090670c37.r2.dev")
UPLOAD_URL_EXPIRES_SECONDS = 900
DATABASE_KEEPALIVE_INTERVAL_SECONDS = int(os.getenv("DATABASE_KEEPALIVE_INTERVAL_SECONDS", "240"))
EXPO_PUSH_SEND_URL = "https://exp.host/--/api/v2/push/send"
PUSH_ALERT_EVENT_TYPES = {"intrusion", "intrusion-front", "intrusion-parking"}


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
