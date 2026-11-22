import os
import unittest
from datetime import datetime, timedelta, timezone
from unittest.mock import patch, MagicMock
import test_esp32 as fixture
from database import EvidenceRecorder, Esp32Hub, SessionLocal


class LiveTests(unittest.TestCase):
    sync=fixture.Esp32Tests.sync
    def setUp(self):
        fixture.Esp32Tests.setUp(self)
        self.env=patch.dict(os.environ,{'NETTRA_LIVE_MEDIA_ORIGIN':'https://media.invalid','NETTRA_LIVE_CONTROL_KEY':'k'*43})
        self.env.start();self.addCleanup(self.env.stop)
        with SessionLocal() as db:
            db.add(EvidenceRecorder(serial='ESP32-TEST',shop_id=self.shop_id,token_hash='fixture',enabled=True,camera_ids=[1,2]));db.commit()
        self.endpoint=f'/shop/{self.shop_id}/cameras/1/live'
        self.remote=MagicMock(status_code=201)
        self.remote.json.return_value=dict(session_id='s'*43,expires_at=(datetime.now(timezone.utc)+timedelta(seconds=180)).isoformat(),duration_seconds=180)
        self.mock=patch('live_api.requests.request',return_value=self.remote);self.send=self.mock.start();self.addCleanup(self.mock.stop)

    def test_owner_session_contains_no_camera_or_publisher_credentials(self):
        result=self.client.post(self.endpoint,headers=self.auth)
        self.assertEqual(result.status_code,201,result.json)
        self.assertEqual(result.headers['Cache-Control'],'private, no-store')
        self.assertEqual(result.json['url'],'https://media.invalid/live/'+'s'*43+'/index.m3u8')
        payload=self.send.call_args.kwargs['json']
        self.assertEqual((payload['serial'],payload['shop_id'],payload['camera_id'],payload['revision']),('ESP32-TEST',self.shop_id,1,'factory'))
        self.assertNotIn('password',str(result.json));self.assertNotIn('k'*43,str(result.json))
        self.assertIsNone(self.sync().json['command'])

    def test_authentication_tenant_scope_and_disabled_camera(self):
        self.assertEqual(self.client.post(self.endpoint).status_code,401)
        other=self.client.post('/auth/signup',json=dict(email='live-other@example.com',password='different-password',phone_number='+918550991124'))
        self.assertEqual(self.client.post(self.endpoint,headers={'Authorization':'Bearer '+other.json['token']}).status_code,403)
        self.assertEqual(self.client.post(self.endpoint.replace('/1/','/2/'),headers=self.auth).status_code,409)
        self.send.assert_not_called()

    def test_hub_offline_does_not_block_a_healthy_independent_camera_stream(self):
        with SessionLocal() as db:
            db.get(Esp32Hub,'ESP32-TEST').last_seen_at=datetime.now(timezone.utc)-timedelta(minutes=2);db.commit()
        self.assertEqual(self.client.post(self.endpoint,headers=self.auth).status_code,201)

    def test_close_is_owner_scoped_and_does_not_issue_alarm_commands(self):
        self.remote.status_code=200
        result=self.client.delete(self.endpoint+'/'+'s'*43,headers=self.auth)
        self.assertEqual(result.status_code,200)
        self.assertEqual(self.send.call_args.args[0],'DELETE')
        self.assertIsNone(self.sync().json['command'])

    def test_bad_or_unavailable_media_fails_without_exposing_remote_details(self):
        for code in (401,403,404,500,503):
            self.remote.status_code=code
            self.assertEqual(self.client.post(self.endpoint,headers=self.auth).status_code,503)
        self.remote.status_code=201;self.remote.json.return_value={'session_id':'invalid','duration_seconds':180,'expires_at':'invalid'}
        self.assertEqual(self.client.post(self.endpoint,headers=self.auth).status_code,503)
        with patch.dict(os.environ,{'NETTRA_LIVE_MEDIA_ORIGIN':'http://insecure.invalid'}):
            self.assertEqual(self.client.post(self.endpoint,headers=self.auth).status_code,503)

    def test_disabled_or_reassigned_recorder_cannot_start_live(self):
        with SessionLocal() as db:
            db.get(EvidenceRecorder,'ESP32-TEST').enabled=False;db.commit()
        self.assertEqual(self.client.post(self.endpoint,headers=self.auth).status_code,409)
        self.send.assert_not_called()


if __name__=='__main__':unittest.main()
