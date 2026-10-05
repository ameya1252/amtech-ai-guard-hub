"""Additive ESP32 API. Alarm policy remains on the hub; RV1106 routes stay compatible."""
import hashlib
import hmac
import re
from datetime import datetime, timedelta, timezone
from uuid import uuid4

from flask import g, jsonify, request
from sqlalchemy.exc import IntegrityError

from database import Alert, Device, Esp32Command, Esp32EventReceipt, Esp32Hub, SessionLocal, Shop

ONLINE_SECONDS = 45
COMMAND_SECONDS = 120
ACTIVE = ("queued", "dispatched")
CAPABILITIES = {
    "arm": True, "force_arm": True, "disarm": True, "stop": True,
    "sensor_status": True, "schedule": False, "editable_contacts": False,
    "camera_slots": 1, "camera_setup": False, "battery": False, "media": False,
    "remote_panic": False,
}
EVENT_TYPES = {"intrusion", "shutter-1", "shutter-2", "panic", "smoke"}
IDENTIFIER = re.compile(r"^[A-Za-z0-9_-]{1,128}$")


def secret_hash(value):
    return hashlib.sha256(value.encode()).hexdigest()


def utc(value):
    return value.replace(tzinfo=timezone.utc) if value and value.tzinfo is None else value


def online(hub):
    return bool(hub.last_seen_at and (datetime.now(timezone.utc) - utc(hub.last_seen_at)).total_seconds() < ONLINE_SECONDS)


def hub_for_shop(db, shop_id, lock=False):
    query = db.query(Esp32Hub).filter(Esp32Hub.shop_id == shop_id)
    return (query.with_for_update() if lock else query).first()


def expire_commands(db, serial):
    now = datetime.now(timezone.utc)
    commands = db.query(Esp32Command).filter(Esp32Command.serial == serial, Esp32Command.status.in_(ACTIVE)).all()
    for command in commands:
        if utc(command.expires_at) <= now:
            command.status = "expired"
            command.reason = "Hub confirmation timed out. Check the reported state before retrying."
            command.completed_at = now
    db.flush()


def command_dict(command):
    if command is None:
        return None
    return {"id": command.id, "name": command.name, "status": command.status,
            "reason": command.reason, "expires_at": utc(command.expires_at).isoformat(),
            "valid_for_ms": max(0, int((utc(command.expires_at) - datetime.now(timezone.utc)).total_seconds() * 1000))}


def status_dict(db, shop, hub):
    expire_commands(db, hub.serial)
    latest = db.query(Esp32Command).filter(Esp32Command.serial == hub.serial).order_by(Esp32Command.created_at.desc()).first()
    pending = latest if latest and latest.status in ACTIVE else None
    state = hub.reported_state or {}
    return {
        "ok": True, "shop_id": shop.id, "device_type": "esp32", "capabilities": CAPABILITIES,
        "armed": state.get("arm_state") == "armed", "arm_state": state.get("arm_state", "unknown"),
        "hub_online": online(hub), "last_seen_at": utc(hub.last_seen_at).isoformat() if hub.last_seen_at else None,
        "battery_level": None, "pending_command": pending.name if pending else None,
        "pending_command_id": pending.id if pending else None,
        "last_command": command_dict(latest), **{k: v for k, v in state.items() if k != "arm_state"},
    }


def esp32_status(shop):
    with SessionLocal() as db:
        hub = hub_for_shop(db, shop.id, lock=True)
        if hub is None:
            return None
        result = status_dict(db, shop, hub)
        db.commit()
        return result


def esp32_metadata(shop_id):
    with SessionLocal() as db:
        hub = hub_for_shop(db, shop_id)
        return {"device_type": "esp32", "capabilities": CAPABILITIES} if hub else {"device_type": "rv1106"}


def queue_command(shop_id, name):
    with SessionLocal() as db:
        hub = hub_for_shop(db, shop_id, lock=True)
        if not hub:
            return jsonify({"ok": False, "error": "This control requires an ESP32 hub"}), 409
        if not online(hub):
            return jsonify({"ok": False, "error": "Hub is offline. Use its existing SMS controls if needed."}), 409
        expire_commands(db, hub.serial)
        active = db.query(Esp32Command).filter(Esp32Command.serial == hub.serial, Esp32Command.status.in_(ACTIVE)).all()
        for command in active:
            if name in ("disarm", "stop") and command.name in ("arm", "arm_force"):
                command.status = "cancelled"
                command.reason = "Superseded by an owner request"
                command.completed_at = datetime.now(timezone.utc)
            else:
                return jsonify({"ok": False, "error": "A command is awaiting hub confirmation"}), 409
        now = datetime.now(timezone.utc)
        db.add(Esp32Command(id=str(uuid4()), serial=hub.serial, name=name, status="queued",
                            created_at=now, expires_at=now + timedelta(seconds=COMMAND_SECONDS)))
        db.flush()
        result = status_dict(db, db.get(Shop, shop_id), hub)
        db.commit()
        return jsonify(result)


