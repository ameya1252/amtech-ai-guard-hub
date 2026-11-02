import base64
import hashlib
import os
from datetime import datetime, timedelta, timezone
from unittest.mock import MagicMock, patch
import unittest

import test_esp32 as fixture
from database import Alert, AlertEvidence, EvidenceRecorder, Esp32Hub, SessionLocal, Shop
from evidence_api import enqueue_evidence, describe

ENV = {'NETTRA_MEDIA_BUCKET':'private-fixture', 'NETTRA_MEDIA_ACCESS_KEY_ID':'fixture-key',
       'NETTRA_MEDIA_SECRET_ACCESS_KEY':'fixture-secret', 'R2_ACCOUNT_ID':'fixture-account'}


class EvidenceTests(unittest.TestCase):
    sync = fixture.Esp32Tests.sync

    def setUp(self):
        fixture.Esp32Tests.setUp(self)
        self.env = patch.dict(os.environ, ENV); self.env.start(); self.addCleanup(self.env.stop)
        self.storage = MagicMock()
        self.storage.generate_presigned_url.return_value = 'https://private.invalid/signed'
        self.mock = patch('evidence_api.storage', return_value=self.storage); self.mock.start(); self.addCleanup(self.mock.stop)
        self.config = f'/shop/{self.shop_id}/evidence-recorder'
        self.enroll()

    def enroll(self):
        r = self.client.put(self.config, headers=self.auth, json={'enabled':True,'camera_ids':[1,2]})
        self.assertEqual(r.status_code,200,r.json)
        self.recorder = {'X-NETTRA-RECORDER-SERIAL':'ESP32-TEST','X-NETTRA-RECORDER-TOKEN':r.json['token']}

    def event(self, event_id='one', kind='intrusion', state=None):
        timestamp=datetime.now(timezone.utc).isoformat()
        r=self.sync(state=state,events=[{'id':event_id,'event_type':kind,'timestamp':timestamp}])
        self.assertEqual(r.status_code,200,r.json)
        with SessionLocal() as db: return db.query(Alert).order_by(Alert.timestamp.desc()).first().id

    def poll(self):
        r=self.client.post('/evidence/recorder/poll',headers=self.recorder,json={'cameras':[{'camera_id':1,'online':True}]})
        self.assertEqual(r.status_code,200,r.json)
        return r.json['jobs']

    def action(self,job,action,**values):
        return self.client.post(f'/evidence/recorder/jobs/{job["id"]}/{action}',headers=self.recorder,json={'lease_id':job['lease_id'],**values})

    def manifest(self):
        data=b'fixture';md5=base64.b64encode(hashlib.md5(data).digest()).decode()
        return dict(pre_seconds=5,post_seconds=10,objects={kind:dict(size=len(data),sha256=hashlib.sha256(data).hexdigest(),md5=md5) for kind in ('photo','thumbnail','video')})

    def test_no_opt_in_no_jobs_and_no_storage_contact_in_sync(self):
        self.client.put(self.config,headers=self.auth,json={'enabled':False,'camera_ids':[1]})
        self.event()
        with SessionLocal() as db:self.assertEqual(db.query(AlertEvidence).count(),0)
        self.storage.generate_presigned_url.assert_not_called()

    def test_dedup_and_camera_specific_routing(self):
        self.event();self.event()
        with SessionLocal() as db:
            rows=db.query(AlertEvidence).all();self.assertEqual(len(rows),1);self.assertEqual(rows[0].camera_id,1)
        first = self.poll()
        self.assertEqual(len(first),1)
        self.assertEqual(self.poll()[0]['lease_id'], first[0]['lease_id'])

    def test_owner_auth_and_no_cross_site_reads(self):
        alert=self.event();url=f'/alerts/{self.shop_id}/{alert}/evidence'
        self.assertEqual(self.client.get(url).status_code,401)
        other=self.client.post('/auth/signup',json={'email':'other-evidence@example.com','password':'a-long-other-password','phone_number':'+918550991122'})
        headers={'Authorization':'Bearer '+other.json['token']}
        self.assertIn(self.client.get(url,headers=headers).status_code,(403,404))
        self.assertEqual(self.client.get(url,headers=self.auth).json['evidence'][0]['status'],'pending')
        self.assertEqual(self.client.get(f'/alerts/{self.shop_id}/missing/evidence',headers=self.auth).status_code,404)

    def test_rotation_revokes_old_recorder_not_hub(self):
        old=self.recorder;self.enroll()
        self.assertEqual(self.client.post('/evidence/recorder/poll',headers=old,json={}).status_code,401)
        self.assertEqual(self.sync().status_code,200)

    def test_missing_storage_blocks_enrollment_not_alarm(self):
        with patch.dict(os.environ,{'NETTRA_MEDIA_BUCKET':''}):
            self.assertEqual(self.client.put(self.config,headers=self.auth,json={'enabled':True,'camera_ids':[1]}).status_code,503)
            self.event()
            self.assertEqual(len(self.poll()),1)

    def test_stale_lease_cannot_attach_or_overwrite_media(self):
        self.event();first=self.poll()[0]
        with SessionLocal() as db:
            db.get(AlertEvidence,first['id']).lease_until=datetime.now(timezone.utc)-timedelta(seconds=1);db.commit()
        second=self.poll()[0];self.assertNotEqual(first['lease_id'],second['lease_id'])
        self.assertEqual(self.action(first,'prepare',manifest=self.manifest()).status_code,409)
        self.assertEqual(self.action(second,'prepare',manifest=self.manifest()).status_code,200)

    def test_prepare_limits_and_immutable_manifest(self):
        self.event();job=self.poll()[0];m=self.manifest();m['objects']['video']['size']=6_000_001
        self.assertEqual(self.action(job,'prepare',manifest=m).status_code,400)
        m=self.manifest();self.assertEqual(self.action(job,'prepare',manifest=m).status_code,200)
        self.assertEqual(self.action(job,'prepare',manifest=m).status_code,200)
        m['post_seconds']=9
        self.assertEqual(self.action(job,'prepare',manifest=m).status_code,409)

    def test_completion_checks_storage_and_is_idempotent(self):
        alert=self.event();job=self.poll()[0];m=self.manifest()
        self.assertEqual(self.action(job,'prepare',manifest=m).status_code,200)
        self.storage.head_object.return_value={'ContentLength':1}
        self.assertEqual(self.action(job,'complete').status_code,409)
        def head(**kw):
            kind=kw['Key'].rsplit('/',1)[1].split('-')[0];v=m['objects'][kind]
            return dict(ContentLength=v['size'],ContentType='video/mp4' if kind=='video' else 'image/jpeg',Metadata={'sha256':v['sha256']},ETag='"'+base64.b64decode(v['md5']).hex()+'"')
        self.storage.head_object.side_effect=head
        self.assertEqual(self.action(job,'complete').json['status'],'ready')
        self.assertEqual(self.action(job,'complete').json['status'],'ready')
        result=self.client.get(f'/alerts/{self.shop_id}/{alert}/evidence',headers=self.auth)
        self.assertEqual(result.headers['Cache-Control'],'private, no-store')
        self.assertIn('video',result.json['evidence'][0])
        rows=self.client.get(f'/alerts/{self.shop_id}',headers=self.auth).json['alerts']
        self.assertTrue(rows[0]['has_thumbnail']);self.assertNotIn('video',rows[0]['evidence'][0])

    def test_expired_evidence_never_signs_or_returns_media(self):
        self.event();job=self.poll()[0]
        with SessionLocal() as db:
            row=db.get(AlertEvidence,job['id']);row.status='ready';row.expires_at=datetime.now(timezone.utc)-timedelta(seconds=1)
            row.manifest={'objects':{'photo':{'key':'private','size':10}}}
            self.assertEqual(describe(row,urls=True)['status'],'expired')
            self.storage.generate_presigned_url.assert_not_called()

    def test_failed_recorder_does_not_issue_commands(self):
        self.event();job=self.poll()[0]
        self.assertEqual(self.action(job,'fail',reason='buffer_missing').json['status'],'unavailable')
        self.assertIsNone(self.sync().json['command'])

    def test_smoke_selects_only_enabled_camera_slots(self):
        self.event(kind='smoke')
        with SessionLocal() as db:self.assertEqual([r.camera_id for r in db.query(AlertEvidence).all()],[1])

    def test_recorder_revoked_if_hub_site_changes(self):
        with SessionLocal() as db:
            db.get(Esp32Hub,'ESP32-TEST').shop_id=None;db.commit()
        self.assertEqual(self.client.post('/evidence/recorder/poll',headers=self.recorder,json={}).status_code,401)

    def test_reenrollment_cannot_claim_or_finish_previous_site_jobs(self):
        self.event(); old_job=self.poll()[0]
        with SessionLocal() as db:
            owner=db.get(Shop,self.shop_id).user_id
            db.add(Shop(id='replacement-site',shop_name='Replacement',user_id=owner));db.flush()
            db.get(Esp32Hub,'ESP32-TEST').shop_id='replacement-site';db.commit()
        r=self.client.put('/shop/replacement-site/evidence-recorder',headers=self.auth,json={'enabled':True,'camera_ids':[1]})
        self.assertEqual(r.status_code,200,r.json)
        self.recorder['X-NETTRA-RECORDER-TOKEN']=r.json['token']
        self.assertEqual(self.poll(),[])
        self.assertEqual(self.action(old_job,'prepare',manifest=self.manifest()).status_code,409)

    def test_two_camera_incidents_keep_source_identity(self):
        from test_esp32_cameras import multicamera_state
        self.assertEqual(self.sync(multicamera_state(True)).status_code,200)
        second=self.event(kind='intrusion-2',state=multicamera_state(True))
        smoke=self.event('smoke-event','smoke',state=multicamera_state(True))
        with SessionLocal() as db:
            self.assertEqual([r.camera_id for r in db.query(AlertEvidence).filter_by(alert_id=second)],[2])
            self.assertEqual(sorted(r.camera_id for r in db.query(AlertEvidence).filter_by(alert_id=smoke)),[1,2])

    def test_advisory_enqueues_media_without_an_alarm_command(self):
        r=self.client.post('/esp32/device/camera-advisory',headers=self.device,json={'event_id':'cover-test','camera_id':1})
        self.assertEqual(r.status_code,200,r.json)
        with SessionLocal() as db:
            alert=db.query(Alert).one();self.assertEqual(alert.event_type,'camera-interference-1')
            self.assertEqual(db.query(AlertEvidence).filter_by(alert_id=alert.id).count(),1)
        self.assertIsNone(self.sync().json['command'])


if __name__=='__main__':unittest.main()
