"""Private incident evidence. Alarm ingestion only inserts jobs; it never contacts storage.

A separately enrolled recorder can only claim its hub's jobs. R2 keys never leave
this server, and signed reads are issued only after checking site ownership.
"""
import base64
import hashlib
import hmac
import math
import os
import re
import secrets
from datetime import datetime, timedelta, timezone
from uuid import uuid4

import boto3
from botocore.config import Config
from flask import jsonify, request
from sqlalchemy import or_

from database import Alert, AlertEvidence, EvidenceRecorder, Esp32Hub, HubAlarmPhoto, SessionLocal

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
                        config=Config(signature_version='s3v4', connect_timeout=3, read_timeout=5,
                                      retries={'max_attempts': 1, 'mode': 'standard'}, request_checksum_calculation='when_required',
                                      response_checksum_validation='when_required'))


def enqueue_evidence(db, hub, alert):
    from alarm_photo_api import enqueue_alarm_photos
    enqueue_alarm_photos(db, hub, alert)
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
    result['has_thumbnail'] = status in ('ready', 'partial') and 'thumbnail' in (row.manifest or {}).get('objects', {})
    offset = (row.manifest or {}).get('photo_offset_seconds')
    if type(offset) in (int, float) and math.isfinite(offset):
        result.update(photo_offset_seconds=offset, capture_mode='recorded_frame',
                      captured_at=(utc(row.event_at)+timedelta(seconds=offset)).isoformat())
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
    result = {'pre_seconds': pre, 'post_seconds': post,
              'objects': {k: {f: v[f] for f in ('size', 'sha256', 'md5')} for k, v in objects.items()}}
    if 'photo_offset_seconds' in data:
        offset = data['photo_offset_seconds']
        if (type(offset) not in (int, float) or not math.isfinite(offset) or 'photo' not in objects
                or not -pre-0.01 <= offset <= post+0.01):
            raise ValueError('Photo timestamp must be inside the actual recorded window')
        result['photo_offset_seconds'] = round(offset, 3)
    return result


def describe_alerts(db, alert_ids, urls=False):
    """One card per camera, with a usable hub photo even if the recorder is absent."""
    if not alert_ids:
        return {}
    by_alert = {}
    rows = db.query(AlertEvidence).filter(AlertEvidence.alert_id.in_(alert_ids)).all()
    recorders = {r.serial:r for r in db.query(EvidenceRecorder).filter(
        EvidenceRecorder.serial.in_({r.serial for r in rows})).all()} if rows else {}
    for row in rows:
        item = describe(row, urls)
        recorder = recorders.get(row.serial)
        healthy = (recorder and recorder.enabled and recorder.last_seen_at
                   and utc(recorder.last_seen_at) > now()-timedelta(seconds=20)
                   and any(c.get('camera_id') == row.camera_id and c.get('online') for c in (recorder.cameras or [])))
        processing = (row.status == 'capturing' and row.lease_until
                      and utc(row.lease_until) > now() and recorder and recorder.enabled)
        item['video_status'] = item['status']
        if item['status'] in ('pending', 'capturing') and not healthy and not processing:
            item['video_status'] = 'unavailable'
            item['video_reason'] = 'recorder_offline'
        by_alert.setdefault(row.alert_id, {})[row.camera_id] = item
    from alarm_photo_api import describe_photo
    for row in db.query(HubAlarmPhoto).filter(HubAlarmPhoto.alert_id.in_(alert_ids)).all():
        cards = by_alert.setdefault(row.alert_id, {})
        photo = describe_photo(row, urls)
        previous = cards.get(row.camera_id)
        if (previous and previous['status'] in ('ready', 'partial')
                and type(previous.get('photo_offset_seconds')) in (int, float)
                and -2.25 <= previous['photo_offset_seconds'] <= -1.75):
            # Once verified buffered evidence is ready, prefer its pre-alarm photo.
            continue
        if previous and previous['status'] in ('ready', 'partial') and photo['status'] != 'ready':
            continue
        if photo['status'] == 'ready':
            if previous:
                photo['video_status'] = previous.get('video_status', previous['status'])
                if previous.get('video_reason'): photo['video_reason'] = previous['video_reason']
                if previous.get('video'):
                    photo['video'] = previous['video']
                    photo['pre_seconds'], photo['post_seconds'] = previous['pre_seconds'], previous['post_seconds']
                    photo['status'] = previous['status']
            cards[row.camera_id] = photo
        elif not previous or previous['status'] not in ('ready', 'partial'):
            cards[row.camera_id] = photo
    return {key: [cards[camera] for camera in sorted(cards)] for key, cards in by_alert.items()}


def register_evidence_routes(app, auth_required, owned_shop_or_response):
    from alarm_photo_api import register_alarm_photo_routes
    register_alarm_photo_routes(app)
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
            response = jsonify(ok=True, evidence=describe_alerts(db, [alert.id], urls=True).get(alert.id, []))
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
            for item in describe_alerts(db, [alert.id], urls=True).get(alert.id, []):
                if item.get('thumbnail'):
                    url = item['thumbnail']['url']
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
            if payload.get('cloud_ingest_protocol_version') == 1:
                # A newly connected cloud receiver cannot reconstruct incidents
                # older than its bounded 180-second ring. Do not let never-started
                # historical work starve current alerts; retain upload retries.
                for stale in scoped.filter(AlertEvidence.status == 'pending', AlertEvidence.attempts == 0,
                                           AlertEvidence.event_at < timestamp-timedelta(seconds=180)).all():
                    stale.status, stale.reason = 'unavailable', 'buffer_missing'
                db.flush()
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
            from esp32_camera_api import unseal,revisions,model
            from cryptography.fernet import InvalidToken
            camera_configs={1:None,2:None}
            for camera_id in recorder.camera_ids:
                current=db.get(model(camera_id),recorder.serial)
                if current and current.revision==revisions(hub.reported_state or {},camera_id)[0]:
                    try: camera_configs[camera_id]=unseal(current)
                    except (InvalidToken,KeyError,ValueError):
                        app.logger.error('Recorder cannot decrypt active camera configuration')
            primary_revision=revisions(hub.reported_state or {},1)[0]
            legacy_primary=primary_revision!='factory' and payload.get('camera_config_protocol_version')!=2
            if legacy_primary:
                # An older recorder must never label its fixed original feed as the new Camera 1.
                for job in jobs:
                    if job.camera_id==1: job.status,job.reason='unavailable','camera_offline'
                result=[job for job in result if job['camera_id']!=1]
            db.commit()
            enabled = {1} | {c.get('camera_id') for c in (hub.reported_state or {}).get('cameras', []) if c.get('enabled')}
            if legacy_primary: enabled.discard(1)
            response = jsonify(ok=True, server_time=timestamp.isoformat(), shop_id=recorder.shop_id, camera_ids=[c for c in recorder.camera_ids if c in enabled],
                               camera_1=None if payload.get('cloud_ingest_protocol_version') == 1 else camera_configs[1],camera_1_active_revision=primary_revision,
                               camera_2=None if payload.get('cloud_ingest_protocol_version') == 1 else camera_configs[2], camera_2_active_revision=(hub.reported_state or {}).get('camera_config_revision', 'factory'), jobs=result)
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
