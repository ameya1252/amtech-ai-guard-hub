import copy
import os
import unittest
from datetime import datetime, timedelta, timezone
from unittest.mock import patch

from cryptography import x509
from cryptography.fernet import Fernet, InvalidToken
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID

import test_esp32 as fixture
from database import Alert, Esp32CameraAdvisoryReceipt, Esp32CameraConfig, Esp32Hub, PushToken, SessionLocal


def multicamera_state(enabled=False):
    return dict(fixture.STATE, camera_protocol_version=1, camera_capacity=2,
                camera_config_revision='factory', camera_config_staged_revision='factory',
                cameras=[dict(camera_id=1, enabled=True, online=True, ready=True, protected=False, human_active=False),
                         dict(camera_id=2, enabled=enabled, online=enabled, ready=enabled, protected=False, human_active=False)])


class MultiCameraTests(unittest.TestCase):
    sync = fixture.Esp32Tests.sync
    status = fixture.Esp32Tests.status

    @classmethod
    def setUpClass(cls):
        key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'Camera fixture')])
        now = datetime.now(timezone.utc)
        cls.pem = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
                   .serial_number(x509.random_serial_number()).not_valid_before(now-timedelta(days=1))
                   .not_valid_after(now+timedelta(days=365)).sign(key, hashes.SHA256())
                   .public_bytes(serialization.Encoding.PEM).decode())

    def setUp(self):
        os.environ['AMTECH_LABEL_ENCRYPTION_KEY'] = Fernet.generate_key().decode()
        fixture.Esp32Tests.setUp(self)
        self.sync(multicamera_state())
        self.url = f'/shop/{self.shop_id}/esp32/camera-2'
        self.input = dict(enabled=True, host='192.168.0.5', port=443, username='admin',
                          password='private-camera-fixture-password', ca_pem=self.pem, expected_revision='factory')

    def put(self, value=None):
        return self.client.put(self.url, headers=self.auth, json=self.input if value is None else value)

    def get(self):
        return self.client.get(self.url, headers=self.auth)

    def test_legacy_hubs_keep_one_slot_and_reject_camera_setup(self):
        self.sync(fixture.STATE)
        self.assertEqual(self.status()['capabilities']['camera_slots'], 1)
        self.assertFalse(self.status()['capabilities']['camera_setup'])
        self.assertFalse(self.get().json['camera']['supported'])
        self.assertEqual(self.put().status_code, 409)
        with SessionLocal() as db:
            self.assertEqual(db.query(Esp32CameraConfig).count(), 0)

    def test_two_slots_are_negotiated_without_enabling_an_absent_camera(self):
        status = self.status()
        self.assertEqual(status['capabilities']['camera_slots'], 2)
        self.assertTrue(status['capabilities']['camera_setup'])
        self.assertTrue(status['camera_online'])
        self.assertFalse(status['cameras'][1]['enabled'])

    def test_password_is_encrypted_and_only_device_sync_receives_it(self):
        response = self.put()
        self.assertEqual(response.status_code, 200, response.json)
        revision = response.json['revision']
        public = self.get()
        self.assertNotIn(self.input['password'], public.text)
        self.assertTrue(public.json['camera']['password_saved'])
        with SessionLocal() as db:
            row = db.query(Esp32CameraConfig).one()
            self.assertNotIn(self.input['password'], row.encrypted_config)
            self.assertNotIn('BEGIN CERTIFICATE', row.encrypted_config)
        device = self.sync(multicamera_state()).json['camera_config']
        self.assertEqual(device['password'], self.input['password'])
        self.assertEqual(device['revision'], revision)
        state = multicamera_state()
        state['camera_config_staged_revision'] = revision
        self.assertNotIn('camera_config', self.sync(state).json)
        self.assertEqual(self.get().json['camera']['active_revision'], 'factory')
        state['camera_config_revision'] = revision
        state['cameras'][1].update(enabled=True, online=True, ready=True)
        self.sync(state)
        self.assertEqual(self.get().json['camera']['active_revision'], revision)

    def test_scope_and_optimistic_concurrency(self):
        self.assertEqual(self.client.put(self.url, json=self.input).status_code, 401)
        other = self.client.post('/auth/signup', json=dict(email='other-camera@example.com', password='another-long-password', phone_number='+918550991122')).json
        self.assertEqual(self.client.get(self.url, headers={'Authorization': 'Bearer '+other['token']}).status_code, 403)
        first = self.put()
        self.assertEqual(first.status_code, 200)
        self.assertEqual(self.put().status_code, 409)
        with SessionLocal() as db:
            self.assertEqual(db.query(Esp32CameraConfig).count(), 1)

    def test_only_online_disarmed_idle_hubs_can_be_configured(self):
        for changes in (dict(arm_state='armed'), dict(arm_state='arming'), dict(siren_active=True)):
            with self.subTest(changes=changes):
                self.sync(dict(multicamera_state(), **changes))
                self.assertEqual(self.put().status_code, 409)
        self.sync(multicamera_state())
        with SessionLocal() as db:
            db.get(Esp32Hub, 'ESP32-TEST').last_seen_at -= timedelta(minutes=1)
            db.commit()
        self.assertEqual(self.put().status_code, 409)

    def test_credentials_are_not_sent_to_an_armed_hub(self):
        self.assertEqual(self.put().status_code, 200)
        response = self.sync(dict(multicamera_state(), arm_state='armed'))
        self.assertNotIn('camera_config', response.json)

    def test_validation_rejects_public_addresses_private_keys_and_malformed_values(self):
        bad = [dict(host='8.8.8.8'), dict(host='127.0.0.1'), dict(host='http://192.168.0.5'),
               dict(port=True), dict(port=0), dict(port=65536), dict(enabled='true'),
               dict(password='x'*129), dict(username='x'*65), dict(ca_pem='invalid'),
               dict(ca_pem=self.pem+'-----BEGIN PRIVATE KEY-----\nsecret\n-----END PRIVATE KEY-----')]
        for changes in bad:
            with self.subTest(changes=list(changes)):
                self.assertEqual(self.put({**self.input, **changes}).status_code, 400)

    def test_password_retention_is_limited_to_the_same_camera(self):
        revision = self.put().json['revision']
        saved = self.put({**self.input, 'expected_revision': revision, 'password': ''})
        self.assertEqual(saved.status_code, 200)
        self.assertEqual(self.put({**self.input, 'expected_revision': saved.json['revision'], 'password': '', 'host': '192.168.0.6'}).status_code, 400)

    def test_disabling_camera_drops_saved_credentials(self):
        revision = self.put().json['revision']
        response = self.put(dict(enabled=False, expected_revision=revision))
        self.assertEqual(response.status_code, 200)
        self.assertFalse(self.get().json['camera']['password_saved'])
        payload = self.sync(multicamera_state()).json['camera_config']
        self.assertFalse(payload['enabled'])
        self.assertNotIn('password', payload)

    def test_per_camera_status_validation_and_camera2_intrusion(self):
        state = multicamera_state(True)
        response = self.sync(state, events=[dict(id='cam2-intrusion', event_type='intrusion-2')])
        self.assertEqual(response.status_code, 200, response.json)
        with SessionLocal() as db:
            self.assertEqual(db.query(Alert).one().event_type, 'intrusion-2')
        self.assertEqual(self.sync(multicamera_state(), events=[dict(id='bad-cam2-event', event_type='intrusion-2')]).status_code, 400)
        invalid = copy.deepcopy(state)
        invalid['cameras'][1]['camera_id'] = 1
        self.assertEqual(self.sync(invalid).status_code, 400)
        invalid = copy.deepcopy(state)
        invalid['cameras'][1]['online'] = False
        self.assertEqual(self.sync(invalid).status_code, 400)

    def test_independent_advisory_cooldowns_and_event_identity(self):
        self.sync(multicamera_state(True))
        with SessionLocal() as db:
            owner = db.get(Esp32Hub, 'ESP32-TEST').shop_id
            from database import Shop
            user_id = db.get(Shop, owner).user_id
            db.add(PushToken(id='multi-token', user_id=user_id, expo_push_token='ExpoPushToken[multicam-fixture]'))
            db.commit()
        with patch('app.send_expo_push_messages', return_value={'data': [{'status': 'ok', 'id': 'multi-ticket'}]}) as sender:
            for camera in (1, 2):
                response = self.client.post('/esp32/device/camera-advisory', headers=self.device, json=dict(event_id='same-episode-id', camera_id=camera))
                self.assertEqual(response.json['status'], 'accepted')
            response = self.client.post('/esp32/device/camera-advisory', headers=self.device, json=dict(event_id='second-episode', camera_id=2))
            self.assertEqual(response.json['status'], 'suppressed')
            self.assertEqual(sender.call_count, 2)
            self.assertEqual(sender.call_args.args[0][0]['data']['camera_id'], 2)
        with SessionLocal() as db:
            self.assertEqual({a.event_type for a in db.query(Alert).all()}, {'camera-interference-1', 'camera-interference-2'})
            self.assertEqual(db.query(Esp32CameraAdvisoryReceipt).count(), 3)
        self.assertEqual(self.status()['arm_state'], 'disarmed')

    def test_config_decryption_failure_does_not_break_device_sync(self):
        self.assertEqual(self.put().status_code, 200)
        with patch('esp32_camera_api.unseal', side_effect=InvalidToken):
            response = self.sync(multicamera_state())
            self.assertEqual(response.status_code, 200)
            self.assertNotIn('camera_config', response.json)
