import json
import os
import unittest
from uuid import uuid4
from cryptography.fernet import Fernet
import test_esp32 as fixture
from database import SessionLocal, Esp32Hub
from operations_models import Asset, OperationsEvent
from operations_api import EOL_CHECKS
os.environ['AMTECH_MANAGEMENT_KEY']='operations-fixture-key-32-bytes-minimum'
os.environ['AMTECH_LABEL_ENCRYPTION_KEY']=Fernet.generate_key().decode()

class OperationsTests(unittest.TestCase):
    setUp=fixture.Esp32Tests.setUp
    sync=fixture.Esp32Tests.sync
    def op(self,method,path,body=None,key=True):
        return self.client.open('/ops/'+path,method=method,json=body,headers={'X-AMTECH-OPERATIONS-KEY':os.environ['AMTECH_MANAGEMENT_KEY']} if key else {})
    def batch(self,kind='hub_base',count=1,key=None):
        r=self.op('POST','batches',dict(kind=kind,count=count,period='2610',request_id=key or str(uuid4())))
        self.assertIn(r.status_code,(200,201),r.json)
        return r.json['batch_id']
    def labels(self,batch):
        r=self.op('GET',f'batches/{batch}/labels');self.assertEqual(r.status_code,200,r.json);return r.json['labels']
    def ready(self):
        label=self.labels(self.batch())[0];serial=label['serial']
        r=self.op('POST',f'assets/{serial}/provision',{'hardware_id':'aabbccddeeff'})
        self.assertEqual(r.status_code,200,r.json)
        device={'X-AMTECH-DEVICE-SERIAL':serial,'X-AMTECH-DEVICE-TOKEN':r.json['device_token']}
        self.assertEqual(self.client.post('/esp32/device/sync',headers=device,json={'state':{**fixture.STATE,'hardware_id':'aabbccddeeff','boot_id':'boot-a','alert_outbox_count':0}}).status_code,200)
        tested=self.op('POST',f'assets/{serial}/test',{'checks':dict.fromkeys(EOL_CHECKS,True)})
        self.assertEqual(tested.status_code,200,tested.json)
        return label
    def order(self,expected=None):
        r=self.op('POST','orders',{'request_id':str(uuid4()),'expected':expected or {'camera':1,'shutter':2,'smoke':1}})
        self.assertEqual(r.status_code,200,r.json);return r.json['order']['id']
    def scan(self,order,label,kind):return self.op('POST',f'orders/{order}/scan',{'scan':label['qr'],'kind':kind})
    def complete(self):
        hub=self.ready();order=self.order();self.assertEqual(self.scan(order,hub,'hub').status_code,200)
        for kind,count in [('camera',1),('shutter',2),('smoke',1)]:
            for label in self.labels(self.batch(kind,count)):self.assertEqual(self.scan(order,label,kind).status_code,200)
        self.assertEqual(self.op('POST',f'orders/{order}/pack',{}).status_code,200)
        return hub,order
    def pair(self,label,auth=None,**location):return self.client.post('/esp32/pair',headers=auth or self.auth,json={**self.pair_data,**json.loads(label['qr']),**location})
    def test_management_key_and_secret_exclusion(self):
        self.assertEqual(self.op('GET','overview',key=False).status_code,401)
        self.batch();data=self.op('GET','assets').get_data(as_text=True)
        for name in ['pairing_code','device_token','claim_cipher','token_cipher']:self.assertNotIn(name,data)
        self.assertEqual(self.op('GET','overview').status_code,200)
    def test_serial_reservation_and_retry(self):
        key=str(uuid4());batch=self.batch(count=2,key=key);self.assertEqual(self.batch(count=2,key=key),batch)
        labels=self.labels(batch);self.assertEqual([x['serial'] for x in labels],['NET-B-2610-00001','NET-B-2610-00002'])
        self.assertEqual(self.labels(self.batch())[0]['serial'],'NET-B-2610-00003')
        self.assertNotEqual(json.loads(labels[0]['qr'])['pairing_code'],json.loads(labels[1]['qr'])['pairing_code'])
    def test_invalid_batch_and_scan(self):
        for p in [dict(kind='camera',count=True,period='2610'),dict(kind='camera',count=51,period='2610'),dict(kind='camera',count=1,period='2613')]:
            self.assertEqual(self.op('POST','batches',{**p,'request_id':str(uuid4())}).status_code,400)
        order=self.order()
        for raw in [None,'../secret','{}','https://example.com','x'*2049]:self.assertEqual(self.op('POST',f'orders/{order}/scan',{'scan':raw,'kind':'hub'}).status_code,400)
    def test_board_binding_and_provision_retry(self):
        label=self.labels(self.batch())[0];path=f"assets/{label['serial']}/provision"
        first=self.op('POST',path,{'hardware_id':'aabbccddeeff'});again=self.op('POST',path,{'hardware_id':'aabbccddeeff'})
        self.assertEqual(first.json['device_token'],again.json['device_token'])
        self.assertEqual(self.op('POST',path,{'hardware_id':'001122334455'}).status_code,409)
        second=self.labels(self.batch())[0]
        self.assertEqual(self.op('POST',f"assets/{second['serial']}/provision",{'hardware_id':'aabbccddeeff'}).status_code,409)
    def test_eol_and_claim_are_gated(self):
        hub=self.labels(self.batch())[0];order=self.order()
        self.assertEqual(self.scan(order,hub,'hub').status_code,409)
        self.assertEqual(self.op('POST',f"assets/{hub['serial']}/test",{'checks':dict.fromkeys(EOL_CHECKS,True)}).status_code,409)
        self.assertEqual(self.pair(hub).status_code,409)
        with SessionLocal() as db:self.assertIsNone(db.get(Esp32Hub,hub['serial']).shop_id)
    def test_hub_first_item_type_and_duplicate(self):
        hub=self.ready();order=self.order();cam=self.labels(self.batch('camera'))[0]
        self.assertEqual(self.scan(order,cam,'camera').status_code,409)
        self.assertEqual(self.scan(order,hub,'hub').status_code,200)
        self.assertEqual(self.scan(order,cam,'shutter').status_code,400)
        self.assertEqual(self.scan(order,cam,'camera').status_code,200)
        duplicate=self.scan(order,cam,'camera');self.assertTrue(duplicate.json['duplicate']);self.assertEqual(duplicate.json['order']['counts']['camera'],1)
        self.assertEqual(self.scan(order,self.labels(self.batch('camera'))[0],'camera').status_code,409)
        self.assertEqual(self.op('POST',f'orders/{order}/pack',{}).status_code,409)
    def test_capacity_limits(self):
        hub=self.ready();order=self.order({'camera':1,'shutter':3,'smoke':1})
        self.assertEqual(self.scan(order,hub,'hub').status_code,409)
        self.assertEqual(self.scan(self.order(),self.labels(self.batch('hub_high'))[0],'hub').status_code,409)
    def test_delete_empty_draft_is_idempotent_and_requires_management_access(self):
        order=self.order()
        self.assertEqual(self.op('DELETE',f'orders/{order}',key=False).status_code,401)
        self.assertEqual(self.op('GET',f'orders/{order}').status_code,200)
        deleted=self.op('DELETE',f'orders/{order}')
        self.assertEqual(deleted.status_code,200);self.assertTrue(deleted.json['deleted'])
        self.assertEqual(deleted.json['released_count'],0)
        self.assertEqual(self.op('GET',f'orders/{order}').status_code,404)
        self.assertFalse(self.op('DELETE',f'orders/{order}').json['deleted'])
        self.assertNotIn(order,[o['id'] for o in self.op('GET','overview').json['orders']])
        with SessionLocal() as db:self.assertEqual(db.query(OperationsEvent).filter_by(kind='draft_deleted').count(),1)
    def test_delete_partial_draft_releases_hub_and_accessories_without_deleting_inventory(self):
        hub=self.ready();order=self.order();camera=self.labels(self.batch('camera'))[0]
        self.scan(order,hub,'hub');self.scan(order,camera,'camera')
        deleted=self.op('DELETE',f'orders/{order}')
        self.assertEqual(deleted.json['released_count'],2)
        with SessionLocal() as db:
            self.assertIsNone(db.get(Asset,hub['serial']).order_id)
            self.assertIsNone(db.get(Asset,camera['serial']).order_id)
            self.assertEqual(db.get(Asset,hub['serial']).test_status,'passed')
            self.assertIsNotNone(db.get(Asset,hub['serial']).claim_cipher)
            self.assertIsNotNone(db.get(Esp32Hub,hub['serial']))
        replacement=self.order()
        self.assertEqual(self.scan(replacement,hub,'hub').status_code,200)
        self.assertEqual(self.scan(replacement,camera,'camera').status_code,200)
    def test_confirmed_and_dispatched_packages_cannot_be_deleted(self):
        hub,order=self.complete()
        self.assertEqual(self.op('DELETE',f'orders/{order}').status_code,409)
        self.assertEqual(len(self.op('GET',f'orders/{order}').json['order']['items']),5)
        self.assertEqual(self.op('POST',f'orders/{order}/dispatch',{}).status_code,200)
        self.assertEqual(self.op('DELETE',f'orders/{order}').status_code,409)
        self.assertEqual(self.op('GET',f'orders/{order}').json['order']['status'],'dispatched')
    def test_draft_removal_and_cross_order_isolation(self):
        hub=self.ready();order=self.order();cam=self.labels(self.batch('camera'))[0];self.scan(order,hub,'hub');self.scan(order,cam,'camera');other=self.order()
        self.assertEqual(self.scan(other,cam,'camera').status_code,409)
        self.assertEqual(self.op('DELETE',f"orders/{order}/items/{hub['serial']}").status_code,409)
        self.assertEqual(self.op('DELETE',f"orders/{order}/items/{cam['serial']}").status_code,200)
        self.assertEqual(self.op('DELETE',f"orders/{order}/items/{hub['serial']}").status_code,200)
        self.assertEqual(self.scan(other,hub,'hub').status_code,200);self.assertEqual(self.scan(other,cam,'camera').status_code,200)
    def test_full_pack_claim_manifest_location_and_dispatch(self):
        hub,order=self.complete();claim=self.pair(hub,latitude=18.52,longitude=73.85)
        self.assertEqual(claim.status_code,201,claim.json);self.assertEqual(len(claim.json['package']['items']),4)
        pack=claim.json['package'];self.assertNotIn('pairing_code',json.dumps(pack));self.assertNotIn('device_token',json.dumps(pack))
        self.assertEqual(self.pair(hub).json['shop_id'],claim.json['shop_id'])
        self.assertEqual(self.client.get('/shop/'+claim.json['shop_id']+'/package',headers=self.auth).json['package'],pack)
        installed=next(h for h in self.op('GET','overview').json['fleet'] if h['serial']==hub['serial']);self.assertEqual(installed['latitude'],18.52)
        self.assertEqual(self.op('DELETE',f"orders/{order}/items/{hub['serial']}").status_code,409)
        self.assertEqual(self.op('POST',f'orders/{order}/dispatch',{}).status_code,200)
        self.assertEqual(self.op('POST',f'orders/{order}/dispatch',{}).status_code,200)
        with SessionLocal() as db:batch=db.get(Asset,hub['serial']).batch_id
        self.assertEqual(self.labels(batch),[])
    def test_customer_isolation(self):
        hub,_=self.complete();claimed=self.pair(hub)
        other=self.client.post('/auth/signup',json=dict(email='other-packer@example.com',password='correct-horse-battery-staple',phone_number='+918550991122'))
        auth={'Authorization':'Bearer '+other.json['token']};self.assertEqual(self.pair(hub,auth).status_code,409)
        path='/shop/'+claimed.json['shop_id']+'/package';self.assertIn(self.client.get(path,headers=auth).status_code,(403,404));self.assertEqual(self.client.get(path).status_code,401)
    def test_location_validation_and_legacy_package(self):
        hub,_=self.complete()
        for loc in [dict(latitude=True,longitude=10),dict(latitude=91,longitude=10),dict(latitude=10),dict(latitude=float('nan'),longitude=10)]:self.assertEqual(self.pair(hub,**loc).status_code,400)
        self.assertFalse(self.client.get(f'/shop/{self.shop_id}/package',headers=self.auth).json['package']['recorded'])
    def test_observation_logs_changes_not_every_heartbeat(self):
        state={**fixture.STATE,'boot_id':'boot-one','uptime_ms':1};self.sync(state)
        with SessionLocal() as db:before=db.query(OperationsEvent).count()
        self.sync({**state,'uptime_ms':999})
        with SessionLocal() as db:self.assertEqual(db.query(OperationsEvent).count(),before)
        self.sync({**state,'boot_id':'boot-two'})
        with SessionLocal() as db:self.assertEqual(db.query(OperationsEvent).count(),before+1)
        self.assertEqual(self.op('GET','logs').status_code,200)
    def test_manufacturing_record_is_saved_and_retry_cannot_change_it(self):
        key=str(uuid4());body=dict(kind='camera',count=1,period='2610',request_id=key,manufacturing_batch='SAAKETH-LOT-12',manufactured_on='2026-10-20')
        self.assertEqual(self.op('POST','batches',body).status_code,201)
        asset=self.op('GET','assets').json['assets'][0]
        self.assertEqual(asset['manufacturing_batch'],'SAAKETH-LOT-12');self.assertEqual(asset['manufactured_on'],'2026-10-20')
        self.assertEqual(self.op('POST','batches',{**body,'manufacturing_batch':'another-lot'}).status_code,400)
        self.assertEqual(self.op('POST','batches',{**body,'request_id':str(uuid4()),'manufactured_on':'2026-02-31'}).status_code,400)

    def test_factory_events_are_not_replayed_into_customer_alarms(self):
        from database import Alert
        hub=self.ready();serial=hub['serial']
        credentials=self.op('POST',f'assets/{serial}/provision',{'hardware_id':'aabbccddeeff'})
        # Released credentials cannot be fetched again. Use a separately prepared
        # fixture token hash only for this isolated device transport test.
        from esp32_api import secret_hash
        with SessionLocal() as db:db.get(Esp32Hub,serial).token_hash=secret_hash('factory-fixture-token');db.commit()
        headers={'X-AMTECH-DEVICE-SERIAL':serial,'X-AMTECH-DEVICE-TOKEN':'factory-fixture-token'}
        event={'id':'eol-smoke-one','event_type':'smoke'}
        state={**fixture.STATE,'hardware_id':'aabbccddeeff','alert_outbox_count':1}
        for _ in range(2):
            r=self.client.post('/esp32/device/sync',headers=headers,json={'state':state,'events':[event,event]})
            self.assertEqual(r.status_code,200);self.assertEqual(r.json['accepted_event_ids'],[event['id'],event['id']])
        with SessionLocal() as db:self.assertEqual(db.query(OperationsEvent).filter_by(kind='factory_smoke').count(),1)
        # No release while the device still reports undrained factory history.
        self.assertEqual(self.op('POST',f'assets/{serial}/test',{'checks':dict.fromkeys(EOL_CHECKS,True)}).status_code,409)
        state['alert_outbox_count']=0
        self.client.post('/esp32/device/sync',headers=headers,json={'state':state})
        self.assertEqual(self.op('POST',f'assets/{serial}/test',{'checks':dict.fromkeys(EOL_CHECKS,True)}).status_code,200)
        order=self.order({'camera':0,'shutter':0,'smoke':0});self.scan(order,hub,'hub');self.op('POST',f'orders/{order}/pack',{})
        paired=self.pair(hub);self.assertEqual(paired.status_code,201)
        # A lost acknowledgement retried after claiming is still a factory event.
        self.client.post('/esp32/device/sync',headers=headers,json={'state':state,'events':[event]})
        with SessionLocal() as db:self.assertEqual(db.query(Alert).filter_by(shop_id=paired.json['shop_id']).count(),0)

if __name__=='__main__':unittest.main()
