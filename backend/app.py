import os
from datetime import datetime, timezone
from functools import wraps
from uuid import uuid4

import bcrypt
import jwt
import requests
from flask import Flask, g, jsonify, request
from sqlalchemy.exc import IntegrityError

from database import Alert, Device, Shop, SessionLocal, User, init_db


app = Flask(__name__)
JWT_SECRET = os.environ["JWT_SECRET"]
init_db()


def env_bool(name, default=False):
    value = os.getenv(name)
    if value is None:
        return default
    return value.strip().lower() in ("1", "true", "yes", "on")


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
        finally:
            db.close()

        return view(*args, **kwargs)

    return wrapped


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


def parse_shop_registration(payload):
    if not isinstance(payload, dict):
        raise ValueError("request body must be a JSON object")

    shop_name = payload.get("shop_name")
    owner_phone = payload.get("owner_phone")
    owner_email = payload.get("owner_email")
    device_serial = payload.get("device_serial")

    if not shop_name:
        raise ValueError("shop_name is required")
    if not owner_phone:
        raise ValueError("owner_phone is required")
    if not device_serial:
        raise ValueError("device_serial is required")

    return {
        "shop_name": str(shop_name),
        "owner_phone": str(owner_phone),
        "owner_email": str(owner_email) if owner_email else None,
        "device_serial": str(device_serial),
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
            whatsapp_sent=False,
        )
        db.add(alert_row)
        db.commit()
        db.refresh(alert_row)
        return alert_row
    except Exception:
        db.rollback()
        raise
    finally:
        db.close()


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
        "whatsapp_sent": alert_row.whatsapp_sent,
    }


def shop_status_to_dict(shop):
    return {
        "ok": True,
        "shop_id": shop.id,
        "armed": bool(shop.armed),
        "hub_online": True,
        "battery_level": 100,
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
        "owner_phone": shop.owner_phone,
        "device_serial": device.device_serial if device else None,
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
        "owner_phone": shop.owner_phone,
        "device_serial": device.device_serial if device else None,
        "armed": bool(shop.armed),
    }


@app.post("/auth/signup")
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
        provider_response = send_whatsapp_alert(alert_payload)
        whatsapp_sent = not bool(provider_response.get("simulated"))
        if alert_row.whatsapp_sent != whatsapp_sent:
            db = SessionLocal()
            try:
                stored_alert = db.get(Alert, alert_row.id)
                stored_alert.whatsapp_sent = whatsapp_sent
                db.commit()
                db.refresh(stored_alert)
                alert_row = stored_alert
            finally:
                db.close()
    except ValueError as exc:
        return jsonify({"ok": False, "error": str(exc)}), 400
    except KeyError as exc:
        return jsonify({"ok": False, "error": f"missing environment variable: {exc.args[0]}"}), 500
    except requests.RequestException as exc:
        return jsonify({"ok": False, "error": f"whatsapp api call failed: {exc}"}), 502

    return jsonify({"ok": True, "alert": alert_to_dict(alert_row), "provider_response": provider_response})


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
            owner_phone=payload["owner_phone"],
            owner_email=payload["owner_email"],
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
