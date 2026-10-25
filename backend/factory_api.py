"""Private browser-station transport. Master keys never enter the USB client."""
import base64
import csv
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from datetime import timezone
from flask import jsonify, request
from database import SessionLocal, Esp32Hub
from operations_models import Asset, FactoryRun
from operations_api import audit, iso, now, ProvisionConflict

OFFSET, SIZE = 0x812000, 0x6000
MANIFEST_PATH = Path(__file__).parent / 'factory_releases' / 'approved.json'

def release_manifest():
    if not MANIFEST_PATH.exists():
        raise ValueError('No factory firmware release is approved')
    return json.loads(MANIFEST_PATH.read_text())

def identity_image(identity):
    with tempfile.TemporaryDirectory(prefix='nettra-usb-') as tmp:
        directory=Path(tmp);directory.chmod(0o700)
        source,output=directory/'identity.csv',directory/'factory.bin'
        with source.open('w',newline='') as f:
            writer=csv.writer(f);writer.writerow(['key','type','encoding','value'])
            writer.writerow(['identity','namespace','',''])
            for key,value in [('serial',identity['device_serial']),('token',identity['device_token']),('backend',identity['backend_url']),('hardware',identity['hardware_id'])]:
                writer.writerow([key,'data','string',value])
        source.chmod(0o600)
        result=subprocess.run([sys.executable,'-m','esp_idf_nvs_partition_gen','generate',str(source),str(output),hex(SIZE)],capture_output=True,timeout=15)
        if result.returncode or not output.exists():
            raise ValueError('Identity image generation failed. Reconnect and retry.')
        output.chmod(0o600);data=output.read_bytes()
        if len(data)!=SIZE:raise ValueError('Identity image has an invalid size')
        return data

def run_dict(db,run):
    hub=db.get(Esp32Hub,run.serial);state=(hub.reported_state or {})if hub else {}
    seen=hub.last_seen_at if hub else None
    if seen and seen.tzinfo is None:seen=seen.replace(tzinfo=timezone.utc)
    started=run.created_at.replace(tzinfo=timezone.utc)if run.created_at.tzinfo is None else run.created_at
    recent=bool(seen and 0 <= (now()-seen).total_seconds()<45)
    after=bool(seen and seen>=started)
    expected=release_manifest()['firmware_version']
    connected=bool(recent and after and state.get('hardware_id')==run.hardware_id and state.get('firmware_version')==expected)
    asset=db.get(Asset,run.serial)
    eligible=bool(hub and not hub.shop_id and asset and not asset.order_id)
    return dict(id=run.id,serial=run.serial,hardware_id=run.hardware_id,release_id=run.release_id,
                operation=run.operation,status=run.status,created_at=iso(run.created_at),verified_at=iso(run.verified_at),
                cloud_verified=connected,ready_for_eol=connected and eligible and run.status=='verified',last_seen_at=iso(seen))

def register_factory_routes(app,private,issue_identity):
    @app.get('/ops/factory/release')
    @private
    def release():
        return jsonify(ok=True,release=release_manifest())

    @app.post('/ops/factory/prepare')
    @private
    def prepare():
        p=request.get_json();release=release_manifest()
        serial,hardware,identity=p.get('serial'),p.get('hardware_id'),p.get('request_id')
        operation=p.get('operation')
        if not isinstance(serial,str) or not re.fullmatch(r'NET-B-\d{4}-\d{5}',serial):raise ValueError('Scan a registered Base hub sticker')
        if not isinstance(hardware,str) or not re.fullmatch(r'[0-9a-f]{12}',hardware):raise ValueError('Read the connected board identity first')
        if not isinstance(identity,str) or not re.fullmatch(r'[A-Za-z0-9_-]{16,64}',identity):raise ValueError('A stable station request ID is required')
        if operation not in ('identity','firmware') or p.get('release_id')!=release['id']:raise ValueError('Select the currently approved release')
        if p.get('confirmed') is not True:raise ValueError('Confirm the uninstalled board and its sticker')
        if p.get('chip')!='ESP32-S3' or p.get('flash_size')!=16*1024*1024 or p.get('security_clear') is not True:raise ValueError('Only verified, unencrypted ESP32-S3 16MB factory boards are supported')
        if type(p.get('factory_blank'))is not bool:raise ValueError('Read the factory partition before continuing')
        if type(p.get('table_valid'))is not bool or type(p.get('table_blank'))is not bool:raise ValueError('Read the partition table before continuing')
        with SessionLocal() as db:
            existing=db.query(FactoryRun).filter_by(id=identity).with_for_update().first()
            if existing and (existing.serial,existing.hardware_id,existing.release_id,existing.operation)!=(serial,hardware,release['id'],operation):raise ValueError('This request belongs to another preparation')
            if existing and existing.status=='verified':raise ProvisionConflict('This preparation already completed. Check its cloud connection.')
            asset=db.get(Asset,serial)
            previous=db.query(FactoryRun).filter_by(serial=serial,hardware_id=hardware,release_id=release['id'],operation='firmware',status='issued').first()
            recovery=bool(asset and asset.hardware_id==hardware and previous)
            if not p['table_valid'] and not p['table_blank'] and not recovery:raise ProvisionConflict('Unrecognized partition layout. Only a previously interrupted AMTECH preparation can be recovered here.')
            if operation=='identity' and not p['table_valid']:raise ValueError('Install the approved firmware first')
            if not p['factory_blank'] and (not asset or asset.hardware_id!=hardware):raise ProvisionConflict('This board already has a factory identity. Use its original serial or a service procedure.')
            credentials=issue_identity(db,serial,{'hardware_id':hardware,'pcb_revision':p.get('pcb_revision'),'firmware_version':release['firmware_version']})
            image=identity_image(credentials);digest=hashlib.sha256(image).hexdigest()
            if existing:
                if existing.image_sha256!=digest:raise ValueError('Identity changed; stop and contact AMTECH')
                run=existing
            else:
                run=FactoryRun(id=identity,serial=serial,hardware_id=hardware,release_id=release['id'],operation=operation,image_sha256=digest)
                db.add(run)
                audit(db,'usb_prepare_started','USB identity prepared for its verified board',serial,{'run_id':identity,'release_id':release['id'],'operation':operation})
            db.commit()
            return jsonify(ok=True,run=run_dict(db,run),recovery_allowed=recovery,image={'offset':OFFSET,'size':SIZE,'sha256':digest,'data':base64.b64encode(image).decode()})

    @app.post('/ops/factory/runs/<run_id>/verify')
    @private
    def verify(run_id):
        p=request.get_json()
        with SessionLocal() as db:
            run=db.query(FactoryRun).filter_by(id=run_id).with_for_update().first()
            if not run:return jsonify(ok=False,error='USB preparation not found'),404
            if p.get('hardware_id')!=run.hardware_id or p.get('sha256')!=run.image_sha256 or p.get('release_id')!=run.release_id:raise ValueError('USB readback does not match this preparation')
            if run.status!='verified':
                run.status='verified';run.verified_at=now()
                audit(db,'usb_verified','USB identity readback verified',run.serial,{'run_id':run.id,'release_id':run.release_id})
                db.commit()
            return jsonify(ok=True,run=run_dict(db,run))

    @app.get('/ops/factory/runs/<run_id>')
    @private
    def status(run_id):
        with SessionLocal() as db:
            run=db.get(FactoryRun,run_id)
            return jsonify(ok=True,run=run_dict(db,run))if run else(jsonify(ok=False,error='USB preparation not found'),404)
