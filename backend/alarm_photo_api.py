"""Bounded ESP32 JPEG uploads. Alarm ingestion never waits for capture or storage."""
import base64
import hashlib
import hmac
from io import BytesIO
from datetime import datetime, timedelta, timezone
from uuid import uuid4
from functools import wraps
from threading import BoundedSemaphore

from flask import jsonify, request
from PIL import Image, ImageOps, UnidentifiedImageError

from database import Alert, AlertEvidence, EvidenceRecorder, Esp32EventReceipt, Esp32Hub, HubAlarmPhoto, SessionLocal

MAX_PHOTO_BYTES = 1024 * 1024
MAX_PIXELS = 12_000_000
UPLOAD_WINDOW = timedelta(minutes=10)
PHOTO_SLOTS = BoundedSemaphore(2)
FAILURES = {'capture_failed', 'camera_offline', 'snapshot_too_large', 'memory_pressure',
            'capture_timeout', 'queue_full', 'upload_failed'}


def bounded_photo_processing(view):
    @wraps(view)
    def wrapped(*args, **kwargs):
        if not PHOTO_SLOTS.acquire(blocking=False):
            return jsonify(ok=False, error='Photo processor busy; retry the same photo'), 503
        try:
            return view(*args, **kwargs)
        finally:
            PHOTO_SLOTS.release()
    return wrapped


def enqueue_alarm_photos(db, hub, alert):
    if (hub.reported_state or {}).get('snapshot_protocol_version') != 1:
        return
    # App-only camera advisories do not go through the alarm capture queue.
    cameras = {'intrusion': [1], 'intrusion-2': [2], 'smoke': [1, 2],
               'panic': [1, 2], 'shutter-1': [1, 2], 'shutter-2': [1, 2]}.get(alert.event_type, [])
    enabled = {1} | {c.get('camera_id') for c in (hub.reported_state or {}).get('cameras', []) if c.get('enabled')}
    from evidence_api import utc, RETENTION
    timestamp = utc(alert.timestamp)
    for camera_id in cameras:
        if camera_id in enabled:
            db.add(HubAlarmPhoto(id=uuid4().hex, alert_id=alert.id, serial=hub.serial,
                                camera_id=camera_id, event_at=timestamp,
                                expires_at=timestamp + RETENTION, status='pending'))


def strip_jpeg_metadata(data):
    """Remove metadata before SOS without re-encoding pixels; decline unusual layouts."""
    output = bytearray(b'\xff\xd8')
    position = 2
    private_markers = (*range(0xe1, 0xf0), 0xfe)
    while position + 4 <= len(data) and data[position] == 0xff:
        start = position
        while position < len(data) and data[position] == 0xff:
            position += 1
        if position + 3 > len(data):
            return None
        marker = data[position]
        if marker in (0, 1, 0xd8, 0xd9) or 0xd0 <= marker <= 0xd7:
            return None
        length = int.from_bytes(data[position+1:position+3], 'big')
        end = position + 1 + length
        if length < 2 or end > len(data):
            return None
        if marker == 0xda:
            # Metadata between progressive scans is unusual; re-encode instead.
            if any(bytes((0xff, value)) in data[end:] for value in private_markers):
                return None
            output.extend(data[start:])
            return bytes(output)
        # The fixed Adobe color-transform header carries no private metadata.
        adobe = marker == 0xee and length == 14 and data[position+3:position+8] == b'Adobe'
        if marker not in private_markers or adobe:
            output.extend(data[start:end])
        position = end
    return None


def optimize_photo(data):
    """Decode only bounded JPEGs; re-encode without source EXIF/location metadata."""
    if not data.startswith(b'\xff\xd8') or not data.endswith(b'\xff\xd9'):
        raise ValueError('A complete JPEG is required')
    try:
        with Image.open(BytesIO(data), formats=['JPEG']) as original:
            if original.width * original.height > MAX_PIXELS:
                raise ValueError('Camera photo has too many pixels')
            original.load()
            image = ImageOps.exif_transpose(original).convert('RGB')
            image.thumbnail((1920, 1080), Image.Resampling.LANCZOS)
            small = image.copy()
            small.thumbnail((320, 180), Image.Resampling.LANCZOS)
            result = {}
            for kind, item, quality in [('photo', image, 82), ('thumbnail', small, 70)]:
                output = BytesIO()
                item.save(output, format='JPEG', quality=quality, optimize=True, progressive=True)
                result[kind] = output.getvalue()
            clean = strip_jpeg_metadata(data)
            if (original.width <= 1920 and original.height <= 1080 and clean
                    and len(clean) < len(result['photo'])):
                result['photo'] = clean
            return result
    except (UnidentifiedImageError, OSError, Image.DecompressionBombError) as error:
        raise ValueError('Invalid camera JPEG') from error


