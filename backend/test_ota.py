import hashlib,io,json,os,struct,tempfile,unittest
from datetime import timedelta
from pathlib import Path
from unittest.mock import patch
from cryptography.fernet import Fernet
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.hazmat.primitives import serialization
import espsecure
import test_esp32 as fixture
from database import SessionLocal,Esp32Hub,Esp32Command
from ota_api import OtaRelease,OtaDeployment,OtaCapability,now

os.environ['AMTECH_MANAGEMENT_KEY']='ota-tests-operations-key-32-bytes-long'
os.environ['R2_BUCKET_NAME']='fixture-only'
os.environ['AMTECH_LABEL_ENCRYPTION_KEY']=Fernet.generate_key().decode()
REPORT=dict(protocol=1,storage_schema=1,rollback_enabled=True,signature_required=True,running_slot='ota_0',deployment_id='',version='',status='idle',reason='',received=0,size=0)
STATE={**fixture.STATE,'hardware_id':'aabbccddeeff','firmware_version':'baseline-v1','boot_id':'boot-one','alert_outbox_count':0}

class OtaTests(unittest.TestCase):
    def setUp(self):
        self.initializing=True;fixture.Esp32Tests.setUp(self);self.initializing=False
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();cls.root=Path(cls.tmp.name)
        key=rsa.generate_private_key(public_exponent=65537,key_size=3072)
        cls.private=cls.root/'key.pem';cls.private.write_bytes(key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
        cls.public=cls.root/'public.pem';cls.public.write_bytes(key.public_key().public_bytes(serialization.Encoding.PEM,serialization.PublicFormat.SubjectPublicKeyInfo))
        image=bytearray(8192);image[0]=0xe9;image[12:14]=(9).to_bytes(2,'little');image[32:36]=(0xabcd5432).to_bytes(4,'little');image[48:51]=b'v2\0'
        output=cls.root/'signed.bin'
        datafile=io.BytesIO(image);datafile.name='fixture.bin'
        with cls.private.open('rb') as private:espsecure.sign_data("2",[private],str(output),False,False,None,[],[],datafile)
        cls.image=output.read_bytes()
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def sync(self,state=None,report=REPORT,**kwargs):
        if self.initializing:return fixture.Esp32Tests.sync(self)
        return self.client.post('/esp32/device/sync',headers=self.device,json=dict(state=state or STATE,ota=report,**kwargs))
    def op(self,method,path,body=None,key=True):return self.client.open('/ops/ota/'+path,method=method,json=body,headers={'X-AMTECH-OPERATIONS-KEY':os.environ['AMTECH_MANAGEMENT_KEY']} if key else {})
    def release(self,identity='release-one',image=None,approved=True):
        image=image or self.image
        m=dict(id=identity,version='v2',target_serial='ESP32-TEST',sha256=hashlib.sha256(image).hexdigest(),size=len(image),source_commit='a'*40,protocol=1,storage_schema=1)
        r=self.op('POST','releases',m);self.assertIn(r.status_code,(200,201),r.json)
        if approved:
            with patch('ota_api.TRUST_KEY',self.public),patch('app.r2_client') as storage:
                r=self.client.put('/ops/ota/releases/'+identity+'/image',headers={'X-AMTECH-OPERATIONS-KEY':os.environ['AMTECH_MANAGEMENT_KEY']},data=image,content_type='application/octet-stream')
                self.assertEqual(r.status_code,200,r.json)
                storage.return_value.put_object.assert_called_once()
                stored=storage.return_value.put_object.call_args.kwargs['Body'];self.assertNotEqual(stored,image)
                self.assertNotIn(image[:128],stored)
        return m
    def schedule(self,identity='job-one'):
        return self.op('POST','deployments',dict(serial='ESP32-TEST',release_id='release-one',request_id=identity,confirmed=True))
    def test_admin_only_signature_verification_and_immutable_release(self):
        self.assertEqual(self.op('GET','releases',key=False).status_code,401)
        self.assertEqual(self.client.post('/ops/ota/releases',headers=self.auth,json={}).status_code,401)
        m=self.release();m['sha256']='b'*64
        self.assertEqual(self.op('POST','releases',m).status_code,400)
        self.assertEqual(self.client.get('/esp32/device/ota/job/image').status_code,401)
    def test_corrupt_and_unsigned_firmware_are_not_approved(self):
        damaged=bytearray(self.image);damaged[100]^=1
        self.release(image=damaged,approved=False)
        with patch('ota_api.TRUST_KEY',self.public),patch('app.r2_client') as storage:
            r=self.client.put('/ops/ota/releases/release-one/image',headers={'X-AMTECH-OPERATIONS-KEY':os.environ['AMTECH_MANAGEMENT_KEY']},data=bytes(damaged))
            self.assertEqual(r.status_code,400);self.assertIn('signature',r.json['error']);storage.assert_not_called()
        with SessionLocal() as db:self.assertEqual(db.get(OtaRelease,'release-one').status,'draft')
    def test_usb_baseline_required_and_all_admission_guards(self):
        self.release();self.assertEqual(self.schedule().status_code,400)
        self.sync();self.assertEqual(self.schedule().status_code,201)
        self.assertEqual(self.schedule().status_code,200)
        self.assertEqual(self.schedule('other-job').status_code,400)
        self.op('POST','deployments/job-one/cancel',{})
        for changed in [dict(arm_state='armed'),dict(siren_active=True),dict(smoke='alarm'),dict(alert_outbox_count=1),dict(hardware_id='001122334455',firmware_version='legacy')]:
            self.sync({**STATE,**changed},report=None if 'firmware_version'in changed else REPORT)
            self.assertEqual(self.schedule('job-guard').status_code,400,changed)
    def test_successful_lifecycle_device_scope_and_fresh_confirmation(self):
        self.release();self.sync();self.assertEqual(self.schedule().status_code,201)
        offer=self.sync().json['ota'];self.assertEqual(offer['id'],'job-one');self.assertNotIn('url',offer)
        report={**REPORT,'deployment_id':'job-one','version':'v2','status':'downloading','received':4096,'size':len(self.image)}
        self.sync(report=report)
        with patch('app.r2_client') as storage:
            body=storage.return_value.get_object.return_value['Body'];body.read.return_value=Fernet(os.environ['AMTECH_LABEL_ENCRYPTION_KEY'].encode()).encrypt(b'NETTRA-OTA1\0'+hashlib.sha256(self.image).hexdigest().encode()+b'\0'+self.image)
            r=self.client.get('/esp32/device/ota/job-one/image',headers=self.device)
            self.assertEqual(r.status_code,200);self.assertEqual(r.data,self.image);body.close.assert_called_once()
        bad=self.sync(report={**report,'status':'confirmed','received':len(self.image)})
        self.assertEqual(bad.status_code,400)
        good=self.sync({**STATE,'firmware_version':'v2','boot_id':'boot-two'},report={**report,'status':'confirmed','received':len(self.image)})
        self.assertEqual(good.status_code,200);self.assertIsNone(good.json['ota'])
        self.assertEqual(self.op('GET','deployments').json['deployments'][0]['status'],'confirmed')
    def test_cancellation_revocation_and_expiry_stop_download_authorization(self):
        self.release();self.sync();self.schedule();self.sync()
        self.op('POST','deployments/job-one/cancel',{})
        self.assertIsNone(self.sync().json['ota']);self.assertEqual(self.client.get('/esp32/device/ota/job-one/image',headers=self.device).status_code,409)
        self.schedule('job-two');self.op('POST','releases/release-one/revoke',{})
        self.assertIsNone(self.sync().json['ota'])
        with SessionLocal() as db:r=db.get(OtaRelease,'release-one');r.status='approved';db.commit()
        self.schedule('job-three')
        with SessionLocal() as db:r=db.get(OtaDeployment,'job-three');r.expires_at=now()-timedelta(seconds=1);db.commit()
        self.assertIsNone(self.sync().json['ota'])
        self.assertEqual(self.op('GET','deployments').json['deployments'][0]['status'],'expired')
    def test_alarm_priority_capability_loss_and_malformed_reports(self):
        self.release();self.sync();self.schedule()
        with SessionLocal() as db:db.add(Esp32Command(id='arm-new',serial='ESP32-TEST',name='arm',status='queued',created_at=now(),expires_at=now()+timedelta(seconds=120)));db.commit()
        self.assertIsNone(self.sync().json['ota'])
        for report in [{**REPORT,'signature_required':False},{**REPORT,'received':-1},{**REPORT,'running_slot':'factory'},{**REPORT,'status':'anything'}]:self.assertEqual(self.sync(report=report).status_code,400)
    def test_cross_device_report_cannot_complete_another_update(self):
        self.release();self.sync();self.schedule()
        with SessionLocal() as db:
            from esp32_api import secret_hash
            db.add(Esp32Hub(serial='OTHER',token_hash=secret_hash('other-secret'),pairing_code_hash='x'));db.commit()
        r=self.client.post('/esp32/device/sync',headers={'X-AMTECH-DEVICE-SERIAL':'OTHER','X-AMTECH-DEVICE-TOKEN':'other-secret'},json={'state':STATE,'ota':{**REPORT,'deployment_id':'job-one','version':'v2','status':'confirmed'}})
        self.assertEqual(r.status_code,200)
        with SessionLocal() as db:self.assertEqual(db.get(OtaDeployment,'job-one').status,'queued')

if __name__=='__main__':unittest.main()
