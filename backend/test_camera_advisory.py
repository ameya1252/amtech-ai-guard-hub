"""Isolated database + fake Expo tickets; no real pushes or hardware requests."""
import unittest
from datetime import datetime, timedelta, timezone
from unittest.mock import patch

import test_esp32 as fixture
STATE = fixture.STATE
from database import SessionLocal, Esp32CameraAdvisoryReceipt, Esp32Command, Alert, PushToken, Shop


class CameraAdvisoryTests(unittest.TestCase):
    setUp = fixture.Esp32Tests.setUp
    sync = fixture.Esp32Tests.sync
    status = fixture.Esp32Tests.status

    def tokens(self):
        with SessionLocal() as db:
            owner = db.get(Shop, self.shop_id).user_id
            db.add(PushToken(id="advisory-token", user_id=owner, expo_push_token="ExpoPushToken[preview-only]"))
            db.commit()

    def send(self, event="event-one", headers=None, camera=1):
        return self.client.post('/esp32/device/camera-advisory', headers=headers or self.device,
                                json={"event_id": event, "camera_id": camera})

    def age(self, seconds):
        with SessionLocal() as db:
            row = db.get(Esp32CameraAdvisoryReceipt, "ESP32-TEST:event-one")
            row.attempted_at -= timedelta(seconds=seconds)
            db.commit()

    def test_accepted_push_is_idempotent_and_never_changes_alarm_state(self):
        self.tokens()
        before = self.status()
        with patch('app.send_expo_push_messages', return_value={"data": [{"status": "ok", "id": "ticket-one"}]}) as push:
            first = self.send()
            self.assertEqual(first.json['status'], 'accepted')
            self.assertGreater(first.json['cooldown_ms'], 1790000)
            self.assertEqual(self.send().json['status'], 'accepted')
            self.assertEqual(push.call_count, 1)
            message = push.call_args.args[0][0]
            self.assertEqual(message['channelId'], 'camera-advisories')
            self.assertIn('Possible camera interference', message['body'])
            self.assertEqual(message['data']['camera_id'], 1)
        after = self.status()
        for key in ('arm_state', 'armed', 'force_armed', 'siren_active', 'pending_command'):
            self.assertEqual(before[key], after[key])
        with SessionLocal() as db:
            self.assertEqual(db.query(Esp32Command).count(), 0)
            self.assertEqual(db.query(Alert).one().event_type, 'camera-interference-1')

    def test_inside_cooldown_is_suppressed_without_extension_or_later_replay(self):
        self.tokens()
        with patch('app.send_expo_push_messages', return_value={"data": [{"status": "ok", "id": "ticket-one"}]}) as push:
            self.send()
            with SessionLocal() as db:
                row = db.get(Esp32CameraAdvisoryReceipt, 'ESP32-TEST:event-one')
                row.accepted_at -= timedelta(minutes=10)
                row.cooldown_until -= timedelta(minutes=10)
                db.commit()
            response = self.send('suppressed-episode')
            self.assertEqual(response.json['status'], 'suppressed')
            self.assertLess(response.json['cooldown_ms'], 1201000)
            self.assertLess(self.send('suppressed-episode').json['cooldown_ms'], 1201000)
            self.assertEqual(push.call_count, 1)
            with SessionLocal() as db:
                for row in db.query(Esp32CameraAdvisoryReceipt).all():
                    row.attempted_at -= timedelta(hours=1)
                    if row.accepted_at: row.accepted_at -= timedelta(hours=1)
                    if row.cooldown_until: row.cooldown_until -= timedelta(hours=1)
                db.commit()
            self.assertEqual(self.send('suppressed-episode').json['status'], 'suppressed')
            self.assertEqual(push.call_count, 1)
            self.assertEqual(self.send('fresh-post-expiry').json['status'], 'accepted')
            self.assertEqual(push.call_count, 2)

    def test_rejection_retries_are_bounded_and_not_counted_as_success(self):
        self.tokens()
        with patch('app.send_expo_push_messages', return_value={"data": [{"status": "error", "details": {"error": "MessageRateExceeded"}}]}) as push:
            self.assertEqual(self.send().json['status'], 'retry')
            self.assertEqual(self.send().json['status'], 'retry')
            self.assertEqual(push.call_count, 1)
            self.age(31)
            self.assertEqual(self.send().json['status'], 'retry')
            self.age(31)
            self.assertEqual(self.send().json['status'], 'failed')
            self.assertEqual(self.send().json['status'], 'failed')
            self.assertEqual(push.call_count, 3)
        with SessionLocal() as db:
            row = db.get(Esp32CameraAdvisoryReceipt, 'ESP32-TEST:event-one')
            self.assertIsNone(row.accepted_at)
            self.assertEqual(db.query(Alert).count(), 1)

    def test_missing_token_retains_history_without_false_acceptance(self):
        with patch('app.send_expo_push_messages') as push:
            self.assertEqual(self.send().json['status'], 'failed')
            self.assertEqual(self.send().json['status'], 'failed')
            push.assert_not_called()
        with SessionLocal() as db:
            self.assertEqual(db.query(Alert).one().event_type, 'camera-interference-1')

    def test_timeout_after_possible_acceptance_does_not_duplicate_push(self):
        self.tokens()
        with patch('app.send_expo_push_messages', side_effect=TimeoutError) as push:
            self.assertEqual(self.send().json['status'], 'uncertain')
            self.assertEqual(self.send().json['status'], 'uncertain')
            self.assertEqual(self.send('another-event').json['status'], 'suppressed')
            self.assertEqual(push.call_count, 1)

    def test_durable_inflight_reservation_survives_backend_restart_gap(self):
        self.tokens()
        with SessionLocal() as db:
            db.add(Esp32CameraAdvisoryReceipt(id='ESP32-TEST:event-one', serial='ESP32-TEST', camera_id=1,
                   status='in_flight', attempts=1, attempted_at=datetime.now(timezone.utc)-timedelta(seconds=25)))
            db.commit()
        with patch('app.send_expo_push_messages') as push:
            self.assertEqual(self.send().json['status'], 'uncertain')
            push.assert_not_called()

    def test_advisory_works_armed_force_armed_disarmed_and_authentication_is_required(self):
        self.tokens()
        with patch('app.send_expo_push_messages', return_value={"data": [{"status": "ok", "id": "ticket"}]}):
            for index, state in enumerate((STATE, {**STATE, 'arm_state': 'armed'}, {**STATE, 'arm_state': 'armed', 'force_armed': True})):
                with SessionLocal() as db:
                    for row in db.query(Esp32CameraAdvisoryReceipt).all():
                        row.attempted_at -= timedelta(hours=1)
                        if row.cooldown_until: row.cooldown_until -= timedelta(hours=1)
                        if row.accepted_at: row.accepted_at -= timedelta(hours=1)
                    db.commit()
                self.sync(state)
                self.assertEqual(self.send(f'mode-{index}').json['status'], 'accepted')
                self.assertEqual(self.status()['arm_state'], state['arm_state'])
        self.assertEqual(self.send(headers={**self.device, 'X-AMTECH-DEVICE-TOKEN': 'wrong'}).status_code, 401)
        self.assertEqual(self.send(camera=2).status_code, 400)  # Only Camera 1 is implemented.
        self.assertEqual(self.send(camera=True).status_code, 400)
        self.assertEqual(self.send(event='../bad').status_code, 400)

    def test_simulation_and_malformed_tickets_never_claim_real_acceptance(self):
        self.tokens()
        with patch('app.send_expo_push_messages', return_value={"simulated": True}):
            self.assertEqual(self.send().json['status'], 'failed')
        self.age(31)
        with patch('app.send_expo_push_messages', return_value={"data": [{"status": "ok"}]}):
            self.assertEqual(self.send('malformed-ticket').json['status'], 'uncertain')


if __name__ == '__main__':
    unittest.main(verbosity=2)
