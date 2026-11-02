"""Private incident evidence. Alarm ingestion only inserts jobs; it never contacts storage.

A separately enrolled recorder can only claim its hub's jobs. R2 keys never leave
this server, and signed reads are issued only after checking site ownership.
"""
import base64
import hashlib
import hmac
import os
import re
import secrets
from datetime import datetime, timedelta, timezone
from uuid import uuid4

import boto3
from botocore.config import Config
from flask import jsonify, request
from sqlalchemy import or_

from database import Alert, AlertEvidence, EvidenceRecorder, Esp32Hub, SessionLocal

RETENTION = timedelta(days=30)
LEASE = timedelta(minutes=10)
KINDS = {'photo': ('image/jpeg', 'jpg', 2_000_000),
         'thumbnail': ('image/jpeg', 'jpg', 150_000),
         'video': ('video/mp4', 'mp4', 6_000_000)}
REASONS = {'buffer_missing', 'camera_offline', 'capture_failed', 'storage_unavailable',
           'upload_failed', 'recorder_disabled', 'retry_exhausted', 'clock_unsynchronized'}


def now():
    return datetime.now(timezone.utc)


def utc(value):
    return value.replace(tzinfo=timezone.utc) if value.tzinfo is None else value


def storage_ready():
    # A separate, private bucket is mandatory. Never fall back to the public RV bucket.
    return (all(os.getenv(k) for k in ('NETTRA_MEDIA_BUCKET', 'NETTRA_MEDIA_ACCESS_KEY_ID',
                                     'NETTRA_MEDIA_SECRET_ACCESS_KEY', 'R2_ACCOUNT_ID'))
            and os.getenv('NETTRA_MEDIA_BUCKET') != os.getenv('R2_BUCKET_NAME'))


def storage():
    if not storage_ready():
        raise RuntimeError('Private evidence storage is not configured')
    return boto3.client('s3', endpoint_url=f"https://{os.environ['R2_ACCOUNT_ID']}.r2.cloudflarestorage.com",
                        aws_access_key_id=os.environ['NETTRA_MEDIA_ACCESS_KEY_ID'],
                        aws_secret_access_key=os.environ['NETTRA_MEDIA_SECRET_ACCESS_KEY'], region_name='auto',
                        config=Config(signature_version='s3v4', request_checksum_calculation='when_required',
                                      response_checksum_validation='when_required'))


def enqueue_evidence(db, hub, alert):
    recorder = db.get(EvidenceRecorder, hub.serial)
    if not recorder or not recorder.enabled or recorder.shop_id != hub.shop_id:
        return
    camera = {'intrusion': 1, 'intrusion-2': 2, 'camera-interference-1': 1,
              'camera-interference-2': 2}.get(alert.event_type)
    if camera is None and alert.event_type not in {'smoke', 'panic', 'shutter-1', 'shutter-2'}:
        return
    enabled = {1} | {c.get('camera_id') for c in (hub.reported_state or {}).get('cameras', []) if c.get('enabled')}
    for camera_id in recorder.camera_ids:
        if camera_id not in enabled or (camera and camera != camera_id):
            continue
        timestamp = utc(alert.timestamp)
        db.add(AlertEvidence(id=uuid4().hex, alert_id=alert.id, serial=hub.serial, camera_id=camera_id,
                             event_at=timestamp, expires_at=timestamp + RETENTION, status='pending'))


def describe(row, urls=False):
    expired = utc(row.expires_at) <= now()
    overdue = row.status in ('pending', 'capturing') and utc(row.event_at) < now() - timedelta(hours=24)
    status = 'expired' if expired else 'unavailable' if overdue else row.status
    result = dict(id=row.id, camera_id=row.camera_id, status=status, expires_at=utc(row.expires_at).isoformat(),
                  reason='retry_exhausted' if overdue else row.reason,
                  pre_seconds=(row.manifest or {}).get('pre_seconds'), post_seconds=(row.manifest or {}).get('post_seconds'))
    if not urls or status not in ('ready', 'partial') or not storage_ready():
        return result
    # Round signing time indirectly by only fetching these URLs on detail/thumbnail reads;
    # list responses contain no expiring credentials.
    client = storage()
    ttl = max(1, min(300, int((utc(row.expires_at) - now()).total_seconds())))
    for kind, item in (row.manifest or {}).get('objects', {}).items():
        result[kind] = dict(url=client.generate_presigned_url('get_object', Params={
            'Bucket': os.environ['NETTRA_MEDIA_BUCKET'], 'Key': item['key']}, ExpiresIn=ttl),
                           content_type=KINDS[kind][0], size=item['size'])
    result['url_expires_at'] = (now() + timedelta(seconds=ttl)).isoformat()
    return result