def describe_photo(row, urls=False):
    from evidence_api import now, utc, storage, storage_ready, KINDS
    expired = utc(row.expires_at) <= now()
    overdue = row.status == 'pending' and utc(row.event_at) + UPLOAD_WINDOW < now()
    status = 'expired' if expired else 'unavailable' if overdue else row.status
    result = dict(id=row.id, camera_id=row.camera_id, status=status,
                  reason='capture_timeout' if overdue else row.reason,
                  expires_at=utc(row.expires_at).isoformat(), pre_seconds=0, post_seconds=0,
                  capture_mode='snapshot', captured_at=utc(row.captured_at).isoformat() if row.captured_at else None,
                  has_thumbnail=status == 'ready' and 'thumbnail' in (row.manifest or {}).get('objects', {}))
    if urls and status == 'ready' and storage_ready():
        client = storage()
        ttl = max(1, min(300, int((utc(row.expires_at) - now()).total_seconds())))
        import os
        for kind, item in (row.manifest or {}).get('objects', {}).items():
            result[kind] = dict(url=client.generate_presigned_url('get_object', Params={
                'Bucket': os.environ['NETTRA_MEDIA_BUCKET'], 'Key': item['key']}, ExpiresIn=ttl),
                               content_type=KINDS[kind][0], size=item['size'])
        result['url_expires_at'] = (now() + timedelta(seconds=ttl)).isoformat()
    return result


