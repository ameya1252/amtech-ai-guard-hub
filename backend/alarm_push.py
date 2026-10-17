"""ESP32 alarm push outbox. Camera advisories keep their separate cooldown policy.

Alarm delivery is at least once: an ambiguous timeout/process death may cause a
duplicate. Confirmed Expo acceptance is never retried. Tickets are acceptance by
Expo, not proof that the operating system displayed a notification.
"""
import logging
import os
import threading
from datetime import datetime, timedelta, timezone
from uuid import uuid4

from database import AlarmPushDelivery, Alert, PushToken, SessionLocal, Shop

log = logging.getLogger(__name__)
wake_event = threading.Event()
MAX_ATTEMPTS = 8
LEASE_SECONDS = 90  # External request has a 10-second timeout.


def utc(value):
    return value.replace(tzinfo=timezone.utc) if value.tzinfo is None else value


def enqueue_alarm_pushes(db, alert, shop):
    now = datetime.now(timezone.utc)
    for token in db.query(PushToken).filter_by(user_id=shop.user_id).all():
        db.add(AlarmPushDelivery(id=str(uuid4()), alert_id=alert.id,
               token_id=token.id, user_id=shop.user_id, status="pending", attempts=0,
               next_attempt_at=now, expires_at=now + timedelta(hours=1)))


def wake_alarm_pushes():
    wake_event.set()


def deliver_pending_alarm_pushes(send_push, limit=20):
    """Bounded pass, also used by tests. Leases survive restarts and serialize workers."""
    now = datetime.now(timezone.utc)
    with SessionLocal() as db:
        ids = [row.id for row in db.query(AlarmPushDelivery).filter(
            AlarmPushDelivery.status.in_(("pending", "sending")),
            AlarmPushDelivery.next_attempt_at <= now,
        ).order_by(AlarmPushDelivery.next_attempt_at).limit(limit).all()]
    for delivery_id in ids:
        now = datetime.now(timezone.utc)
        lease = str(uuid4())
        with SessionLocal() as db:
            # Atomic compare-and-set works across processes as well as threads.
            claimed = db.query(AlarmPushDelivery).filter(
                AlarmPushDelivery.id == delivery_id,
                AlarmPushDelivery.status.in_(("pending", "sending")),
                AlarmPushDelivery.next_attempt_at <= now,
            ).update({"status": "sending", "lease_id": lease,
                      "next_attempt_at": now + timedelta(seconds=LEASE_SECONDS),
                      "attempts": AlarmPushDelivery.attempts + 1}, synchronize_session=False)
            db.commit()
            if not claimed:
                continue
            row = db.get(AlarmPushDelivery, delivery_id)
            token = db.get(PushToken, row.token_id)
            alert = db.get(Alert, row.alert_id)
            shop = db.get(Shop, alert.shop_id) if alert else None
            if not token or token.user_id != row.user_id or not shop or shop.user_id != row.user_id:
                row.status = "cancelled"
                db.commit()
                continue
            if utc(row.expires_at) <= now or row.attempts > MAX_ATTEMPTS:
                row.status = "failed"
                db.commit()
                log.warning("Alarm push retry window exhausted delivery=%s", delivery_id)
                continue
            label = alert.event_type.replace("-", " ").title()
            message = {"to": token.expo_push_token, "title": f"AMTECH: {label}",
                       "body": f"{label} at {shop.shop_name}. Check the hub's current status.",
                       "sound": "default", "channelId": "alerts",
                       "data": {"shop_id": shop.id, "alert_id": alert.id, "event_type": alert.event_type}}
            token_id, token_value = token.id, token.expo_push_token

        outcome, ticket_id, invalid_token = "pending", None, False
        try:
            result = send_push([message])
            data = result.get("data") if isinstance(result, dict) else None
            ticket = data[0] if isinstance(data, list) and len(data) == 1 else data if isinstance(data, dict) else None
            if isinstance(result, dict) and result.get("simulated"):
                outcome = "simulated"
            elif isinstance(ticket, dict):
                if ticket.get("status") == "ok" and isinstance(ticket.get("id"), str) and ticket["id"]:
                    outcome, ticket_id = "sent", ticket["id"]
                elif ticket.get("status") == "error":
                    details = ticket.get("details")
                    error = details.get("error") if isinstance(details, dict) else None
                    invalid_token = error == "DeviceNotRegistered"
                    if invalid_token or error == "MessageTooBig":
                        outcome = "failed"
        except Exception:
            log.warning("Alarm push attempt failed; queued for retry delivery=%s", delivery_id)

        with SessionLocal() as db:
            row = db.query(AlarmPushDelivery).filter_by(id=delivery_id).with_for_update().one()
            # A late response from an expired lease must not overwrite a newer attempt/logout.
            if row.lease_id != lease or row.status != "sending":
                continue
            row.status, row.ticket_id = outcome, ticket_id
            owner_id = row.user_id
            if outcome == "pending":
                if row.attempts >= MAX_ATTEMPTS:
                    row.status = "failed"
                    log.warning("Alarm push retry limit exhausted delivery=%s", delivery_id)
                else:
                    row.next_attempt_at = datetime.now(timezone.utc) + timedelta(seconds=min(900, 30 * 2 ** (row.attempts - 1)))
            db.commit()
        if invalid_token:
            # Don't hold an outbox lock while deleting a token: logout locks the
            # token before cancelling its jobs. Separate transactions avoid inversion.
            with SessionLocal() as db:
                db.query(PushToken).filter_by(id=token_id, user_id=owner_id,
                                             expo_push_token=token_value).delete()
                db.commit()


def start_alarm_push_worker(send_push):
    if os.getenv("ALARM_PUSH_WORKER_DISABLED", "").lower() in ("1", "true", "yes"):
        return

    def run():
        while True:
            wake_event.clear()
            try:
                deliver_pending_alarm_pushes(send_push)
            except Exception:
                log.exception("Alarm push worker pass failed; durable jobs retained")
            wake_event.wait(15)

    threading.Thread(target=run, daemon=True, name="alarm-push-outbox").start()
