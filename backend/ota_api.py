"""Admin-approved, device-authenticated application-only OTA. No customer write routes."""
import hashlib
import hmac
import io
import os
import re
from contextlib import redirect_stdout
from datetime import datetime, timedelta, timezone
from pathlib import Path
from uuid import uuid4

from flask import Response, jsonify, request, stream_with_context
from sqlalchemy import Column, DateTime, ForeignKey, Integer, JSON, String, UniqueConstraint
from database import Base, Esp32Command, Esp32Hub, SessionLocal

MAX_IMAGE = 0x400000
ACTIVE = ('queued', 'offered', 'downloading', 'staged', 'rebooting', 'confirming')
TERMINAL = ('confirmed', 'failed', 'cancelled', 'interrupted', 'rolled_back', 'expired')
TRUST_KEY = Path(__file__).parent/'ota_trust'/'signing-public.pem'
IDENTIFIER = re.compile(r'^[A-Za-z0-9_.-]{1,64}$')

def now(): return datetime.now(timezone.utc)
def utc(value): return value.replace(tzinfo=timezone.utc) if value.tzinfo is None else value

class OtaRelease(Base):
    __tablename__ = 'esp32_ota_releases'
    __table_args__ = (UniqueConstraint('target_serial','version',name='uq_ota_target_version'),)
    id = Column(String(64), primary_key=True)
    version = Column(String(31), nullable=False)
    target_serial = Column(String(128), nullable=False)
    sha256 = Column(String(64), nullable=False)
    size = Column(Integer, nullable=False)
    source_commit = Column(String(40), nullable=False)
    object_key = Column(String(255), nullable=False)
    status = Column(String(16), nullable=False, default='draft')
    created_at = Column(DateTime(timezone=True), nullable=False, default=now)

class OtaDeployment(Base):
    __tablename__ = 'esp32_ota_deployments'
    id = Column(String(64), primary_key=True)
    serial = Column(String(128), ForeignKey('esp32_hubs.serial'), nullable=False, index=True)
    release_id = Column(String(64), ForeignKey('esp32_ota_releases.id'), nullable=False)
    hardware_id = Column(String(12), nullable=False)
    from_version = Column(String(128), nullable=False)
    status = Column(String(24), nullable=False, default='queued')
    received = Column(Integer, nullable=False, default=0)
    reason = Column(String(512), nullable=True)
    last_report = Column(JSON, nullable=True)
    created_at = Column(DateTime(timezone=True), nullable=False, default=now)
    expires_at = Column(DateTime(timezone=True), nullable=False)
    completed_at = Column(DateTime(timezone=True), nullable=True)


def release_dict(row):
    return dict(id=row.id, version=row.version, target_serial=row.target_serial, sha256=row.sha256, size=row.size,
                source_commit=row.source_commit, status=row.status, storage_schema=1, protocol=1)

def deployment_dict(row):
    return dict(id=row.id, serial=row.serial, release_id=row.release_id, hardware_id=row.hardware_id,
                from_version=row.from_version, status=row.status, received=row.received,
                reason=row.reason, created_at=utc(row.created_at).isoformat(),
                expires_at=utc(row.expires_at).isoformat(), last_report=row.last_report)

def validate_report(value):
    if value is None: return None
    if not isinstance(value, dict): raise ValueError('Invalid OTA report')
    if value.get('protocol') != 1 or value.get('storage_schema') != 1: raise ValueError('Unsupported OTA protocol')
    if value.get('rollback_enabled') is not True or value.get('signature_required') is not True:
        raise ValueError('OTA safeguards not enabled')
    out = {key: value[key] for key in ('protocol', 'storage_schema', 'rollback_enabled', 'signature_required')}
    for key, limit in [('deployment_id',64),('version',31),('reason',128),('running_slot',16),('status',24)]:
        v=value.get(key,'')
        if not isinstance(v,str) or len(v)>limit: raise ValueError('Invalid OTA '+key)
        out[key]=v
    if out['deployment_id'] and not IDENTIFIER.fullmatch(out['deployment_id']): raise ValueError('Invalid OTA deployment')
    if out['status'] not in ACTIVE+TERMINAL+('idle',): raise ValueError('Invalid OTA status')
    if out['running_slot'] not in ('ota_0','ota_1'): raise ValueError('Unsupported OTA slot')
    for key in ('received','size'):
        v=value.get(key,0)
        if type(v) is not int or not 0<=v<=MAX_IMAGE: raise ValueError('Invalid OTA '+key)
        out[key]=v
    if out['received']>out['size']: raise ValueError('Invalid OTA progress')
    return out


