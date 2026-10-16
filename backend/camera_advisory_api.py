"""App-only Camera 1 advisory. Never queues alarm commands, SMS or voice calls.

Durable reservation precedes Expo dispatch. Uncertain outcomes are suppressed,
not blindly resent. A ticket confirms Expo acceptance, not phone delivery.
"""
import hmac
from datetime import datetime, timedelta, timezone
from uuid import uuid4

from flask import jsonify, request
from sqlalchemy import or_
from database import Alert, Esp32CameraAdvisoryReceipt, Esp32Hub, PushToken, SessionLocal, Shop
from esp32_api import IDENTIFIER, secret_hash, utc

COOLDOWN = timedelta(minutes=30)
RETRY = timedelta(seconds=30)
MAX_ATTEMPTS = 3


def register_camera_advisory_routes(app, send_push):
    @app.post("/esp32/device/camera-advisory")
    def camera_advisory():
        serial = request.headers.get("X-AMTECH-DEVICE-SERIAL", "")
        token = request.headers.get("X-AMTECH-DEVICE-TOKEN", "")
        if not IDENTIFIER.fullmatch(serial) or not token or len(token) > 256:
            return jsonify(ok=False, error="Device authentication required"), 401
        if request.content_length and request.content_length > 2048:
            return jsonify(ok=False, error="Advisory payload too large"), 413
        payload = request.get_json(silent=True)
        if (not isinstance(payload, dict) or not isinstance(payload.get("event_id"), str)
                or not IDENTIFIER.fullmatch(payload["event_id"])
                or type(payload.get("camera_id")) is not int or payload["camera_id"] != 1):
            return jsonify(ok=False, error="A valid event ID and Camera 1 are required"), 400
        event_id, camera_id = payload["event_id"], payload["camera_id"]
        receipt_id = f"{serial}:{event_id}"
        now = datetime.now(timezone.utc)

        def reply(status, until=None):
            remaining = max(0, min(1800000, int((until - datetime.now(timezone.utc)).total_seconds() * 1000))) if until else 0
            return jsonify(ok=True, event_id=event_id, status=status, cooldown_ms=remaining)

        with SessionLocal() as db:
            # Shared hub lock also serializes concurrent advisory submissions.
            hub = db.query(Esp32Hub).filter(Esp32Hub.serial == serial).with_for_update().first()
            if not hub or not hmac.compare_digest(hub.token_hash, secret_hash(token)):
                return jsonify(ok=False, error="Invalid device credentials"), 401
            if not hub.shop_id:
                return reply("failed")
            receipt = db.get(Esp32CameraAdvisoryReceipt, receipt_id)
            if receipt:
                if receipt.camera_id != camera_id:
                    return jsonify(ok=False, error="Event identity mismatch"), 409
                if receipt.status == "accepted":
                    return reply("accepted", utc(receipt.accepted_at) + COOLDOWN)
                if receipt.status in ("uncertain", "suppressed"):
                    return reply(receipt.status, utc(receipt.cooldown_until))
                if receipt.status == "failed":
                    return reply("failed")
                if receipt.status == "in_flight":
                    if now - utc(receipt.attempted_at) < timedelta(seconds=20):
                        return reply("retry")
                    # Process death/timeout after submission: delivery is unknown.
                    receipt.status = "uncertain"
                    receipt.cooldown_until = utc(receipt.attempted_at) + COOLDOWN
                    db.commit()
                    return reply("uncertain", utc(receipt.attempted_at) + COOLDOWN)
                if receipt.attempts >= MAX_ATTEMPTS or now - utc(receipt.created_at) >= timedelta(seconds=90):
                    receipt.status = "failed"
                    db.commit()
                    return reply("failed")
                if now - utc(receipt.attempted_at) < RETRY:
                    return reply("retry")
            else:
                recent = db.query(Esp32CameraAdvisoryReceipt).filter(
                    Esp32CameraAdvisoryReceipt.serial == serial,
                    Esp32CameraAdvisoryReceipt.camera_id == camera_id,
                    or_(Esp32CameraAdvisoryReceipt.attempted_at > now - COOLDOWN,
                        Esp32CameraAdvisoryReceipt.cooldown_until > now),
                ).all()
                guard_until = max((
                    utc(row.cooldown_until) if row.cooldown_until else utc(row.attempted_at) + COOLDOWN
                    for row in recent if row.status in ("accepted", "uncertain", "in_flight")
                ), default=None)
                if guard_until and now < guard_until:
                    # Remember suppression permanently for this episode: no delayed replay.
                    db.add(Esp32CameraAdvisoryReceipt(id=receipt_id, serial=serial, camera_id=camera_id,
                           status="suppressed", attempts=0, attempted_at=now, cooldown_until=guard_until))
                    db.commit()
                    return reply("suppressed", guard_until)
                # Failure quota is separate from the successful-send cooldown.
                failures = [row for row in recent if row.status in ("failed", "retry")]
                exhausted = sum(max(1, row.attempts) for row in failures) >= MAX_ATTEMPTS
                if exhausted or any(now - utc(row.attempted_at) < RETRY for row in failures):
                    failure_until = max((utc(row.attempted_at) + COOLDOWN for row in failures), default=now + RETRY) if exhausted else now + RETRY
                    db.add(Esp32CameraAdvisoryReceipt(id=receipt_id, serial=serial, camera_id=camera_id,
                           status="suppressed", attempts=0, attempted_at=now, cooldown_until=failure_until))
                    db.commit()
                    return reply("suppressed", failure_until)
                receipt = Esp32CameraAdvisoryReceipt(id=receipt_id, serial=serial, camera_id=camera_id,
                            status="retry", attempts=0, attempted_at=now)
                db.add(receipt)

            shop = db.get(Shop, hub.shop_id)
            if not receipt.alert_id:
                alert = Alert(id=str(uuid4()), shop_id=shop.id,
                              event_type=f"camera-interference-{camera_id}", timestamp=now)
                db.add(alert)
                db.flush()
                receipt.alert_id = alert.id
            tokens = db.query(PushToken).filter(PushToken.user_id == shop.user_id).all()
            if not tokens:
                receipt.status = "failed"
                receipt.attempted_at = now
                db.commit()
                app.logger.info("Camera advisory retained in history; no owner push tokens camera=%s", camera_id)
                return reply("failed")
            alert_id = receipt.alert_id
            messages = [{"to": row.expo_push_token, "title": f"Nettra: Camera {camera_id} advisory",
                         "body": "Possible camera interference detected. Please check your camera's live view in its camera app.",
                         "channelId": "camera-advisories", "sound": "default",
                         "data": {"shop_id": shop.id, "alert_id": alert_id,
                                  "event_type": f"camera-interference-{camera_id}", "camera_id": camera_id}}
                        for row in tokens]
            receipt.attempts += 1
            receipt.attempted_at = now
            receipt.status = "in_flight"
            db.commit()  # Crash-safe boundary BEFORE external side effects.

        status, tickets = "uncertain", []
        try:
            result = send_push(messages)
            data = result.get("data") if isinstance(result, dict) else None
            if isinstance(data, dict):
                data = [data]
            if isinstance(data, list) and len(data) == len(messages):
                tickets = [row["id"] for row in data if isinstance(row, dict)
                           and row.get("status") == "ok" and isinstance(row.get("id"), str) and row["id"]]
                if tickets:
                    status = "accepted"
                elif all(isinstance(row, dict) and row.get("status") == "error" for row in data):
                    status = "retry"  # Known rejection: bounded retry is safe.
            if isinstance(result, dict) and result.get("simulated"):
                status = "failed"  # Simulation must never count as real acceptance.
        except Exception:
            # Could have been accepted before a network/process fault. Do not resend.
            app.logger.warning("Camera advisory dispatch uncertain camera=%s event=%s", camera_id, event_id)

        completed = datetime.now(timezone.utc)
        with SessionLocal() as db:
            receipt = db.query(Esp32CameraAdvisoryReceipt).filter_by(id=receipt_id).with_for_update().one()
            receipt.status = status
            receipt.ticket_ids = tickets or None
            if status == "accepted":
                receipt.accepted_at = completed
                receipt.cooldown_until = completed + COOLDOWN
            elif status == "uncertain":
                receipt.cooldown_until = now + COOLDOWN
            elif status == "retry" and receipt.attempts >= MAX_ATTEMPTS:
                receipt.status = status = "failed"
            db.commit()
        app.logger.info("Camera advisory push result camera=%s event=%s status=%s", camera_id, event_id, status)
        return reply(status, completed + COOLDOWN if status == "accepted" else now + COOLDOWN if status == "uncertain" else None)