def register_alarm_photo_routes(app):
    def find_photo(db, event_id, camera_id):
        from esp32_api import IDENTIFIER, secret_hash
        serial = request.headers.get('X-AMTECH-DEVICE-SERIAL', '')
        token = request.headers.get('X-AMTECH-DEVICE-TOKEN', '')
        if not IDENTIFIER.fullmatch(serial) or not token or len(token) > 256:
            return None, (jsonify(ok=False, error='Device authentication required'), 401)
        hub = db.get(Esp32Hub, serial)
        if not hub or not hmac.compare_digest(hub.token_hash, secret_hash(token)):
            return None, (jsonify(ok=False, error='Invalid device credentials'), 401)
        if not IDENTIFIER.fullmatch(event_id) or camera_id not in (1, 2):
            return None, (jsonify(ok=False, error='Invalid alarm photo identity'), 400)
        receipt = db.get(Esp32EventReceipt, f'{serial}:{event_id}')
        alert = db.get(Alert, receipt.alert_id) if receipt else None
        if not hub.shop_id or not alert or alert.shop_id != hub.shop_id:
            return None, (jsonify(ok=False, error='Alarm is not accepted for this hub/site'), 404)
        row = db.query(HubAlarmPhoto).filter_by(alert_id=alert.id, camera_id=camera_id,
                                               serial=serial).with_for_update().first()
        if row is None:
            return None, (jsonify(ok=False, error='No photo requested for this camera'), 404)
        return row, None

    @app.post('/esp32/device/photos/<event_id>/<int:camera_id>')
    @bounded_photo_processing
    def upload_alarm_photo(event_id, camera_id):
        if request.mimetype != 'image/jpeg' or not request.content_length or not 4 <= request.content_length <= MAX_PHOTO_BYTES:
            return jsonify(ok=False, error='Supply a JPEG of at most one MiB'), 413
        from evidence_api import now, utc, storage, storage_ready
        with SessionLocal() as db:
            row, error = find_photo(db, event_id, camera_id)
            if error:
                return error
            if utc(row.expires_at) <= now() or utc(row.event_at) + UPLOAD_WINDOW < now():
                return jsonify(ok=False, error='Alarm photo capture window expired'), 410
            try:
                captured = datetime.fromtimestamp(int(request.headers.get('X-NETTRA-CAPTURED-AT', '')), timezone.utc)
            except (ValueError, OverflowError, OSError):
                return jsonify(ok=False, error='Valid capture time required'), 400
            if not utc(row.event_at) - timedelta(seconds=5) <= captured <= min(now() + timedelta(seconds=5), utc(row.event_at) + timedelta(seconds=30)):
                return jsonify(ok=False, error='Photo was not captured near the alarm'), 400
            data = request.stream.read(MAX_PHOTO_BYTES + 1)
            if len(data) != request.content_length or len(data) > MAX_PHOTO_BYTES:
                return jsonify(ok=False, error='Incomplete or oversized JPEG'), 413
            digest = hashlib.sha256(data).hexdigest()
            if row.status == 'ready':
                if row.source_sha256 != digest or utc(row.captured_at) != captured:
                    return jsonify(ok=False, error='Alarm photo is already attached'), 409
                return jsonify(ok=True, status='ready')
            if not storage_ready():
                return jsonify(ok=False, error='Private photo storage unavailable'), 503
            try:
                media = optimize_photo(data)
            except ValueError as error:
                return jsonify(ok=False, error=str(error)), 400
            import os
            client = storage()
            objects = {}
            try:
                for kind, body in media.items():
                    sha = hashlib.sha256(body).hexdigest()
                    key = f'evidence/v1/{row.serial}/snapshots/{row.id}/{digest}/{kind}-{sha}.jpg'
                    client.put_object(Bucket=os.environ['NETTRA_MEDIA_BUCKET'], Key=key, Body=body,
                                      ContentType='image/jpeg', CacheControl='private, no-store',
                                      ContentMD5=base64.b64encode(hashlib.md5(body).digest()).decode(),
                                      Metadata={'sha256': sha})
                    objects[kind] = {'key': key, 'size': len(body), 'sha256': sha}
            except Exception:
                app.logger.warning('Private alarm-photo storage unavailable; hub can retry')
                return jsonify(ok=False, error='Photo storage unavailable; retry the same photo'), 503
            row.status, row.reason = 'ready', None
            row.captured_at, row.source_sha256 = captured, digest
            row.manifest = {'objects': objects}
            db.commit()
            return jsonify(ok=True, status='ready')

    @app.post('/esp32/device/photos/<event_id>/<int:camera_id>/failure')
    def report_photo_failure(event_id, camera_id):
        if request.content_length and request.content_length > 1024:
            return jsonify(ok=False, error='Payload too large'), 413
        payload = request.get_json(silent=True)
        if not isinstance(payload, dict) or payload.get('reason') not in FAILURES:
            return jsonify(ok=False, error='Valid photo failure reason required'), 400
        with SessionLocal() as db:
            row, error = find_photo(db, event_id, camera_id)
            if error:
                return error
            if row.status != 'ready':
                row.status, row.reason = 'unavailable', payload['reason']
                db.commit()
            return jsonify(ok=True, status=row.status)

    @app.post('/esp32/device/photo-test')
    def create_photo_test():
        """USB technician commissioning only: silent history entry, never an alarm command/push."""
        from esp32_api import IDENTIFIER, secret_hash
        from evidence_api import now, utc, RETENTION
        if request.content_length and request.content_length > 1024:
            return jsonify(ok=False, error='Payload too large'), 413
        serial = request.headers.get('X-AMTECH-DEVICE-SERIAL', '')
        token = request.headers.get('X-AMTECH-DEVICE-TOKEN', '')
        payload = request.get_json(silent=True)
        if not IDENTIFIER.fullmatch(serial) or not token or len(token) > 256:
            return jsonify(ok=False, error='Device authentication required'), 401
        if (not isinstance(payload, dict) or not isinstance(payload.get('event_id'), str)
                or not IDENTIFIER.fullmatch(payload['event_id']) or type(payload.get('camera_id')) is not int
                or payload['camera_id'] not in (1, 2) or type(payload.get('event_at')) is not int):
            return jsonify(ok=False, error='Valid photo test identity required'), 400
        with SessionLocal() as db:
            hub = db.query(Esp32Hub).filter_by(serial=serial).with_for_update().first()
            if not hub or not hmac.compare_digest(hub.token_hash, secret_hash(token)):
                return jsonify(ok=False, error='Invalid device credentials'), 401
            receipt_id = f"{serial}:{payload['event_id']}"
            known = db.get(Esp32EventReceipt, receipt_id)
            if known:
                alert = db.get(Alert, known.alert_id)
                photo = db.query(HubAlarmPhoto).filter_by(alert_id=known.alert_id, camera_id=payload['camera_id']).first()
                if alert and alert.shop_id == hub.shop_id and alert.event_type == 'test' and photo:
                    return jsonify(ok=True)
                return jsonify(ok=False, error='Event identity already used'), 409
            state = hub.reported_state or {}
            enabled = {1} | {c.get('camera_id') for c in state.get('cameras', []) if c.get('enabled')}
            if (not hub.shop_id or state.get('snapshot_protocol_version') != 1
                    or state.get('arm_state') != 'disarmed' or state.get('siren_active') is not False
                    or payload['camera_id'] not in enabled):
                return jsonify(ok=False, error='A disarmed photo-capable hub and configured camera are required'), 409
            try:
                event_at = datetime.fromtimestamp(payload['event_at'], timezone.utc)
            except (ValueError, OverflowError, OSError):
                return jsonify(ok=False, error='Invalid test time'), 400
            if abs((now()-event_at).total_seconds()) > 600:
                return jsonify(ok=False, error='Photo test expired'), 410
            count = db.query(HubAlarmPhoto).join(Alert, Alert.id == HubAlarmPhoto.alert_id).filter(
                HubAlarmPhoto.serial == serial, Alert.event_type == 'test',
                HubAlarmPhoto.event_at > now()-timedelta(hours=1)).count()
            if count >= 5:
                return jsonify(ok=False, error='Photo commissioning limit reached; retry later'), 429
            alert = Alert(id=str(uuid4()), shop_id=hub.shop_id, event_type='test', timestamp=event_at)
            db.add(alert); db.flush()
            db.add(Esp32EventReceipt(id=receipt_id, alert_id=alert.id))
            db.add(HubAlarmPhoto(id=uuid4().hex, alert_id=alert.id, serial=serial, camera_id=payload['camera_id'],
                                event_at=event_at, expires_at=event_at+RETENTION, status='pending'))
            recorder = db.get(EvidenceRecorder, serial)
            if (recorder and recorder.enabled and recorder.shop_id == hub.shop_id
                    and payload['camera_id'] in recorder.camera_ids):
                db.add(AlertEvidence(id=uuid4().hex, alert_id=alert.id, serial=serial, camera_id=payload['camera_id'],
                                     event_at=event_at, expires_at=event_at+RETENTION, status='pending'))
            db.commit()
            return jsonify(ok=True)