def validate_manifest(data):
    if not isinstance(data, dict) or not isinstance(data.get('objects'), dict):
        raise ValueError('A media manifest is required')
    objects = data['objects']
    if not objects or set(objects) - set(KINDS) or ('thumbnail' in objects and 'photo' not in objects):
        raise ValueError('Invalid media kinds')
    pre, post = data.get('pre_seconds'), data.get('post_seconds')
    if any(type(v) not in (int, float) or not 0 <= v <= 15 for v in (pre, post)):
        raise ValueError('Invalid recording coverage')
    if pre > 5.25 or post > 10.25 or pre + post > 15.25:
        raise ValueError('Recording exceeds the incident window')
    for kind, item in objects.items():
        if not isinstance(item, dict) or type(item.get('size')) is not int or not 1 <= item['size'] <= KINDS[kind][2]:
            raise ValueError('Media exceeds the size limit')
        if not isinstance(item.get('sha256'), str) or not re.fullmatch(r'[0-9a-f]{64}', item['sha256']):
            raise ValueError('Invalid media checksum')
        try:
            md5 = base64.b64decode(item.get('md5', ''), validate=True)
        except (ValueError, TypeError):
            raise ValueError('Invalid media checksum') from None
        if len(md5) != 16:
            raise ValueError('Invalid media checksum')
    return {'pre_seconds': pre, 'post_seconds': post,
            'objects': {k: {f: v[f] for f in ('size', 'sha256', 'md5')} for k, v in objects.items()}}