def fresh_safe(hub):
    state=hub.reported_state or {}
    return bool(hub.last_seen_at and 0 <= (now()-utc(hub.last_seen_at)).total_seconds()<45
                and state.get('arm_state')=='disarmed' and state.get('siren_active') is False
                and state.get('smoke')=='normal' and state.get('alert_outbox_count')==0)


def sync_ota(db,hub,report):
    """Called under the existing hub row lock, in the heartbeat transaction."""
    if report:
        # Capability report is separate from changing sensor/boot audit snapshots.
        if report['deployment_id']:
            run=db.query(OtaDeployment).filter_by(id=report['deployment_id']).with_for_update().first()
            if run and run.serial==hub.serial and run.hardware_id==(hub.reported_state or {}).get('hardware_id'):
                release=db.get(OtaRelease,run.release_id)
                if run.status in ACTIVE:
                    status=report['status']
                    if status=='confirmed' and (report['version']!=release.version or
                         (hub.reported_state or {}).get('firmware_version')!=release.version):
                        raise ValueError('OTA confirmation contradicts firmware identity')
                    if status in ACTIVE+TERMINAL:
                        run.status=status;run.received=report['received'];run.reason=report['reason'] or None
                        run.last_report=report
                        if status in TERMINAL:run.completed_at=now()
        capability=db.get(OtaCapability,hub.serial)
        if not capability:capability=OtaCapability(serial=hub.serial);db.add(capability)
        stable={k:report[k] for k in ('protocol','storage_schema','rollback_enabled','signature_required','running_slot')}
        stable.update(firmware_version=(hub.reported_state or {}).get('firmware_version'),boot_id=(hub.reported_state or {}).get('boot_id'))
        if capability.report!=stable:capability.report=stable
    runs=db.query(OtaDeployment).filter(OtaDeployment.serial==hub.serial,OtaDeployment.status.in_(ACTIVE)).with_for_update().all()
    offer=None
    for run in runs:
        if run.status not in ACTIVE:continue
        release=db.get(OtaRelease,run.release_id)
        if utc(run.expires_at)<=now():run.status='expired';run.reason='Update authorization expired';run.completed_at=now();continue
        if not release or release.status!='approved':run.status='cancelled';run.reason='Release withdrawn';run.completed_at=now();continue
        if not report or not fresh_safe(hub) or (hub.reported_state or {}).get('hardware_id')!=run.hardware_id:continue
        command=db.query(Esp32Command.id).filter(Esp32Command.serial==hub.serial,Esp32Command.status.in_(('queued','dispatched'))).first()
        if command:continue
        if run.status=='queued':run.status='offered'
        offer=dict(id=run.id,version=release.version,sha256=release.sha256,size=release.size,storage_schema=1,
                   protocol=1,commit=report.get('status')=='staged' and run.status=='staged',
                   valid_for_ms=min(600000,max(0,int((utc(run.expires_at)-now()).total_seconds()*1000))))
    return offer

class OtaCapability(Base):
    __tablename__='esp32_ota_capabilities'
    serial=Column(String(128),ForeignKey('esp32_hubs.serial'),primary_key=True)
    report=Column(JSON,nullable=False)


def verify_image(image,release):
    if len(image)!=release.size or hashlib.sha256(image).hexdigest()!=release.sha256:
        raise ValueError('Image size or checksum mismatch')
    if image[0]!=0xe9 or int.from_bytes(image[12:14],'little')!=9 or int.from_bytes(image[32:36],'little')!=0xabcd5432:
        raise ValueError('An ESP32-S3 application image is required')
    version=image[48:80].split(b'\0',1)[0].decode('ascii')
    if version!=release.version:raise ValueError('Image version does not match release')
    import espsecure
    # The trust anchor is installed by the operator, never supplied by an upload.
    key=TRUST_KEY
    datafile=io.BytesIO(image);datafile.name='firmware.bin'
    with key.open('rb') as public,redirect_stdout(io.StringIO()):
        try:espsecure.verify_signature_v2(False,None,public,datafile)
        except Exception as exc:raise ValueError('Firmware signature verification failed') from exc


