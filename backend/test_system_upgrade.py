import unittest
from datetime import datetime,timedelta,timezone
from queue import Empty
from unittest.mock import patch
import test_esp32 as fixture
from database import SessionLocal,Shop,User,TechnicianSiteAccess,Esp32ContactConfig
from test_esp32_cameras import MultiCameraTests,multicamera_state
from status_stream import subscribe,unsubscribe,publish_status


def upgraded_state():
    return dict(multicamera_state(),status_protocol_version=2,camera_config_protocol_version=2,
                camera_1_config_revision='factory',camera_1_config_staged_revision='factory',
                contact_protocol_version=1,contact_config_revision='factory',
                alert_contacts=[dict(slot=1,phone='+12025550101')])


class ContactsTests(unittest.TestCase):
    sync=fixture.Esp32Tests.sync
    status=fixture.Esp32Tests.status
    def setUp(self):
        fixture.Esp32Tests.setUp(self)
        self.sync(upgraded_state())
        self.url=f'/shop/{self.shop_id}/esp32/contacts'
        self.input=dict(expected_revision='factory',contacts=[dict(slot=1,name='Owner',phone='+12025550102')])
    def put(self,value=None):return self.client.put(self.url,headers=self.auth,json=self.input if value is None else value)
    def test_change_is_pending_until_device_confirms_and_idempotent(self):
        first=self.put();self.assertEqual(first.status_code,200,first.json)
        self.assertFalse(first.json['applied']);revision=first.json['revision']
        current=self.client.get(self.url,headers=self.auth).json
        self.assertEqual(current['applied_contacts'][0]['phone'],'+12025550101')
        self.assertFalse(current['applied']);self.assertEqual(self.put().status_code,409)
        offer=self.sync(upgraded_state()).json['contact_config'];self.assertEqual(offer['revision'],revision)
        self.assertEqual(self.sync(upgraded_state()).json['contact_config'],offer)
        state=dict(upgraded_state(),contact_config_revision=revision,alert_contacts=[dict(slot=1,phone='+12025550102')])
        self.assertNotIn('contact_config',self.sync(state).json)
        self.assertTrue(self.client.get(self.url,headers=self.auth).json['applied'])
    def test_legacy_and_armed_hubs_are_not_configurable(self):
        self.sync();self.assertEqual(self.put().status_code,409)
        self.sync(dict(upgraded_state(),arm_state='armed'));self.assertEqual(self.put().status_code,409)
        self.assertNotIn('contact_config',self.sync(dict(upgraded_state(),arm_state='armed')).json)
    def test_scope_empty_duplicates_and_bad_numbers(self):
        self.assertEqual(self.client.put(self.url,json=self.input).status_code,401)
        for value in ([],[dict(slot=1,name='',phone='123')],[dict(slot=1,name='',phone='+12025550101')]*2):
            self.assertEqual(self.put(dict(self.input,contacts=value)).status_code,400)
        with SessionLocal() as db:self.assertEqual(db.query(Esp32ContactConfig).count(),0)
    def test_post_commit_status_fanout_and_bounded_queue(self):
        inbox=subscribe(self.shop_id)
        try:
            self.sync(upgraded_state());state=inbox.get_nowait();self.assertEqual(state['shop_id'],self.shop_id)
            self.assertTrue(state['hub_online']);self.assertEqual(state['contact_config_revision'],'factory')
            for i in range(20):publish_status(self.shop_id,dict(sequence=i))
            self.assertEqual(inbox.get_nowait(),dict(sequence=19))
            with self.assertRaises(Empty):inbox.get_nowait()
        finally:unsubscribe(self.shop_id,inbox)


class TechnicianTests(MultiCameraTests):
    def test_owner_without_technician_grant_cannot_edit(self):
        with SessionLocal() as db:
            db.query(TechnicianSiteAccess).delete();db.commit()
        self.assertFalse(self.get().json['camera']['can_edit'])
        self.assertEqual(self.put().status_code,403)
    def test_expired_grant_cannot_edit(self):
        with SessionLocal() as db:
            db.query(TechnicianSiteAccess).one().expires_at=datetime.now(timezone.utc)-timedelta(seconds=1);db.commit()
        self.assertEqual(self.put().status_code,403)
    def test_primary_requires_v2_and_cannot_be_disabled(self):
        self.url=f'/shop/{self.shop_id}/esp32/camera-1'
        self.assertEqual(self.put().status_code,409)
        self.sync(upgraded_state())
        self.assertEqual(self.put(dict(self.input,enabled=False)).status_code,400)
        response=self.put();self.assertEqual(response.status_code,200,response.json)
        device=self.sync(upgraded_state()).json['camera_config']
        self.assertEqual(device['camera_id'],1)
        self.assertEqual(device['password'],self.input['password'])
        self.assertNotIn(self.input['password'],self.get().text)
    def test_installer_gets_only_assigned_site_status_and_camera_access(self):
        technician=self.client.post('/auth/signup',json=dict(email='installer@example.com',password='fixture-long-password',phone_number='+12025550103')).json
        token={'Authorization':'Bearer '+technician['token']}
        with SessionLocal() as db:
            user=db.query(User).filter_by(email='installer@example.com').one()
            db.add(TechnicianSiteAccess(shop_id=self.shop_id,user_id=user.id,expires_at=datetime.now(timezone.utc)+timedelta(hours=1),granted_by='test-operator'));db.commit()
        sites=self.client.get('/me/shops',headers=token)
        self.assertEqual([s['shop_id'] for s in sites.json['shops']],[self.shop_id])
        status=self.client.get(f'/shop/{self.shop_id}/status',headers=token)
        self.assertEqual(status.status_code,200);self.assertFalse(status.json['capabilities']['arm'])
        self.assertEqual(self.client.put(self.url,headers=token,json=self.input).status_code,200)
        self.assertEqual(self.client.post(f'/shop/{self.shop_id}/arm',headers=token).status_code,403)
        self.assertEqual(self.client.get(f'/shop/{self.shop_id}/esp32/contacts',headers=token).status_code,403)
        self.assertEqual(self.client.get(f'/alerts/{self.shop_id}',headers=token).status_code,403)
