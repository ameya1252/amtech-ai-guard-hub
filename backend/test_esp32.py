import os
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from unittest.mock import patch

database_file = tempfile.NamedTemporaryFile(suffix=".sqlite", delete=False)
database_file.close()
os.environ.update(DATABASE_URL=f"sqlite:///{database_file.name}", JWT_SECRET="esp32-test-secret-32-bytes-minimum",
                  DEVICE_CONFIG_SYNC_TOKEN="legacy-test-token", DATABASE_KEEPALIVE_DISABLED="1", SIMULATE_SMS="1", SIMULATE_PUSH="1")
from app import app, limiter
from database import Base, engine, SessionLocal, Esp32Hub, Esp32Command, Esp32EventReceipt, Shop, Alert
from esp32_api import secret_hash

STATE = dict(arm_state="disarmed", force_armed=False, siren_active=False, siren_owner="none",
             shutter_1="secure", shutter_2="secure", smoke="normal", camera_online=True, camera_protection_active=False)


class Esp32Tests(unittest.TestCase):
    def setUp(self):
        limiter.reset()
        with engine.begin() as connection:
            for table in reversed(Base.metadata.sorted_tables):
                connection.execute(table.delete())
        self.client = app.test_client()
        response = self.client.post('/auth/signup', json=dict(email="owner@example.com", password="correct-horse-battery-staple", phone_number="+918550991121"))
        self.auth = {"Authorization": f"Bearer {response.json['token']}"}
        with SessionLocal() as db:
            db.add(Esp32Hub(serial="ESP32-TEST", token_hash=secret_hash("device-secret"), pairing_code_hash=secret_hash("claim-secret")))
            db.commit()
        self.device = {"X-AMTECH-DEVICE-SERIAL": "ESP32-TEST", "X-AMTECH-DEVICE-TOKEN": "device-secret"}
        self.pair_data = dict(device_serial="ESP32-TEST", pairing_code="claim-secret", shop_name="ESP Shop", owner_name="Owner", address="Address")
        self.shop_id = self.client.post('/esp32/pair', headers=self.auth, json=self.pair_data).json['shop_id']
        self.sync()

    def sync(self, state=None, **kwargs):
        if kwargs.get('ack', {}).get('status') == 'applied':
            kwargs['ack'].setdefault('arm_state', (state or STATE)['arm_state'])
        return self.client.post('/esp32/device/sync', headers=self.device, json=dict(state=state or STATE, **kwargs))

    def status(self):
        return self.client.get(f'/shop/{self.shop_id}/status', headers=self.auth).json

    def command(self, name):
        return self.client.post(f'/shop/{self.shop_id}/{name}', headers=self.auth)

    def test_pairing_and_device_scope(self):
        self.assertEqual(self.client.post('/esp32/pair', headers=self.auth, json={**self.pair_data, 'pairing_code': 'wrong'}).status_code, 403)
        self.assertEqual(self.client.post('/shop', headers=self.auth, json=self.pair_data).status_code, 409)
        self.assertEqual(self.client.post('/esp32/pair', headers=self.auth, json=self.pair_data).json['shop_id'], self.shop_id)
        self.assertEqual(self.client.post('/esp32/device/sync', headers={'X-AMTECH-DEVICE-SERIAL': 'ESP32-TEST', 'X-AMTECH-DEVICE-TOKEN': 'wrong'}, json={'state': STATE}).status_code, 401)
        self.assertEqual(self.client.post('/esp32/device/sync', headers={'X-AMTECH-DEVICE-CONFIG-TOKEN': 'legacy-test-token'}, json={'state': STATE}).status_code, 401)
        self.assertEqual(self.command('arm-force').status_code, 200)
        self.assertEqual(self.client.post(f'/shop/{self.shop_id}/stop').status_code, 401)

    def test_queued_arm_does_not_claim_success_and_rejection_is_visible(self):
        response = self.command('arm')
        self.assertFalse(response.json['armed'])
        command = self.sync().json['command']
        response = self.sync(ack=dict(id=command['id'], status='rejected', reason='Shutter 1 is open'))
        self.assertEqual(response.json['ack_received'], command['id'])
        status = self.status()
        self.assertFalse(status['armed'])
        self.assertIsNone(status['pending_command'])
        self.assertEqual(status['last_command']['reason'], 'Shutter 1 is open')

    def test_hardware_confirmation_and_sms_state(self):
        self.command('arm')
        command = self.sync().json['command']
        bad = self.sync(ack=dict(id=command['id'], status='applied'))
        self.assertEqual(bad.status_code, 400)
        armed = {**STATE, 'arm_state': 'armed', 'camera_protection_active': True}
        self.assertEqual(self.sync(armed, ack=dict(id=command['id'], status='applied')).status_code, 200)
        self.assertTrue(self.status()['armed'])
        self.sync()  # Physical/SMS disarm is authoritative without an app command.
        self.assertFalse(self.status()['armed'])

    def test_stop_follows_reported_alarm_specific_state(self):
        armed = {**STATE, 'arm_state': 'armed', 'siren_active': True, 'siren_owner': 'smoke'}
        self.sync(armed)
        self.command('stop')
        command = self.sync(armed).json['command']
        self.assertEqual(command['name'], 'stop')
        self.sync({**armed, 'siren_active': False}, ack=dict(id=command['id'], status='applied'))
        self.assertTrue(self.status()['armed'])
        self.assertFalse(self.status()['siren_active'])
        # Intrusion STOP deliberately disarms in the unchanged ESP32 core.
        self.sync({**armed, 'siren_owner': 'intrusion'})
        self.command('stop')
        command = self.sync({**armed, 'siren_owner': 'intrusion'}).json['command']
        self.sync(STATE, ack=dict(id=command['id'], status='applied'))
        self.assertFalse(self.status()['armed'])
        self.assertEqual(self.status()['last_command']['status'], 'applied')

    def test_preemption_duplicates_and_expiry(self):
        arm = self.command('arm').json['pending_command_id']
        self.assertEqual(self.command('arm-force').status_code, 409)
        disarm = self.command('disarm').json['pending_command_id']
        self.assertNotEqual(arm, disarm)
        self.sync(ack=dict(id=arm, status='rejected', reason='Cancelled'))
        self.assertEqual(self.status()['pending_command_id'], disarm)
        with SessionLocal() as db:
            db.get(Esp32Command, disarm).expires_at = datetime.now(timezone.utc) - timedelta(seconds=1)
            db.commit()
        self.assertIsNone(self.sync().json['command'])
        self.assertEqual(self.status()['last_command']['status'], 'expired')

    def test_offline_and_unsupported_configuration(self):
        with SessionLocal() as db:
            db.get(Esp32Hub, 'ESP32-TEST').last_seen_at = datetime.now(timezone.utc) - timedelta(seconds=60)
            db.commit()
        self.assertFalse(self.status()['hub_online'])
        self.assertIsNone(self.status()['battery_level'])
        self.assertEqual(self.command('arm').status_code, 409)
        self.assertEqual(self.client.get(f'/shop/{self.shop_id}/device-config', headers=self.auth).status_code, 409)
        self.assertEqual(self.client.post('/alert', json={'shop_id': self.shop_id, 'event_type': 'smoke'}).status_code, 403)

    def test_offline_alert_retry_is_idempotent_and_pushes_smoke(self):
        event = dict(id='boot1_event1', event_type='smoke', timestamp='2026-10-04T12:00:00Z')
        with SessionLocal() as db:
            from database import PushToken
            owner = db.get(Shop, self.shop_id).user_id
            db.add(PushToken(id='push1', user_id=owner, expo_push_token='ExpoPushToken[test]', platform='android'))
            db.commit()
        with patch('app.send_expo_push_messages') as mock:
            self.assertEqual(self.sync(events=[event, event]).json['accepted_event_ids'], [event['id'], event['id']])
            self.sync(events=[event])
            self.assertEqual(mock.call_count, 1)
            self.assertEqual(mock.call_args[0][0][0]['title'], 'AMTECH: Smoke')
        with SessionLocal() as db:
            self.assertEqual(db.query(Alert).filter(Alert.shop_id == self.shop_id).count(), 1)
            self.assertEqual(db.query(Esp32EventReceipt).count(), 1)

    def test_other_owner_and_cross_device_ack_are_isolated(self):
        other = self.client.post('/auth/signup', json=dict(email="other@example.com", password="correct-horse-battery-staple", phone_number="+918550991122"))
        other_auth = {"Authorization": f"Bearer {other.json['token']}"}
        self.assertEqual(self.client.post('/esp32/pair', headers=other_auth, json=self.pair_data).status_code, 409)
        self.assertIn(self.client.post(f'/shop/{self.shop_id}/stop', headers=other_auth).status_code, (403, 404))
        command_id = self.command('arm').json['pending_command_id']
        with SessionLocal() as db:
            db.add(Esp32Hub(serial='ESP32-OTHER', token_hash=secret_hash('other-secret'), pairing_code_hash=secret_hash('other-code')))
            db.commit()
        response = self.client.post('/esp32/device/sync', headers={'X-AMTECH-DEVICE-SERIAL': 'ESP32-OTHER', 'X-AMTECH-DEVICE-TOKEN': 'other-secret'}, json={'state': STATE, 'ack': {'id': command_id, 'status': 'rejected'}})
        self.assertIsNone(response.json['ack_received'])
        self.assertEqual(self.status()['pending_command_id'], command_id)

    def test_malformed_events_and_cors(self):
        for event in ({'id': 123, 'event_type': 'smoke'}, {'id': 'valid', 'event_type': []}, {'id': 'valid', 'event_type': 'smoke', 'timestamp': 123}):
            self.assertEqual(self.sync(events=[event]).status_code, 400)
        allowed = self.client.options('/esp32/pair', headers={'Origin': 'http://localhost:4173'})
        self.assertEqual(allowed.headers.get('Access-Control-Allow-Origin'), 'http://localhost:4173')
        denied = self.client.options('/esp32/pair', headers={'Origin': 'https://untrusted.example'})
        self.assertIsNone(denied.headers.get('Access-Control-Allow-Origin'))

    def test_legacy_rv1106_commands_remain_compatible(self):
        legacy = self.client.post('/shop', headers=self.auth, json={**self.pair_data, 'device_serial': 'RV-TEST'}).json['shop_id']
        command = self.client.post(f'/shop/{legacy}/arm', headers=self.auth)
        self.assertTrue(command.json['armed'])
        self.assertEqual(command.json['pending_command'], 'arm')
        ack = self.client.post(f'/shop/{legacy}/pending-command/ack', headers={'X-AMTECH-DEVICE-CONFIG-TOKEN': 'legacy-test-token'}, json={'pending_command_id': command.json['pending_command_id']})
        self.assertIsNone(ack.json['pending_command'])


if __name__ == '__main__':
    try:
        unittest.main()
    finally:
        os.unlink(database_file.name)