def register_ota_routes(app,private,storage_client):
    from operations_api import audit, cipher
    @app.get('/ops/ota/releases')
    @private
    def releases():
        with SessionLocal() as db:return jsonify(ok=True,releases=[release_dict(r) for r in db.query(OtaRelease).order_by(OtaRelease.created_at.desc()).limit(100)])

    @app.post('/ops/ota/releases')
    @private
    def create():
        p=request.get_json();identity=p.get('id');version=p.get('version');sha=p.get('sha256');size=p.get('size');commit=p.get('source_commit');target=p.get('target_serial')
        if not isinstance(identity,str) or not IDENTIFIER.fullmatch(identity):raise ValueError('Invalid release ID')
        if not isinstance(target,str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,128}',target):raise ValueError('A specific target hub is required')
        if not isinstance(version,str) or not re.fullmatch(r'[A-Za-z0-9_.-]{1,31}',version):raise ValueError('Invalid firmware version')
        if not isinstance(sha,str) or not re.fullmatch('[a-f0-9]{64}',sha):raise ValueError('Invalid SHA256')
        if type(size)is not int or not 8192<=size<=MAX_IMAGE or size%4096:raise ValueError('Invalid signed image size')
        if not isinstance(commit,str) or not re.fullmatch('[a-f0-9]{40}',commit):raise ValueError('Exact source commit required')
        if p.get('storage_schema')!=1 or p.get('protocol')!=1:raise ValueError('Unsupported storage schema or OTA protocol')
        with SessionLocal() as db:
            old=db.get(OtaRelease,identity)
            if old:
                if (old.version,old.sha256,old.size,old.source_commit,old.target_serial)!=(version,sha,size,commit,target):raise ValueError('Release IDs are immutable')
                return jsonify(ok=True,release=release_dict(old))
            # Versions cannot refer to different bytes: confirmation must identify one image.
            if db.query(OtaRelease.id).filter_by(version=version,target_serial=target).first():raise ValueError('Firmware version already exists')
            r=OtaRelease(id=identity,version=version,target_serial=target,sha256=sha,size=size,source_commit=commit,object_key='ota/releases/'+str(uuid4())+'.bin')
            db.add(r);audit(db,'ota_release_created','OTA release draft created',details={'release_id':identity,'sha256':sha});db.commit()
            return jsonify(ok=True,release=release_dict(r)),201

    @app.put('/ops/ota/releases/<release_id>/image')
    @private
    def upload(release_id):
        with SessionLocal() as db:
            release=db.query(OtaRelease).filter_by(id=release_id).with_for_update().first()
            if not release:return jsonify(ok=False,error='Release not found'),404
            if release.status!='draft':raise ValueError('Only draft releases accept an image')
            image=request.stream.read(MAX_IMAGE+1)
            verify_image(image,release)
            storage_client().put_object(Bucket=os.environ['R2_BUCKET_NAME'],Key=release.object_key,Body=cipher().encrypt(b'NETTRA-OTA1\0'+release.sha256.encode()+b'\0'+image),ContentType='application/octet-stream')
            release.status='approved';audit(db,'ota_release_approved','Signed OTA image verified and approved',details={'release_id':release.id,'sha256':release.sha256});db.commit()
            return jsonify(ok=True,release=release_dict(release))

    @app.post('/ops/ota/releases/<release_id>/revoke')
    @private
    def revoke(release_id):
        with SessionLocal() as db:
            r=db.query(OtaRelease).filter_by(id=release_id).with_for_update().first()
            if not r:return jsonify(ok=False,error='Release not found'),404
            r.status='revoked';audit(db,'ota_release_revoked','OTA release withdrawn',details={'release_id':r.id});db.commit()
            return jsonify(ok=True,release=release_dict(r))

    @app.get('/ops/ota/deployments')
    @private
    def deployments():
        with SessionLocal() as db:
            q=db.query(OtaDeployment)
            if request.args.get('serial'):q=q.filter_by(serial=request.args['serial'])
            serial=request.args.get('serial')
            cap=db.get(OtaCapability,serial) if serial else None
            hub=db.get(Esp32Hub,serial) if serial else None
            state=hub.reported_state or {} if hub else {}
            capable=bool(cap and cap.report.get('firmware_version')==state.get('firmware_version') and cap.report.get('boot_id')==state.get('boot_id'))
            return jsonify(ok=True,deployments=[deployment_dict(r) for r in q.order_by(OtaDeployment.created_at.desc()).limit(100)],
                           capable=capable,ready=bool(capable and hub and fresh_safe(hub)))

    @app.post('/ops/ota/deployments')
    @private
    def schedule():
        p=request.get_json();serial=p.get('serial');identity=p.get('request_id')
        if not isinstance(identity,str) or not IDENTIFIER.fullmatch(identity):raise ValueError('Stable request ID required')
        if not isinstance(serial,str) or not IDENTIFIER.fullmatch(serial):raise ValueError('Invalid hub serial')
        if p.get('confirmed') is not True:raise ValueError('Confirm the selected device and planned reboot')
        with SessionLocal() as db:
            hub=db.query(Esp32Hub).filter_by(serial=serial).with_for_update().first()
            if not hub:return jsonify(ok=False,error='Hub not found'),404
            release=db.get(OtaRelease,p.get('release_id'))
            if not release or release.status!='approved':raise ValueError('Select an approved signed release')
            if release.target_serial!=serial:raise ValueError('This release belongs to a different site hub')
            old=db.get(OtaDeployment,identity)
            if old:
                if old.serial!=serial or old.release_id!=release.id:raise ValueError('Request ID belongs to another update')
                return jsonify(ok=True,deployment=deployment_dict(old))
            cap=db.get(OtaCapability,serial)
            state=hub.reported_state or {}
            if not cap or cap.report.get('protocol')!=1 or cap.report.get('rollback_enabled') is not True or cap.report.get('signature_required') is not True or cap.report.get('firmware_version')!=state.get('firmware_version') or cap.report.get('boot_id')!=state.get('boot_id'):
                raise ValueError('Install and qualify the signed USB OTA baseline first')
            if not fresh_safe(hub):raise ValueError('Hub must be freshly online, disarmed, siren off, smoke normal and alert outbox empty')
            if not re.fullmatch('[a-f0-9]{12}',state.get('hardware_id','')):raise ValueError('Physical board identity required')
            if state.get('firmware_version')==release.version:raise ValueError('Hub already reports this firmware')
            for run in db.query(OtaDeployment).filter(OtaDeployment.serial==serial,OtaDeployment.status.in_(ACTIVE)).with_for_update().all():
                if utc(run.expires_at)<=now():run.status='expired';run.completed_at=now()
                else:raise ValueError('An OTA update is already active for this hub')
            if db.query(Esp32Command.id).filter(Esp32Command.serial==serial,Esp32Command.status.in_(('queued','dispatched'))).first():raise ValueError('Wait for the pending hub command')
            run=OtaDeployment(id=identity,serial=serial,release_id=release.id,hardware_id=state['hardware_id'],from_version=state['firmware_version'],expires_at=now()+timedelta(minutes=10))
            db.add(run);audit(db,'ota_scheduled','Admin approved a planned OTA update',serial,{'deployment_id':identity,'release_id':release.id});db.commit()
            return jsonify(ok=True,deployment=deployment_dict(run)),201

    @app.post('/ops/ota/deployments/<deployment_id>/cancel')
    @private
    def cancel(deployment_id):
        with SessionLocal() as db:
            run=db.query(OtaDeployment).filter_by(id=deployment_id).with_for_update().first()
            if not run:return jsonify(ok=False,error='Deployment not found'),404
            if run.status in ACTIVE:run.status='cancelled';run.reason='Cancelled by AMTECH';run.completed_at=now();audit(db,'ota_cancelled','OTA authorization cancelled',run.serial,{'deployment_id':run.id});db.commit()
            return jsonify(ok=True,deployment=deployment_dict(run))

    @app.get('/esp32/device/ota/<deployment_id>/image')
    def image(deployment_id):
        serial=request.headers.get('X-AMTECH-DEVICE-SERIAL','');token=request.headers.get('X-AMTECH-DEVICE-TOKEN','')
        if not token or len(token)>256:return jsonify(ok=False,error='Device authentication required'),401
        with SessionLocal() as db:
            hub=db.get(Esp32Hub,serial)
            if not hub or not hmac.compare_digest(hub.token_hash,hashlib.sha256(token.encode()).hexdigest()):return jsonify(ok=False,error='Invalid device credentials'),401
            run=db.get(OtaDeployment,deployment_id)
            if not run or run.serial!=serial:return jsonify(ok=False,error='Deployment not found'),404
            r=db.get(OtaRelease,run.release_id)
            if run.status not in ACTIVE or utc(run.expires_at)<=now() or r.status!='approved' or not fresh_safe(hub) or (hub.reported_state or {}).get('hardware_id')!=run.hardware_id:
                return jsonify(ok=False,error='OTA authorization inactive'),409
            key,size,expected=r.object_key,r.size,r.sha256
        body=storage_client().get_object(Bucket=os.environ['R2_BUCKET_NAME'],Key=key)['Body']
        try: encrypted=body.read(6*1024*1024)
        finally: body.close()
        try:
            plain=cipher().decrypt(encrypted)
            prefix=b'NETTRA-OTA1\0'+expected.encode()+b'\0'
            if not plain.startswith(prefix):raise ValueError()
            image=plain[len(prefix):]
            if len(image)!=size or hashlib.sha256(image).hexdigest()!=expected:raise ValueError()
        except Exception:
            return jsonify(ok=False,error='Stored firmware integrity check failed'),502
        return Response(image,headers={'Content-Length':str(size),'Cache-Control':'no-store'},content_type='application/octet-stream')
