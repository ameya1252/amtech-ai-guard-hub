from datetime import datetime, timedelta, timezone
from io import BytesIO
import os
import unittest
from unittest.mock import MagicMock, patch

import test_esp32 as fixture
from database import Alert, AlertEvidence, Esp32EventReceipt, Esp32Hub, HubAlarmPhoto, SessionLocal, Shop
from alarm_photo_api import optimize_photo
from PIL import Image
from test_evidence import ENV


class AlarmPhotoTests(unittest.TestCase):
    sync = fixture.Esp32Tests.sync

    def setUp(self):
        fixture.Esp32Tests.setUp(self)
        self.env = patch.dict(os.environ, ENV)
        self.env.start(); self.addCleanup(self.env.stop)
        self.storage = MagicMock()
        self.storage.generate_presigned_url.return_value = 'https://private.invalid/signed'
        self.mock = patch('evidence_api.storage', return_value=self.storage)
        self.mock.start(); self.addCleanup(self.mock.stop)
        self.capture_time = int(datetime.now(timezone.utc).timestamp())
        self.photo = self.jpeg()

    def jpeg(self, color='red', size=(1920, 1080)):
        out = BytesIO(); image = Image.new('RGB', size, color)
        exif = Image.Exif(); exif[270] = 'private-camera-metadata'
        image.save(out, format='JPEG', quality=90, exif=exif)
        return out.getvalue()

    def event(self, event_id='photo-one', kind='intrusion', state=None):
        self.event_id = event_id
        state = {**(state or fixture.STATE), 'snapshot_protocol_version': 1}
        r = self.sync(state, events=[{'id': event_id, 'event_type': kind,
                                    'timestamp': datetime.fromtimestamp(self.capture_time, timezone.utc).isoformat()}])
        self.assertEqual(r.status_code, 200, r.json)
        with SessionLocal() as db:
            self.alert_id = db.get(Esp32EventReceipt, 'ESP32-TEST:'+event_id).alert_id
        return r

    def upload(self, camera=1, data=None, captured=None, headers=None, event_id=None):
        return self.client.post(f'/esp32/device/photos/{event_id or self.event_id}/{camera}',
                                headers={**(headers if headers is not None else self.device),
                                         'X-NETTRA-CAPTURED-AT': str(self.capture_time if captured is None else captured)},
                                data=self.photo if data is None else data, content_type='image/jpeg')

    def evidence(self, headers=None):
        return self.client.get(f'/alerts/{self.shop_id}/{self.alert_id}/evidence',
                               headers=headers if headers is not None else self.auth)

    def test_snapshot_jobs_without_recorder_and_duplicate_sync(self):
        self.event(); self.event()
        with SessionLocal() as db:
            self.assertEqual(db.query(HubAlarmPhoto).count(), 1)
            self.assertEqual(db.query(AlertEvidence).count(), 0)
        self.storage.put_object.assert_not_called()
        self.assertEqual(self.evidence().json['evidence'][0]['status'], 'pending')
        self.assertIsNone(self.sync().json['command'])

    def test_old_firmware_does_not_advertise_or_enqueue_snapshots(self):
        self.sync(events=[{'id': 'old', 'event_type': 'intrusion'}])
        with SessionLocal() as db:
            self.assertEqual(db.query(HubAlarmPhoto).count(), 0)
        self.assertEqual(self.upload(event_id='old').status_code, 404)

    def test_camera_routing_and_disabled_second_camera(self):
        self.event(kind='smoke')
        with SessionLocal() as db:
            self.assertEqual([p.camera_id for p in db.query(HubAlarmPhoto).all()], [1])
        self.assertEqual(self.upload(camera=2).status_code, 404)
        from test_esp32_cameras import multicamera_state
        self.event('second', 'intrusion-2', multicamera_state(True))
        with SessionLocal() as db:
            self.assertEqual([p.camera_id for p in db.query(HubAlarmPhoto).filter_by(alert_id=self.alert_id)], [2])
        self.assertEqual(self.upload(camera=1).status_code, 404)
        self.assertEqual(self.upload(camera=2).status_code, 200)

    def test_device_authentication_and_unknown_alert(self):
        self.event()
        self.assertEqual(self.upload(headers={}).status_code, 401)
        self.assertEqual(self.upload(headers={**self.device, 'X-AMTECH-DEVICE-TOKEN': 'wrong'}).status_code, 401)
        self.assertEqual(self.upload(event_id='invented').status_code, 404)
        self.assertEqual(self.upload(headers=self.auth).status_code, 401)
        self.storage.put_object.assert_not_called()

    def test_photo_optimization_strips_metadata_and_limits_resolution(self):
        media = optimize_photo(self.jpeg(size=(3200, 1800)))
        for kind, data in media.items():
            with Image.open(BytesIO(data)) as image:
                image.load()
                self.assertFalse(image.getexif())
                limit = (1920, 1080) if kind == 'photo' else (320, 180)
                self.assertLessEqual(image.width, limit[0]); self.assertLessEqual(image.height, limit[1])

    def test_small_compressed_camera_jpeg_retains_pixels_without_metadata(self):
        from alarm_photo_api import strip_jpeg_metadata
        for progressive in (False, True):
            source = BytesIO()
            image = Image.new('RGB', (640, 360), 'green')
            exif = Image.Exif(); exif[270] = 'private-camera-data'
            image.save(source, format='JPEG', quality=70, progressive=progressive, exif=exif)
            data = source.getvalue()
            clean = strip_jpeg_metadata(data)
            self.assertIsNotNone(clean)
            self.assertLess(len(clean), len(data))
            with Image.open(BytesIO(data)) as before, Image.open(BytesIO(clean)) as after:
                self.assertFalse(after.getexif())
                self.assertEqual(before.tobytes(), after.tobytes())

    def test_upload_private_reads_thumbnail_and_idempotence(self):
        self.event()
        self.assertEqual(self.upload().status_code, 200)
        self.assertEqual(self.upload().status_code, 200)
        self.assertEqual(self.storage.put_object.call_count, 2)
        for call in self.storage.put_object.call_args_list:
            self.assertEqual(call.kwargs['CacheControl'], 'private, no-store')
            self.assertEqual(call.kwargs['Bucket'], ENV['NETTRA_MEDIA_BUCKET'])
            self.assertTrue(call.kwargs['Key'].startswith('evidence/v1/ESP32-TEST/snapshots/'))
        self.assertEqual(self.upload(data=self.jpeg('blue')).status_code, 409)
        detail = self.evidence()
        self.assertEqual(detail.headers['Cache-Control'], 'private, no-store')
        item = detail.json['evidence'][0]
        self.assertEqual(item['status'], 'ready'); self.assertIn('photo', item); self.assertNotIn('video', item)
        self.assertEqual(item['capture_mode'], 'snapshot')
        self.assertIsNotNone(item['captured_at'])
        listing = self.client.get(f'/alerts/{self.shop_id}', headers=self.auth).json['alerts'][0]
        self.assertTrue(listing['has_thumbnail']); self.assertNotIn('photo', listing['evidence'][0])
        redirect = self.client.get(f'/alerts/{self.shop_id}/{self.alert_id}/thumbnail', headers=self.auth)
        self.assertEqual(redirect.status_code, 302)

    def test_owner_and_site_bound_reads(self):
        self.event(); self.upload()
        self.assertEqual(self.evidence(headers={}).status_code, 401)
        other = self.client.post('/auth/signup', json={'email': 'photo-other@example.com',
                                  'password': 'fixture-password', 'phone_number': '+918550991123'})
        self.assertEqual(self.evidence({'Authorization': 'Bearer '+other.json['token']}).status_code, 403)

    def test_bad_jpeg_oversize_and_pixel_bomb_never_reach_storage(self):
        self.event()
        self.assertEqual(self.upload(data=b'not-an-image').status_code, 400)
        self.assertEqual(self.upload(data=b'\xff\xd8bad\xff\xd9').status_code, 400)
        self.assertEqual(self.upload(data=b'x'*(1024*1024+1)).status_code, 413)
        with patch('alarm_photo_api.MAX_PIXELS', 100):
            self.assertEqual(self.upload().status_code, 400)
        self.storage.put_object.assert_not_called()

    def test_capture_must_be_near_alarm_and_upload_not_stale(self):
        self.event()
        self.assertEqual(self.upload(captured=self.capture_time-6).status_code, 400)
        self.assertEqual(self.upload(captured=self.capture_time+60).status_code, 400)
        with SessionLocal() as db:
            row = db.query(HubAlarmPhoto).one()
            row.event_at = datetime.now(timezone.utc)-timedelta(minutes=11); db.commit()
        self.assertEqual(self.upload().status_code, 410)
        self.assertEqual(self.evidence().json['evidence'][0]['status'], 'unavailable')
        self.assertEqual(self.evidence().json['evidence'][0]['reason'], 'capture_timeout')

    def test_storage_failure_retry_preserves_same_capture_and_alarm(self):
        self.event()
        self.storage.put_object.side_effect = RuntimeError('storage unavailable')
        self.assertEqual(self.upload().status_code, 503)
        self.assertIsNone(self.sync().json['command'])
        self.storage.put_object.side_effect = None
        self.assertEqual(self.upload().status_code, 200)
        self.assertEqual(self.evidence().json['evidence'][0]['status'], 'ready')

    def test_failure_is_honest_and_cannot_replace_ready_photo(self):
        self.event()
        endpoint = f'/esp32/device/photos/{self.event_id}/1/failure'
        self.assertEqual(self.client.post(endpoint, headers=self.device, json={'reason': 'camera_offline'}).status_code, 200)
        self.assertEqual(self.evidence().json['evidence'][0]['status'], 'unavailable')
        self.upload()
        result = self.client.post(endpoint, headers=self.device, json={'reason': 'camera_offline'})
        self.assertEqual(result.json['status'], 'ready')
        self.assertEqual(self.evidence().json['evidence'][0]['status'], 'ready')

    def test_video_recorder_cannot_hide_photo_and_merges_without_duplicate_cards(self):
        self.event(); self.upload()
        with SessionLocal() as db:
            photo = db.query(HubAlarmPhoto).one()
            recorder = AlertEvidence(id='recorder-test', alert_id=self.alert_id, serial='ESP32-TEST', camera_id=1,
                                     status='pending', event_at=photo.event_at, expires_at=photo.expires_at)
            db.add(recorder); db.commit()
        self.assertEqual(len(self.evidence().json['evidence']), 1)
        self.assertEqual(self.evidence().json['evidence'][0]['status'], 'ready')
        with SessionLocal() as db:
            row = db.get(AlertEvidence, 'recorder-test'); row.status = 'ready'
            row.manifest = {'pre_seconds': 5, 'post_seconds': 10, 'objects': {'video': {'key': 'private-video', 'size': 100}}}
            db.commit()
        items = self.evidence().json['evidence']
        self.assertEqual(len(items), 1); self.assertIn('photo', items[0]); self.assertIn('video', items[0])
        self.assertEqual(items[0]['pre_seconds'], 5)

    def test_buffered_pre_alarm_photo_replaces_snapshot_only_after_ready(self):
        self.event();self.upload()
        with SessionLocal() as db:
            photo=db.query(HubAlarmPhoto).one()
            row=AlertEvidence(id='buffered-pre-frame',alert_id=self.alert_id,serial='ESP32-TEST',camera_id=1,
                status='capturing',event_at=photo.event_at,expires_at=photo.expires_at)
            db.add(row);db.commit()
        pending=self.evidence().json['evidence'][0]
        self.assertEqual(pending['capture_mode'],'snapshot')
        self.assertEqual(pending['video_status'],'unavailable')
        with SessionLocal() as db:
            row=db.get(AlertEvidence,'buffered-pre-frame');row.status='ready'
            row.manifest={'pre_seconds':5,'post_seconds':10,'photo_offset_seconds':-2,
                'objects':{'photo':{'key':'pre-photo','size':100},'video':{'key':'incident-video','size':1000}}}
            db.commit()
        cards=self.evidence().json['evidence'];self.assertEqual(len(cards),1)
        self.assertEqual(cards[0]['capture_mode'],'recorded_frame')
        self.assertEqual(cards[0]['photo_offset_seconds'],-2)
        self.assertIn('video',cards[0]);self.assertIn('photo',cards[0])

    def test_silent_photo_test_also_checks_enrolled_video_without_alarm_push(self):
        self.sync({**fixture.STATE,'snapshot_protocol_version':1})
        enrolled=self.client.put(f'/shop/{self.shop_id}/evidence-recorder',headers=self.auth,
            json={'enabled':True,'camera_ids':[1]})
        self.assertEqual(enrolled.status_code,200)
        with patch('esp32_api.enqueue_alarm_pushes') as push:
            for _ in range(2):
                response=self.client.post('/esp32/device/photo-test',headers=self.device,
                    json={'event_id':'silent-buffer-test','camera_id':1,'event_at':self.capture_time})
                self.assertEqual(response.status_code,200,response.json)
            push.assert_not_called()
        with SessionLocal() as db:
            self.assertEqual(db.query(AlertEvidence).count(),1)
            self.assertEqual(db.query(HubAlarmPhoto).count(),1)
            self.assertEqual(db.query(Alert).one().event_type,'test')
        self.assertIsNone(self.sync().json['command'])

    def test_reassignment_cannot_upload_old_site_photo(self):
        self.event()
        with SessionLocal() as db:
            owner = db.get(Shop, self.shop_id).user_id
            db.add(Shop(id='photo-replacement', shop_name='Replacement', user_id=owner)); db.flush()
            db.get(Esp32Hub, 'ESP32-TEST').shop_id = 'photo-replacement'; db.commit()
        self.assertEqual(self.upload().status_code, 404)

    def test_retention_never_returns_expired_photo_url(self):
        self.event(); self.upload()
        with SessionLocal() as db:
            db.query(HubAlarmPhoto).one().expires_at = datetime.now(timezone.utc)-timedelta(seconds=1)
            db.commit()
        self.storage.generate_presigned_url.reset_mock()
        self.assertEqual(self.evidence().json['evidence'][0]['status'], 'expired')
        self.storage.generate_presigned_url.assert_not_called()

    def test_silent_commissioning_is_idempotent_and_never_enqueues_alarm_push(self):
        self.sync({**fixture.STATE, 'snapshot_protocol_version': 1})
        payload = {'event_id': 'silent-photo', 'camera_id': 1, 'event_at': self.capture_time}
        self.assertEqual(self.client.post('/esp32/device/photo-test', json=payload).status_code, 401)
        with patch('esp32_api.enqueue_alarm_pushes') as push:
            for _ in range(2):
                response = self.client.post('/esp32/device/photo-test', headers=self.device, json=payload)
                self.assertEqual(response.status_code, 200, response.json)
            push.assert_not_called()
        with SessionLocal() as db:
            self.assertEqual(db.query(Alert).count(), 1)
            self.assertEqual(db.query(Alert).one().event_type, 'test')
            self.assertEqual(db.query(HubAlarmPhoto).count(), 1)
        self.assertEqual(self.upload(event_id='silent-photo').status_code, 200)
        self.assertIsNone(self.sync().json['command'])

    def test_commissioning_requires_safe_state_and_cannot_reuse_alarm_identity(self):
        self.event()
        payload = {'event_id': self.event_id, 'camera_id': 1, 'event_at': self.capture_time}
        self.assertEqual(self.client.post('/esp32/device/photo-test', headers=self.device, json=payload).status_code, 409)
        payload['event_id'] = 'new-silent-test'
        self.sync({**fixture.STATE, 'snapshot_protocol_version': 1, 'arm_state': 'armed'})
        self.assertEqual(self.client.post('/esp32/device/photo-test', headers=self.device, json=payload).status_code, 409)
        self.sync({**fixture.STATE, 'snapshot_protocol_version': 1})
        payload['camera_id'] = 2
        self.assertEqual(self.client.post('/esp32/device/photo-test', headers=self.device, json=payload).status_code, 409)


if __name__ == '__main__': unittest.main()
