import base64
import hashlib
from unittest import TestCase, main
from unittest.mock import patch
from uuid import uuid4
from datetime import timedelta
import test_operations as fixture
from database import SessionLocal, Esp32Hub
from operations_models import Asset, FactoryRun, OperationsEvent
from operations_api import unseal, EOL_CHECKS, now
from factory_api import release_manifest

class FactoryTests(TestCase):
    setUp=fixture.OperationsTests.setUp
    sync=fixture.OperationsTests.sync
    op=fixture.OperationsTests.op
    batch=fixture.OperationsTests.batch
    labels=fixture.OperationsTests.labels
    def payload(self):
        label=self.labels(self.batch())[0]
        return dict(serial=label['serial'],hardware_id='aabbccddeeff',request_id=str(uuid4()),operation='firmware',
                    release_id=release_manifest()['id'],confirmed=True,chip='ESP32-S3',flash_size=16*1024*1024,
                    security_clear=True,factory_blank=True,table_valid=False,table_blank=True)
    def prepare(self,p):
        result=self.op('POST','factory/prepare',p);self.assertEqual(result.status_code,200,result.json);return result.json
    def verify(self,result,**changes):
        return self.op('POST',f"factory/runs/{result['run']['id']}/verify",{**dict(hardware_id=result['run']['hardware_id'],sha256=result['image']['sha256'],release_id=result['run']['release_id']),**changes})
    def test_private_release_and_real_image_roundtrip(self):
        self.assertEqual(self.op('GET','factory/release',key=False).status_code,401)
        self.assertEqual(self.op('GET','factory/release').status_code,200)
        p=self.payload();result=self.prepare(p);image=base64.b64decode(result['image']['data'])
        self.assertEqual(len(image),0x6000);self.assertEqual(hashlib.sha256(image).hexdigest(),result['image']['sha256'])
        self.assertIn(p['serial'].encode(),image);self.assertIn(p['hardware_id'].encode(),image)
        self.assertFalse(result['run']['ready_for_eol'])
        self.assertNotIn('device_token',result);self.assertNotIn('pairing_code',result)
        again=self.prepare(p);self.assertEqual(again['image'],result['image'])
        self.assertEqual(self.verify(result,sha256='0'*64).status_code,400)
        self.assertEqual(self.verify(result).status_code,200);self.assertEqual(self.verify(result).status_code,200)
        with SessionLocal() as db:
            self.assertEqual(db.query(FactoryRun).count(),1)
            self.assertEqual(db.query(OperationsEvent).filter_by(kind='usb_verified').count(),1)
        self.assertEqual(self.op('POST','factory/prepare',p).status_code,409)
    def test_matching_fresh_cloud_report_is_required(self):
        p=self.payload();result=self.prepare(p);self.verify(result)
        with SessionLocal() as db:token=unseal(db.get(Asset,p['serial']).token_cipher)
        headers={'X-AMTECH-DEVICE-SERIAL':p['serial'],'X-AMTECH-DEVICE-TOKEN':token}
        state={**fixture.fixture.STATE,'hardware_id':p['hardware_id'],'alert_outbox_count':0,'firmware_version':'wrong-version'}
        self.client.post('/esp32/device/sync',headers=headers,json={'state':state})
        path='factory/runs/'+result['run']['id']
        self.assertFalse(self.op('GET',path).json['run']['ready_for_eol'])
        state['firmware_version']=release_manifest()['firmware_version']
        self.client.post('/esp32/device/sync',headers=headers,json={'state':state})
        self.assertTrue(self.op('GET',path).json['run']['ready_for_eol'])
        passed=self.op('POST',f"assets/{p['serial']}/test",{'checks':dict.fromkeys(EOL_CHECKS,True)})
        self.assertEqual(passed.status_code,200,passed.json)
        self.assertEqual(self.op('POST','factory/prepare',{**p,'request_id':str(uuid4())}).status_code,409)
        with SessionLocal() as db:db.get(Esp32Hub,p['serial']).last_seen_at=now()-timedelta(minutes=1);db.commit()
        self.assertFalse(self.op('GET',path).json['run']['ready_for_eol'])
    def test_board_release_and_confirmation_gates(self):
        p=self.payload()
        for changes in [{'chip':'ESP32'},{'flash_size':8*1024*1024},{'security_clear':False},{'confirmed':False},{'release_id':'old'},{'hardware_id':'../bad'},{'factory_blank':None}]:
            with self.subTest(changes=changes):self.assertEqual(self.op('POST','factory/prepare',{**p,**changes}).status_code,400)
        self.assertEqual(self.op('POST','factory/prepare',{**p,'factory_blank':False}).status_code,409)
        with SessionLocal() as db:self.assertIsNone(db.get(Asset,p['serial']).hardware_id)
        self.prepare(p)
        self.assertEqual(self.op('POST','factory/prepare',{**p,'hardware_id':'001122334455'}).status_code,400)
        q={**p,'serial':self.labels(self.batch())[0]['serial'],'request_id':str(uuid4())}
        self.assertEqual(self.op('POST','factory/prepare',q).status_code,409)
    def test_image_failure_rolls_back_identity_allocation(self):
        p=self.payload()
        with patch('factory_api.identity_image',side_effect=ValueError('Image generation failed')):
            self.assertEqual(self.op('POST','factory/prepare',p).status_code,400)
        with SessionLocal() as db:
            asset=db.get(Asset,p['serial']);self.assertIsNone(asset.hardware_id);self.assertIsNone(asset.token_cipher);self.assertEqual(db.query(FactoryRun).count(),0)
    def test_one_board_cannot_use_another_run_or_sticker(self):
        p=self.payload();result=self.prepare(p)
        self.assertEqual(self.verify(result,hardware_id='001122334455').status_code,400)
        self.assertEqual(self.verify(result,release_id='wrong').status_code,400)
        self.assertEqual(self.op('GET','factory/runs/not-found').status_code,404)
    def test_interrupted_table_write_requires_an_existing_matching_preparation(self):
        p=self.payload();broken={**p,'table_blank':False}
        self.assertEqual(self.op('POST','factory/prepare',broken).status_code,409)
        self.prepare(p)
        result=self.prepare({**broken,'request_id':str(uuid4()),'factory_blank':False})
        self.assertTrue(result['recovery_allowed'])
        self.assertEqual(self.op('POST','factory/prepare',{**broken,'request_id':str(uuid4()),'operation':'identity'}).status_code,400)

if __name__=='__main__':main()