def validate_state(state):
    if not isinstance(state, dict) or state.get("arm_state") not in ("disarmed", "arming", "armed"):
        raise ValueError("A valid reported arm_state is required")
    result = {"arm_state": state["arm_state"]}
    for key in ("force_armed", "siren_active", "camera_online", "camera_protection_active"):
        if type(state.get(key)) is not bool:
            raise ValueError(f"{key} must be boolean")
        result[key] = state[key]
    choices = {"siren_owner": ("none", "intrusion", "panic", "smoke", "other"),
               "shutter_1": ("unknown", "secure", "open", "wiring_open", "wiring_short"),
               "shutter_2": ("unknown", "secure", "open", "wiring_open", "wiring_short"),
               "smoke": ("unknown", "normal", "alarm")}
    for key, allowed in choices.items():
        if state.get(key) not in allowed:
            raise ValueError(f"Invalid {key}")
        result[key] = state[key]
    reason = state.get("arm_block_reason")
    if reason is not None and (not isinstance(reason, str) or len(reason) > 512):
        raise ValueError("Invalid arm block reason")
    result["arm_block_reason"] = reason
    return result


def register_esp32_routes(app, auth_required, owned_shop_or_response, send_expo_push_messages):
    @app.post("/esp32/pair")
    @auth_required
    def pair_esp32():
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict):
            return jsonify({"ok": False, "error": "JSON object required"}), 400
        required = ("device_serial", "pairing_code", "shop_name", "owner_name", "address")
        if any(not isinstance(payload.get(k), str) or not payload[k].strip() for k in required):
            return jsonify({"ok": False, "error": "Hub serial, pairing code and shop details are required"}), 400
        if any(len(payload[k]) > limit for k, limit in zip(required, (128, 128, 255, 255, 1024))):
            return jsonify({"ok": False, "error": "Pairing details are too long"}), 400
        with SessionLocal() as db:
            hub = db.query(Esp32Hub).filter(Esp32Hub.serial == payload["device_serial"].strip()).with_for_update().first()
            if hub is None or not hmac.compare_digest(hub.pairing_code_hash, secret_hash(payload["pairing_code"].strip())):
                return jsonify({"ok": False, "error": "Hub serial or pairing code is incorrect"}), 403
            if hub.shop_id:
                shop = db.get(Shop, hub.shop_id)
                if shop.user_id == g.user_id:
                    return jsonify({"ok": True, "shop_id": shop.id, "device_type": "esp32"})
                return jsonify({"ok": False, "error": "This hub is already paired"}), 409
            shop = Shop(id=str(uuid4()), user_id=g.user_id, shop_name=payload["shop_name"].strip(),
                        owner_name=payload["owner_name"].strip(), address=payload["address"].strip(),
                        owner_phone=g.user_phone, owner_email=g.user_email,
                        armed=(hub.reported_state or {}).get("arm_state") == "armed")
            db.add(shop)
            db.flush()
            db.add(Device(id=str(uuid4()), shop_id=shop.id, device_serial=hub.serial,
                          status="online" if online(hub) else "offline", last_seen_at=hub.last_seen_at))
            hub.shop_id = shop.id
            try:
                db.commit()
            except IntegrityError:
                db.rollback()
                return jsonify({"ok": False, "error": "This hub serial is already registered"}), 409
            return jsonify({"ok": True, "shop_id": shop.id, "device_type": "esp32"}), 201

    for path, name in (("arm-force", "arm_force"), ("stop", "stop")):
        def action(shop_id, operation=name):
            with SessionLocal() as db:
                _, error = owned_shop_or_response(db, shop_id)
                if error:
                    return error
            return queue_command(shop_id, operation)
        app.add_url_rule(f"/shop/<shop_id>/{path}", f"esp32_{name}", auth_required(action), methods=["POST"])

    @app.post("/esp32/device/sync")
    def sync_esp32():
        if request.content_length and request.content_length > 65536:
            return jsonify({"ok": False, "error": "Device sync payload is too large"}), 413
        serial = request.headers.get("X-AMTECH-DEVICE-SERIAL", "")
        token = request.headers.get("X-AMTECH-DEVICE-TOKEN", "")
        if not IDENTIFIER.fullmatch(serial) or not token or len(token) > 256:
            return jsonify({"ok": False, "error": "Device authentication required"}), 401
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict):
            return jsonify({"ok": False, "error": "JSON object required"}), 400
        try:
            state = validate_state(payload.get("state"))
            ack = payload.get("ack")
            if ack is not None and (not isinstance(ack, dict) or ack.get("status") not in ("applied", "rejected")
                                    or not isinstance(ack.get("id"), str) or len(ack["id"]) > 128):
                raise ValueError("Invalid command acknowledgement")
            events = payload.get("events", [])
            if not isinstance(events, list) or len(events) > 32:
                raise ValueError("At most 32 events may be sent")
            for event in events:
                if (not isinstance(event, dict) or not isinstance(event.get("id"), str)
                        or not IDENTIFIER.fullmatch(event["id"]) or not isinstance(event.get("event_type"), str)
                        or event["event_type"] not in EVENT_TYPES):
                    raise ValueError("Invalid alert event")
                if event.get("timestamp"):
                    if not isinstance(event["timestamp"], str):
                        raise ValueError("Invalid alert timestamp")
                    stamp = datetime.fromisoformat(event["timestamp"].replace("Z", "+00:00"))
                    if stamp.tzinfo is None:
                        raise ValueError("Alert timestamp must include a timezone")
        except ValueError as exc:
            return jsonify({"ok": False, "error": str(exc)}), 400
        messages = []
        with SessionLocal() as db:
            hub = db.query(Esp32Hub).filter(Esp32Hub.serial == serial).with_for_update().first()
            if not hub or not hmac.compare_digest(hub.token_hash, secret_hash(token)):
                return jsonify({"ok": False, "error": "Invalid device credentials"}), 401
            hub.last_seen_at = datetime.now(timezone.utc)
            hub.reported_state = state
            ack_received = None
            if ack:
                command = db.get(Esp32Command, ack["id"])
                if command and command.serial == serial:
                    if command.status in ACTIVE:
                        if ack["status"] == "applied" and ((command.name in ("arm", "arm_force") and ack.get("arm_state") != "armed")
                                                             or (command.name == "disarm" and ack.get("arm_state") != "disarmed")):
                            return jsonify({"ok": False, "error": "Acknowledgement contradicts reported arm state"}), 400
                        command.status = ack["status"]
                        command.reason = str(ack.get("reason") or "")[:512] or None
                        command.completed_at = datetime.now(timezone.utc)
                    ack_received = command.id
            accepted = []
            if hub.shop_id:
                shop = db.get(Shop, hub.shop_id)
                shop.armed = state["arm_state"] == "armed"
                device = db.query(Device).filter(Device.shop_id == shop.id).first()
                device.last_seen_at, device.status = hub.last_seen_at, "online"
                for event in events:
                    receipt_id = f"{serial}:{event['id']}"
                    if db.get(Esp32EventReceipt, receipt_id) is None:
                        timestamp = datetime.fromisoformat(event["timestamp"].replace("Z", "+00:00")) if event.get("timestamp") else hub.last_seen_at
                        alert = Alert(id=str(uuid4()), shop_id=shop.id, event_type=event["event_type"], timestamp=timestamp)
                        db.add(alert)
                        db.flush()
                        db.add(Esp32EventReceipt(id=receipt_id, alert_id=alert.id))
                        db.flush()
                        from database import PushToken
                        tokens = db.query(PushToken).filter(PushToken.user_id == shop.user_id).all()
                        label = event["event_type"].replace("-", " ").title()
                        messages.extend({"to": row.expo_push_token, "title": f"AMTECH: {label}",
                                         "body": f"{label} at {shop.shop_name}. Check the hub's current status.",
                                         "sound": "default", "channelId": "alerts",
                                         "data": {"shop_id": shop.id, "alert_id": alert.id, "event_type": event["event_type"]}}
                                        for row in tokens)
                    accepted.append(event["id"])
                expire_commands(db, serial)
                pending = db.query(Esp32Command).filter(Esp32Command.serial == serial, Esp32Command.status.in_(ACTIVE)).first()
                if pending:
                    pending.status = "dispatched"
            else:
                pending = None
            result = {"ok": True, "paired": bool(hub.shop_id), "command": command_dict(pending),
                      "ack_received": ack_received, "accepted_event_ids": accepted}
            db.commit()
        if messages:
            try:
                send_expo_push_messages(messages)
            except Exception:
                app.logger.warning("ESP32 push delivery failed; alert history has been retained")
        return jsonify(result)