def register_evidence_routes(app, auth_required, owned_shop_or_response):
    @app.route('/shop/<shop_id>/evidence-recorder', methods=['GET', 'PUT'])
    @auth_required
    def recorder_settings(shop_id):
        from esp32_api import hub_for_shop
        with SessionLocal() as db:
            shop, error = owned_shop_or_response(db, shop_id)
            if error:
                return error
            hub = hub_for_shop(db, shop_id, lock=True)
            if not hub:
                return jsonify(ok=False, error='An ESP32 hub is required'), 409
            row = db.get(EvidenceRecorder, hub.serial)
            token = None
            if request.method == 'PUT':
                payload = request.get_json(silent=True)
                if (not isinstance(payload, dict) or type(payload.get('enabled')) is not bool
                    or not isinstance(payload.get('camera_ids'), list) or not payload['camera_ids']
                    or any(type(c) is not int or c not in (1, 2) for c in payload['camera_ids'])
                    or len(set(payload['camera_ids'])) != len(payload['camera_ids'])):
                    return jsonify(ok=False, error='Choose Camera 1 and/or Camera 2'), 400
                if payload['enabled'] and not storage_ready():
                    return jsonify(ok=False, error='Private evidence storage is not configured'), 503
                if row is None:
                    row = EvidenceRecorder(serial=hub.serial, shop_id=shop.id)
                    db.add(row)
                # Explicit provisioning rotates the dedicated credential, never the hub credential.
                token = secrets.token_urlsafe(32)
                row.token_hash = hashlib.sha256(token.encode()).hexdigest()
                row.enabled, row.camera_ids = payload['enabled'], payload['camera_ids']
                row.shop_id = shop.id
                if not row.enabled:
                    for job in db.query(AlertEvidence).filter_by(serial=hub.serial).filter(AlertEvidence.status.in_(('pending', 'capturing'))):
                        job.status, job.reason = 'unavailable', 'recorder_disabled'
                db.commit()
            return jsonify(ok=True, enabled=bool(row and row.enabled), serial=hub.serial,
                           camera_ids=row.camera_ids if row else [], token=token,
                           storage_ready=storage_ready(), last_seen_at=utc(row.last_seen_at).isoformat() if row and row.last_seen_at else None,
                           cameras=row.cameras if row else [], retention_days=30)

    @app.get('/alerts/<shop_id>/<alert_id>/evidence')
    @auth_required
    def read_evidence(shop_id, alert_id):
        with SessionLocal() as db:
            shop, error = owned_shop_or_response(db, shop_id)
            if error:
                return error
            alert = db.get(Alert, alert_id)
            if not alert or alert.shop_id != shop.id:
                return jsonify(ok=False, error='Alert not found'), 404
            rows = db.query(AlertEvidence).filter_by(alert_id=alert.id).order_by(AlertEvidence.camera_id).all()
            response = jsonify(ok=True, evidence=[describe(row, urls=True) for row in rows])
            response.headers['Cache-Control'] = 'private, no-store'
            return response

    @app.get('/alerts/<shop_id>/<alert_id>/thumbnail')
    @auth_required
    def thumbnail(shop_id, alert_id):
        # Authenticated redirect, suitable for a React Native Image with auth headers.
        from flask import redirect
        with SessionLocal() as db:
            shop, error = owned_shop_or_response(db, shop_id)
            if error:
                return error
            alert = db.get(Alert, alert_id)
            if not alert or alert.shop_id != shop.id:
                return jsonify(ok=False, error='Alert not found'), 404
            rows = db.query(AlertEvidence).filter_by(alert_id=alert.id).order_by(AlertEvidence.camera_id).all()
            for row in rows:
                if row.status in ('ready', 'partial') and utc(row.expires_at) > now() and 'thumbnail' in (row.manifest or {}).get('objects', {}) and storage_ready():
                    url = describe(row, urls=True)['thumbnail']['url']
                    response = redirect(url, code=302)
                    response.headers['Cache-Control'] = 'private, no-store'
                    return response
            return jsonify(ok=False, error='No thumbnail available'), 404

    def recorder_auth(db):
        serial = request.headers.get('X-NETTRA-RECORDER-SERIAL', '')
        token = request.headers.get('X-NETTRA-RECORDER-TOKEN', '')
        if len(serial) > 128 or not 20 <= len(token) <= 128:
            return None
        row = db.query(EvidenceRecorder).filter_by(serial=serial).with_for_update().first()
        hub = db.get(Esp32Hub, serial) if row else None
        if row and row.enabled and hub and row.shop_id == hub.shop_id and hmac.compare_digest(row.token_hash, hashlib.sha256(token.encode()).hexdigest()):
            return row
        return None

    @app.post('/evidence/recorder/poll')
    def recorder_poll():
        if request.content_length and request.content_length > 4096:
            return jsonify(ok=False, error='Payload too large'), 413
        payload = request.get_json(silent=True) or {}
        cameras = payload.get('cameras', []) if isinstance(payload, dict) else None
        if not isinstance(cameras, list) or len(cameras) > 2 or any(not isinstance(c, dict) or type(c.get('camera_id')) is not int or c['camera_id'] not in (1, 2) or type(c.get('online')) is not bool for c in cameras):
            return jsonify(ok=False, error='Invalid camera health'), 400
        with SessionLocal() as db:
            recorder = recorder_auth(db)
            if not recorder:
                return jsonify(ok=False, error='Recorder authentication required'), 401
            recorder.last_seen_at = now()
            recorder.cameras = [{k: c[k] for k in ('camera_id', 'online')} for c in cameras]
            timestamp = now()
            # The recorder row lock serializes claims on PostgreSQL across workers.
            # Return an existing active lease again after a lost poll response.
            # Waiting ten minutes to reclaim it would lose the pre-event buffer.
            scoped = db.query(AlertEvidence).join(Alert, Alert.id == AlertEvidence.alert_id).filter(
                AlertEvidence.serial == recorder.serial, Alert.shop_id == recorder.shop_id)
            active_job = scoped.filter(AlertEvidence.status == 'capturing').filter(
                AlertEvidence.lease_until > timestamp).order_by(AlertEvidence.event_at).first()
            jobs = [active_job] if active_job else scoped.filter(
                AlertEvidence.status.in_(('pending', 'capturing')),
                or_(AlertEvidence.lease_until.is_(None), AlertEvidence.lease_until < timestamp)).order_by(AlertEvidence.event_at).limit(1).all()
            result = []
            for job in jobs:
                if job.attempts >= 6 or utc(job.event_at) < timestamp - timedelta(hours=24):
                    job.status, job.reason = 'unavailable', 'retry_exhausted'
                    continue
                if job is not active_job:
                    job.status, job.lease_id = 'capturing', uuid4().hex
                    job.lease_until, job.attempts = timestamp + LEASE, job.attempts + 1
                    # Never reuse uploaded keys from a previous lease.
                    job.manifest = None
                result.append(dict(id=job.id, camera_id=job.camera_id, event_at=utc(job.event_at).isoformat(),
                                   lease_id=job.lease_id, lease_until=utc(job.lease_until).isoformat(),
                                   pre_seconds=5, post_seconds=10))
            hub = db.get(Esp32Hub, recorder.serial)
            config = None
            if 2 in recorder.camera_ids:
                from database import Esp32CameraConfig
                from esp32_camera_api import unseal
                current = db.get(Esp32CameraConfig, recorder.serial)
                if current and current.revision == (hub.reported_state or {}).get('camera_config_revision'):
                    from cryptography.fernet import InvalidToken
                    try:
                        config = unseal(current)
                    except (InvalidToken, KeyError, ValueError):
                        # Broken Camera 2 configuration must not stop Camera 1 jobs.
                        app.logger.error('Recorder cannot decrypt active Camera 2 configuration')
            db.commit()
            enabled = {1} | {c.get('camera_id') for c in (hub.reported_state or {}).get('cameras', []) if c.get('enabled')}
            response = jsonify(ok=True, server_time=timestamp.isoformat(), camera_ids=[c for c in recorder.camera_ids if c in enabled],
                               camera_2=config, camera_2_active_revision=(hub.reported_state or {}).get('camera_config_revision', 'factory'), jobs=result)
            response.headers['Cache-Control'] = 'no-store'
            return response

    @app.post('/evidence/recorder/jobs/<job_id>/<action>')
    def recorder_job(job_id, action):
        if action not in ('prepare', 'complete', 'fail'):
            return jsonify(ok=False, error='Unknown action'), 404
        if request.content_length and request.content_length > 8192:
            return jsonify(ok=False, error='Payload too large'), 413
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict):
            return jsonify(ok=False, error='Payload required'), 400
        with SessionLocal() as db:
            recorder = recorder_auth(db)
            if not recorder:
                return jsonify(ok=False, error='Recorder authentication required'), 401
            job = db.query(AlertEvidence).join(Alert, Alert.id == AlertEvidence.alert_id).filter(
                AlertEvidence.id == job_id, AlertEvidence.serial == recorder.serial,
                Alert.shop_id == recorder.shop_id).with_for_update().first()
            if not job or payload.get('lease_id') != job.lease_id:
                return jsonify(ok=False, error='Unknown or stale recording job'), 409
            if action == 'complete' and job.status in ('ready', 'partial'):
                return jsonify(ok=True, status=job.status)
            if job.status != 'capturing' or utc(job.lease_until) <= now() or utc(job.expires_at) <= now():
                return jsonify(ok=False, error='Recording lease expired'), 409
            if action == 'fail':
                reason = payload.get('reason')
                if reason not in REASONS or type(payload.get('retry', False)) is not bool:
                    return jsonify(ok=False, error='Invalid failure reason'), 400
                job.reason = reason
                job.status = 'pending' if payload.get('retry') and job.attempts < 6 else 'unavailable'
                job.lease_until = now() + timedelta(seconds=30)
                db.commit()
                return jsonify(ok=True, status=job.status)
            if not storage_ready():
                return jsonify(ok=False, error='Private evidence storage unavailable'), 503
            if action == 'prepare':
                try:
                    manifest = validate_manifest(payload.get('manifest'))
                except ValueError as exc:
                    return jsonify(ok=False, error=str(exc)), 400
                if job.manifest:
                    # Idempotent retry only; one immutable manifest per lease.
                    old = {**job.manifest, 'objects': {k: {f: v[f] for f in ('size', 'sha256', 'md5')} for k, v in job.manifest['objects'].items()}}
                    if old != manifest:
                        return jsonify(ok=False, error='Manifest already prepared'), 409
                for kind, item in manifest['objects'].items():
                    item['key'] = f'evidence/v1/{job.serial}/{job.id}/{job.lease_id}/{kind}-{item["sha256"]}.{KINDS[kind][1]}'
                job.manifest = manifest
                job.lease_until = now() + LEASE
                db.commit()
                client = storage()
                uploads = {}
                for kind, item in manifest['objects'].items():
                    params = dict(Bucket=os.environ['NETTRA_MEDIA_BUCKET'], Key=item['key'], ContentType=KINDS[kind][0],
                                  ContentMD5=item['md5'], Metadata={'sha256': item['sha256']}, CacheControl='private, no-store')
                    uploads[kind] = dict(url=client.generate_presigned_url('put_object', Params=params, ExpiresIn=300),
                                         headers={'Content-Type': KINDS[kind][0], 'Content-MD5': item['md5'], 'x-amz-meta-sha256': item['sha256'], 'Cache-Control': 'private, no-store'})
                return jsonify(ok=True, uploads=uploads)
            if not job.manifest:
                return jsonify(ok=False, error='Prepare media before completion'), 409
            client = storage()
            for kind, item in job.manifest['objects'].items():
                try:
                    head = client.head_object(Bucket=os.environ['NETTRA_MEDIA_BUCKET'], Key=item['key'])
                except Exception:
                    return jsonify(ok=False, error='Uploaded media not yet available'), 409
                expected_etag = base64.b64decode(item['md5']).hex()
                if (head.get('ContentLength') != item['size'] or head.get('ContentType') != KINDS[kind][0]
                    or head.get('Metadata', {}).get('sha256') != item['sha256']
                    or head.get('ETag', '').strip('"') != expected_etag):
                    return jsonify(ok=False, error='Uploaded media verification failed'), 409
            complete = (set(job.manifest['objects']) == set(KINDS) and job.manifest['pre_seconds'] >= 4.75
                        and job.manifest['post_seconds'] >= 9.75)
            job.status, job.reason = ('ready', None) if complete else ('partial', 'buffer_missing')
            db.commit()
            return jsonify(ok=True, status=job.status)
