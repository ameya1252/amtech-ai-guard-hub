"""Private operations API, accessed by the portal's server, never its browser.

There is no portal account/login. A server-to-server key keeps manufacturing
credentials off the public customer API. Missing configuration fails closed.
"""
import hashlib
import hmac
import json
import math
import os
import re
import secrets
from datetime import datetime, timezone
from functools import wraps
from uuid import uuid4
from cryptography.fernet import Fernet, InvalidToken
from flask import jsonify, request
from sqlalchemy.exc import IntegrityError
from sqlalchemy import func
from database import Alert, Esp32Command, Esp32Hub, SessionLocal, Shop
from operations_models import Asset, InstallationLocation, LabelBatch, OperationsEvent, PackingOrder, SerialCounter

KINDS = {'hub_base': 'B', 'hub_high': 'H', 'camera': 'C', 'shutter': 'S', 'smoke': 'D'}
CAPACITY = {'camera': 1, 'shutter': 2, 'smoke': 1}
EOL_CHECKS = ('identity', 'sensors', 'siren', 'lte', 'ethernet', 'wifi', 'camera', 'app')
SERIAL = re.compile(r'^[A-Za-z0-9_-]{1,128}$')

def now():
    return datetime.now(timezone.utc)

def iso(value):
    return value.replace(tzinfo=timezone.utc).isoformat() if value and value.tzinfo is None else value.isoformat() if value else None

def digest(value):
    return hashlib.sha256(value.encode()).hexdigest()

def cipher():
    return Fernet(os.environ['AMTECH_LABEL_ENCRYPTION_KEY'].encode())

def seal(value):
    return cipher().encrypt(value.encode()).decode()

def unseal(value):
    return cipher().decrypt(value.encode()).decode()

def audit(db, kind, message, serial=None, details=None):
    db.add(OperationsEvent(id=str(uuid4()), serial=serial, kind=kind, message=message, details=details))

def asset_dict(asset):
    return dict(serial=asset.serial, kind=asset.kind, batch_id=asset.batch_id,
                order_id=asset.order_id, hardware_id=asset.hardware_id,
                pcb_revision=asset.pcb_revision, firmware_version=asset.firmware_version,
                manufacturing_batch=asset.manufacturing_batch, manufactured_on=asset.manufactured_on,
                test_status=asset.test_status, test_results=asset.test_results,
                created_at=iso(asset.created_at))

def order_dict(db, order):
    items = db.query(Asset).filter_by(order_id=order.id).order_by(Asset.serial).all()
    counts = {kind: sum(a.kind == kind for a in items) for kind in CAPACITY}
    return dict(id=order.id, reference=order.reference, status=order.status,
                hub_serial=order.hub_serial, expected=order.expected, counts=counts,
                items=[asset_dict(a) for a in items], created_at=iso(order.created_at),
                packed_at=iso(order.packed_at), dispatched_at=iso(order.dispatched_at),
                complete=bool(order.hub_serial and counts == order.expected))

def package_for_hub(db, serial):
    order = db.query(PackingOrder).filter_by(hub_serial=serial).first()
    if not order or order.status not in ('packed', 'dispatched'):
        return {'hub_serial': serial, 'recorded': False, 'items': [], 'counts': {}}
    items = db.query(Asset).filter(Asset.order_id == order.id, Asset.serial != serial).order_by(Asset.kind, Asset.serial).all()
    return dict(hub_serial=serial, recorded=True, reference=order.reference,
                items=[{'serial': a.serial, 'kind': a.kind} for a in items],
                counts={k: sum(a.kind == k for a in items) for k in CAPACITY},
                installation_verified=False)

def location_payload(payload):
    lat, lng = payload.get('latitude'), payload.get('longitude')
    if lat is None and lng is None:
        return None
    if type(lat) not in (int, float) or type(lng) not in (int, float):
        raise ValueError('Latitude and longitude must both be numbers')
    if not math.isfinite(lat) or not math.isfinite(lng) or not -90 <= lat <= 90 or not -180 <= lng <= 180:
        raise ValueError('Invalid installation location')
    return lat, lng

def claim_manufactured_hub(db, hub, location):
    """Called inside the existing pairing transaction, after owner validation."""
    asset = db.get(Asset, hub.serial)
    if asset:
        order = db.query(PackingOrder).filter_by(hub_serial=hub.serial).first()
        if not order or order.status not in ('packed', 'dispatched') or asset.test_status != 'passed':
            raise ValueError('This hub has not completed AMTECH testing and packing')
        asset.claim_cipher = None
        asset.token_cipher = None
        # The ownership row makes claiming single-use. Retain only its hash so
        # the original owner can safely retry a pairing whose HTTP reply was lost.
    if location:
        db.add(InstallationLocation(shop_id=hub.shop_id, latitude=str(location[0]), longitude=str(location[1]), source='phone'))
    audit(db, 'hub_claimed', 'Hub linked to its customer installation', hub.serial)

def record_observation(db, hub, previous):
    state = hub.reported_state or {}
    dynamic = {'uptime_ms', 'free_heap_bytes'}
    current = {k: v for k, v in state.items() if k not in dynamic}
    old = {k: v for k, v in (previous or {}).items() if k not in dynamic}
    if current != old:
        reboot = bool(state.get('boot_id') and state.get('boot_id') != (previous or {}).get('boot_id'))
        audit(db, 'hub_boot' if reboot else 'state_changed', 'Hub started a new session' if reboot else 'Reported hub state changed', hub.serial, current)

def factory_receipt_id(serial, event_id):
    return digest('factory:' + serial + ':' + event_id)

def factory_event_ids(db, serial, events):
    if not events or not re.fullmatch(r'NET-[BH]-[0-9]{4}-[0-9]{5}', serial):
        return set()
    identities = {factory_receipt_id(serial, event['id']): event['id'] for event in events}
    return {identities[row.id] for row in db.query(OperationsEvent.id).filter(OperationsEvent.id.in_(identities)).all()}

def record_factory_events(db, hub, events):
    """Factory siren/sensor tests must never become the buyer's alarm history."""
    if not events or not db.get(Asset, hub.serial):
        return []  # Preserve the legacy unclaimed-hub outbox contract.
    known = factory_event_ids(db, hub.serial, events)
    for event in events:
        if event['id'] not in known:
            db.add(OperationsEvent(id=factory_receipt_id(hub.serial, event['id']), serial=hub.serial,
                kind='factory_' + event['event_type'], message='Factory test event: ' + event['event_type'],
                details={'context': 'factory', 'device_timestamp': event.get('timestamp')}))
            known.add(event['id'])
    return [event['id'] for event in events]

def parse_scan(raw):
    if not isinstance(raw, str) or len(raw) > 2048:
        raise ValueError('Scan a valid AMTECH label')
    raw = raw.strip()
    if raw.startswith('{'):
        try:
            value = json.loads(raw)
            raw = value.get('device_serial') or value.get('serial')
        except (ValueError, AttributeError):
            raise ValueError('Invalid QR label')
    if not isinstance(raw, str) or not SERIAL.fullmatch(raw):
        raise ValueError('Invalid AMTECH serial')
    return raw

class ProvisionConflict(Exception):
    pass

def issue_identity(db, serial, p):
    hardware = p.get('hardware_id', '')
    if not isinstance(hardware, str):
        raise ValueError('Read the board MAC address before provisioning')
    hardware = hardware.replace(':', '').lower()
    if not re.fullmatch('[0-9a-f]{12}', hardware):
        raise ValueError('Read the board MAC address before provisioning')
    # Match pairing/sync's lock order: hub before manufacturing asset.
    hub = db.query(Esp32Hub).filter_by(serial=serial).with_for_update().first()
    asset = db.query(Asset).filter_by(serial=serial).with_for_update().first()
    if not asset or asset.kind != 'hub_base':
        raise ProvisionConflict('A supported Base hub label is required')
    if not hub or hub.shop_id or asset.order_id or asset.test_status == 'passed':
        raise ProvisionConflict('A released or claimed hub cannot be reprovisioned')
    if asset.hardware_id and asset.hardware_id != hardware:
        raise ProvisionConflict('This serial is already bound to another physical board')
    if not asset.token_cipher:
        token = secrets.token_urlsafe(32)
        asset.token_cipher = seal(token)
        hub.token_hash = digest(token)
    asset.hardware_id = hardware
    asset.pcb_revision = str(p.get('pcb_revision') or '')[:64]
    asset.firmware_version = str(p.get('firmware_version') or '')[:128]
    audit(db, 'identity_provisioned', 'Device credentials issued for its physical board', serial)
    return dict(device_serial=serial, device_token=unseal(asset.token_cipher),
                backend_url=os.getenv('PUBLIC_BACKEND_URL', 'https://amtech-ai-guard-hub-production.up.railway.app'), hardware_id=hardware)

def register_operations_routes(app, auth_required, owned_shop_or_response, storage_client=None):
    def private(view):
        @wraps(view)
        def wrapped(*args, **kwargs):
            expected = os.getenv('AMTECH_MANAGEMENT_KEY', '')
            if len(expected) < 32:
                return jsonify(ok=False, error='Operations access is not configured'), 503
            if not hmac.compare_digest(request.headers.get('X-AMTECH-OPERATIONS-KEY', '').encode(), expected.encode()):
                return jsonify(ok=False, error='Operations service access required'), 401
            image_upload = request.method == 'PUT' and re.fullmatch(r'/ops/ota/releases/[A-Za-z0-9_.-]{1,64}/image', request.path)
            if request.content_length and request.content_length > (0x400000 if image_upload else 32768):
                return jsonify(ok=False, error='Request too large'), 413
            if request.method == 'POST' and not isinstance(request.get_json(silent=True), dict):
                return jsonify(ok=False, error='A JSON object is required'), 400
            try:
                response = view(*args, **kwargs)
            except ProvisionConflict as exc:
                return jsonify(ok=False, error=str(exc)), 409
            except (ValueError, TypeError) as exc:
                return jsonify(ok=False, error=str(exc)), 400
            except IntegrityError:
                return jsonify(ok=False, error='Record already assigned or changed. Refresh and retry.'), 409
            except (KeyError, InvalidToken) as exc:
                app.logger.error('Operations configuration error type=%s', type(exc).__name__)
                return jsonify(ok=False, error='Label encryption configuration needs attention'), 503
            return response
        return wrapped

    @app.after_request
    def operations_no_cache(response):
        if request.path.startswith('/ops/'):
            response.headers['Cache-Control'] = 'no-store'
        return response

    @app.get('/ops/overview')
    @private
    def overview():
        with SessionLocal() as db:
            rows = db.query(Esp32Hub, Shop, Asset, InstallationLocation).outerjoin(Shop, Esp32Hub.shop_id == Shop.id).outerjoin(Asset, Esp32Hub.serial == Asset.serial).outerjoin(InstallationLocation, Shop.id == InstallationLocation.shop_id).order_by(Esp32Hub.serial).limit(1000).all()
            fleet = []
            for hub, shop, asset, location in rows:
                seen = hub.last_seen_at
                if seen and seen.tzinfo is None:
                    seen = seen.replace(tzinfo=timezone.utc)
                state = hub.reported_state or {}
                fleet.append(dict(serial=hub.serial, kind=asset.kind if asset else 'hub_base',
                    site=shop.shop_name if shop else None, owner=shop.owner_name if shop else None,
                    address=shop.address if shop else None, shop_id=hub.shop_id,
                    latitude=float(location.latitude) if location and location.latitude is not None else None,
                    longitude=float(location.longitude) if location and location.longitude is not None else None,
                    online=bool(seen and (now() - seen).total_seconds() < 45), last_seen_at=iso(seen),
                    state=state, test_status=asset.test_status if asset else 'legacy',
                    hardware_id=asset.hardware_id if asset else None,
                    pcb_revision=asset.pcb_revision if asset else None,
                    manufacturing_batch=asset.manufacturing_batch if asset else None,
                    manufactured_on=asset.manufactured_on if asset else None,
                    firmware_version=state.get('firmware_version') or (asset.firmware_version if asset else None)))
            orders = db.query(PackingOrder).order_by(PackingOrder.created_at.desc()).limit(100).all()
            totals = db.query(Asset.order_id, Asset.kind, func.count(Asset.serial)).filter(Asset.order_id.in_([o.id for o in orders])).group_by(Asset.order_id, Asset.kind).all() if orders else []
            counts = {}
            for order_id, kind, count in totals:
                counts.setdefault(order_id, {})[kind] = count
            summaries = [dict(id=o.id, reference=o.reference, status=o.status, hub_serial=o.hub_serial,
                expected=o.expected, counts={k: counts.get(o.id, {}).get(k, 0) for k in CAPACITY},
                created_at=iso(o.created_at), packed_at=iso(o.packed_at), dispatched_at=iso(o.dispatched_at)) for o in orders]
            return jsonify(ok=True, fleet=fleet, inventory_count=db.query(Asset).count(), orders=summaries,
                server_time=iso(now()), capabilities={'hub_base': CAPACITY, 'hub_high': None})

    @app.get('/ops/assets')
    @private
    def assets():
        with SessionLocal() as db:
            rows = db.query(Asset).order_by(Asset.created_at.desc(), Asset.serial).limit(1000).all()
            batches = db.query(LabelBatch).order_by(LabelBatch.created_at.desc()).limit(100).all()
            return jsonify(ok=True, assets=[asset_dict(a) for a in rows], batches=[dict(id=b.id, kind=b.kind, count=b.count, period=b.period, created_at=iso(b.created_at)) for b in batches])

    @app.post('/ops/batches')
    @private
    def create_batch():
        p = request.get_json(silent=True) or {}
        kind, count, period, identity = p.get('kind'), p.get('count'), p.get('period'), p.get('request_id')
        if kind not in KINDS or type(count) is not int or not 1 <= count <= 50:
            raise ValueError('Choose an item type and 1–50 labels')
        if not isinstance(period, str) or not re.fullmatch(r'[0-9]{2}(0[1-9]|1[0-2])', period):
            raise ValueError('Period must be YYMM, for example 2610')
        if not isinstance(identity, str) or not re.fullmatch(r'[A-Za-z0-9_-]{16,64}', identity):
            raise ValueError('A stable request ID is required')
        batch_name = p.get('manufacturing_batch') or ''
        manufactured_on = p.get('manufactured_on') or ''
        if not isinstance(batch_name, str) or len(batch_name) > 64 or not isinstance(manufactured_on, str):
            raise ValueError('Invalid manufacturing batch or date')
        if manufactured_on:
            datetime.strptime(manufactured_on, '%Y-%m-%d')
        production = {'manufacturing_batch': batch_name.strip(), 'manufactured_on': manufactured_on}
        with SessionLocal() as db:
            existing = db.get(LabelBatch, identity)
            if existing:
                if (existing.kind, existing.count, existing.period) != (kind, count, period):
                    raise ValueError('This request ID belongs to a different label batch')
                if existing.production != production:
                    raise ValueError('This request ID has different manufacturing details')
                return jsonify(ok=True, batch_id=identity), 200
            prefix = f'NET-{KINDS[kind]}-{period}'
            from sqlalchemy.dialects.postgresql import insert as pg_insert
            from sqlalchemy.dialects.sqlite import insert as sqlite_insert
            insert = sqlite_insert if db.bind.dialect.name == 'sqlite' else pg_insert
            db.execute(insert(SerialCounter).values(prefix=prefix, value=0).on_conflict_do_nothing(index_elements=['prefix']))
            counter = db.query(SerialCounter).filter_by(prefix=prefix).with_for_update().one()
            if counter.value + count > 99999:
                raise ValueError('This serial-number range is exhausted')
            db.add(LabelBatch(id=identity, kind=kind, count=count, period=period, production=production))
            db.flush()
            for number in range(counter.value + 1, counter.value + count + 1):
                serial = f'{prefix}-{number:05d}'
                code = secrets.token_urlsafe(24) if kind.startswith('hub_') else None
                db.add(Asset(serial=serial, kind=kind, batch_id=identity, claim_cipher=seal(code) if code else None,
                    manufacturing_batch=production['manufacturing_batch'] or None, manufactured_on=manufactured_on or None))
                if code:
                    db.add(Esp32Hub(serial=serial, token_hash=digest(secrets.token_urlsafe(48)), pairing_code_hash=digest(code)))
            counter.value += count
            audit(db, 'labels_generated', f'{count} {kind.replace("_", " ")} labels reserved', details={'batch_id': identity})
            db.commit()
            return jsonify(ok=True, batch_id=identity), 201

    @app.get('/ops/batches/<batch_id>/labels')
    @private
    def labels(batch_id):
        with SessionLocal() as db:
            batch = db.get(LabelBatch, batch_id)
            if not batch:
                return jsonify(ok=False, error='Batch not found'), 404
            rows = db.query(Asset).filter_by(batch_id=batch_id).order_by(Asset.serial).all()
            result = []
            for asset in rows:
                if asset.kind.startswith('hub_'):
                    if not asset.claim_cipher:
                        continue
                    qr = dict(device_type='esp32', device_serial=asset.serial, pairing_code=unseal(asset.claim_cipher))
                else:
                    qr = dict(amtech_asset=1, serial=asset.serial, kind=asset.kind)
                result.append(dict(serial=asset.serial, kind=asset.kind, qr=json.dumps(qr, separators=(',', ':')), protected=asset.kind.startswith('hub_')))
            audit(db, 'labels_exported', 'Label sheet prepared', details={'batch_id': batch_id, 'count': len(result)})
            db.commit()
            return jsonify(ok=True, labels=result)

    @app.post('/ops/assets/<serial>/provision')
    @private
    def provision(serial):
        p = request.get_json(silent=True) or {}
        with SessionLocal() as db:
            identity = issue_identity(db, serial, p)
            db.commit()
            return jsonify(ok=True, **identity)

    @app.post('/ops/assets/<serial>/test')
    @private
    def eol(serial):
        p = request.get_json(silent=True) or {}
        checks = p.get('checks')
        if not isinstance(checks, dict) or set(checks) != set(EOL_CHECKS) or any(type(v) is not bool for v in checks.values()):
            raise ValueError('Record every required EOL check as passed or failed')
        with SessionLocal() as db:
            hub = db.query(Esp32Hub).filter_by(serial=serial).with_for_update().first()
            asset = db.query(Asset).filter_by(serial=serial).with_for_update().first()
            if not asset or asset.kind != 'hub_base':
                raise ValueError('Select a supported Base hub')
            if hub.shop_id or asset.order_id:
                return jsonify(ok=False, error='Packed or customer-owned units need a service workflow'), 409
            seen = hub.last_seen_at.replace(tzinfo=timezone.utc) if hub.last_seen_at and hub.last_seen_at.tzinfo is None else hub.last_seen_at
            if not asset.hardware_id or not seen or (now() - seen).total_seconds() > 120:
                return jsonify(ok=False, error='Provision this board and verify a recent authenticated hub connection first'), 409
            if checks['identity'] and hub.reported_state.get('hardware_id') != asset.hardware_id:
                return jsonify(ok=False, error='Reported hardware identity does not match the registered board'), 409
            if all(checks.values()):
                state = hub.reported_state or {}
                if state.get('alert_outbox_count') != 0:
                    return jsonify(ok=False, error='Wait for factory events to sync and a confirmed empty alert outbox'), 409
                if state.get('arm_state') != 'disarmed' or state.get('siren_active') or state.get('smoke') != 'normal':
                    return jsonify(ok=False, error='Finish testing with the hub disarmed, siren off and smoke input normal'), 409
            asset.test_status = 'passed' if all(checks.values()) else 'failed'
            asset.test_results = dict(checks=checks, notes=str(p.get('notes') or '')[:1000], recorded_at=iso(now()))
            audit(db, 'eol_' + asset.test_status, 'Final functional test recorded: ' + asset.test_status, serial, {'checks': checks})
            db.commit()
            return jsonify(ok=True, asset=asset_dict(asset))

    @app.post('/ops/orders')
    @private
    def create_order():
        p = request.get_json(silent=True) or {}
        expected = p.get('expected')
        if not isinstance(expected, dict) or set(expected) != set(CAPACITY) or any(type(v) is not int or not 0 <= v <= 20 for v in expected.values()):
            raise ValueError('Specify the camera, shutter and smoke quantities')
        identity = p.get('request_id')
        if not isinstance(identity, str) or not re.fullmatch(r'[A-Za-z0-9_-]{16,64}', identity):
            raise ValueError('A stable request ID is required')
        reference = str(p.get('reference') or f'ORD-{now():%Y%m%d}-{identity[:6].upper()}').strip()
        if not 1 <= len(reference) <= 128:
            raise ValueError('Order reference must be 1–128 characters')
        with SessionLocal() as db:
            order = db.get(PackingOrder, identity)
            if order:
                if order.expected != expected or order.reference != reference:
                    raise ValueError('Request ID belongs to another order')
            else:
                order = PackingOrder(id=identity, reference=reference, expected=expected)
                db.add(order)
                audit(db, 'order_created', 'Packing order created: ' + reference)
                db.commit()
            return jsonify(ok=True, order=order_dict(db, order)), 200

    @app.get('/ops/orders/<order_id>')
    @private
    def get_order(order_id):
        with SessionLocal() as db:
            order = db.get(PackingOrder, order_id)
            return (jsonify(ok=True, order=order_dict(db, order)) if order else (jsonify(ok=False, error='Order not found'), 404))

    @app.delete('/ops/orders/<order_id>')
    @private
    def delete_draft(order_id):
        with SessionLocal() as db:
            # Use the same order lock as scanning and finalization: either the
            # draft is deleted or packing wins; a confirmed package is never lost.
            order = db.query(PackingOrder).filter_by(id=order_id).with_for_update().first()
            if not order:
                return jsonify(ok=True, deleted=False, released_count=0)
            if order.status != 'draft':
                return jsonify(ok=False, error='Only draft packing orders can be deleted'), 409
            items = db.query(Asset).filter_by(order_id=order.id).order_by(Asset.serial).with_for_update().all()
            released = [asset.serial for asset in items]
            for asset in items:
                asset.order_id = None
            audit(db, 'draft_deleted', 'Packing draft deleted: ' + order.reference, order.hub_serial,
                  {'order_id': order.id, 'released_serials': released, 'released_count': len(released)})
            db.delete(order)
            db.commit()
            return jsonify(ok=True, deleted=True, released_count=len(released))

    @app.post('/ops/orders/<order_id>/scan')
    @private
    def scan(order_id):
        p = request.get_json(silent=True) or {}
        serial, expected_kind = parse_scan(p.get('scan')), p.get('kind')
        if expected_kind not in ('hub', *CAPACITY):
            raise ValueError('Select the item type being scanned')
        with SessionLocal() as db:
            order = db.query(PackingOrder).filter_by(id=order_id).with_for_update().first()
            if not order or order.status != 'draft':
                return jsonify(ok=False, error='Open a draft packing order'), 409
            asset = db.query(Asset).filter_by(serial=serial).with_for_update().first()
            if not asset:
                return jsonify(ok=False, error='This label is not registered in AMTECH inventory'), 404
            kind = 'hub' if asset.kind.startswith('hub_') else asset.kind
            if kind != expected_kind:
                raise ValueError(f'Expected {expected_kind}; scanned item is {kind}')
            if asset.order_id == order.id:
                return jsonify(ok=True, duplicate=True, order=order_dict(db, order))
            if asset.order_id:
                return jsonify(ok=False, error='This item is already allocated to another order'), 409
            if kind == 'hub':
                if order.hub_serial:
                    return jsonify(ok=False, error='Finish or remove the current hub first'), 409
                if asset.kind != 'hub_base':
                    return jsonify(ok=False, error='High-End capabilities are not qualified yet'), 409
                if any(order.expected[k] > CAPACITY[k] for k in CAPACITY):
                    return jsonify(ok=False, error='Base supports at most 1 camera, 2 shutter inputs and 1 smoke input'), 409
                if asset.test_status != 'passed':
                    return jsonify(ok=False, error='The hub must pass EOL testing before packing'), 409
                hub = db.get(Esp32Hub, serial)
                if hub.shop_id:
                    return jsonify(ok=False, error='This hub is already customer-owned'), 409
                order.hub_serial = serial
            else:
                if not order.hub_serial:
                    return jsonify(ok=False, error='Scan the hub first'), 409
                count = db.query(Asset).filter_by(order_id=order.id, kind=kind).count()
                if count >= order.expected[kind]:
                    return jsonify(ok=False, error='The required quantity for this item is already complete'), 409
            asset.order_id = order.id
            audit(db, 'item_packed', kind.title() + ' added to ' + order.reference, serial, {'order_id': order.id, 'hub_serial': order.hub_serial})
            db.commit()
            return jsonify(ok=True, duplicate=False, order=order_dict(db, order))

    @app.delete('/ops/orders/<order_id>/items/<serial>')
    @private
    def remove_item(order_id, serial):
        with SessionLocal() as db:
            order = db.query(PackingOrder).filter_by(id=order_id).with_for_update().first()
            if not order or order.status != 'draft':
                return jsonify(ok=False, error='Only draft packages can be changed'), 409
            asset = db.query(Asset).filter_by(serial=serial, order_id=order.id).with_for_update().first()
            if not asset:
                return jsonify(ok=False, error='Item not in this order'), 404
            if order.hub_serial == serial:
                if db.query(Asset).filter(Asset.order_id == order.id, Asset.serial != serial).count():
                    return jsonify(ok=False, error='Remove accessories before removing the hub'), 409
                order.hub_serial = None
            asset.order_id = None
            audit(db, 'item_removed', 'Item removed from ' + order.reference, serial)
            db.commit()
            return jsonify(ok=True, order=order_dict(db, order))

    @app.post('/ops/orders/<order_id>/<action>')
    @private
    def finish_order(order_id, action):
        if action not in ('pack', 'dispatch'):
            return jsonify(ok=False, error='Action not found'), 404
        with SessionLocal() as db:
            order = db.query(PackingOrder).filter_by(id=order_id).with_for_update().first()
            if not order:
                return jsonify(ok=False, error='Order not found'), 404
            if action == 'pack' and order.status == 'draft':
                if not order_dict(db, order)['complete']:
                    return jsonify(ok=False, error='Scan the hub and every required accessory before confirming'), 409
                hub = db.get(Asset, order.hub_serial)
                if hub.test_status != 'passed':
                    return jsonify(ok=False, error='Hub EOL approval is missing'), 409
                order.status, order.packed_at = 'packed', now()
                audit(db, 'package_confirmed', 'Package confirmed: ' + order.reference, order.hub_serial)
            elif action == 'dispatch' and order.status == 'packed':
                order.status, order.dispatched_at = 'dispatched', now()
                audit(db, 'package_dispatched', 'Package dispatched: ' + order.reference, order.hub_serial)
            elif not (action == 'pack' and order.status in ('packed', 'dispatched') or action == 'dispatch' and order.status == 'dispatched'):
                return jsonify(ok=False, error='Confirm packing before dispatch'), 409
            db.commit()
            return jsonify(ok=True, order=order_dict(db, order))

    @app.get('/ops/logs')
    @private
    def logs():
        serial = request.args.get('serial') or None
        if serial and not SERIAL.fullmatch(serial):
            raise ValueError('Invalid hub serial')
        with SessionLocal() as db:
            query = db.query(OperationsEvent)
            if serial:
                query = query.filter_by(serial=serial)
            result = [dict(id=e.id, serial=e.serial, kind=e.kind, message=e.message, details=e.details, timestamp=iso(e.created_at)) for e in query.order_by(OperationsEvent.created_at.desc()).limit(100).all()]
            alerts = db.query(Alert, Esp32Hub.serial).join(Esp32Hub, Alert.shop_id == Esp32Hub.shop_id)
            commands = db.query(Esp32Command)
            if serial:
                alerts = alerts.filter(Esp32Hub.serial == serial)
                commands = commands.filter_by(serial=serial)
            for alert, hub_serial in alerts.order_by(Alert.timestamp.desc()).limit(100).all():
                result.append(dict(id=alert.id, serial=hub_serial, kind=alert.event_type, message=alert.event_type.replace('-', ' ').title(), details=None, timestamp=iso(alert.timestamp)))
            for c in commands.order_by(Esp32Command.created_at.desc()).limit(50).all():
                result.append(dict(id=c.id, serial=c.serial, kind='command_' + c.status, message=f'{c.name.upper()}: {c.status}', details={'reason': c.reason}, timestamp=iso(c.completed_at or c.created_at)))
            return jsonify(ok=True, events=sorted(result, key=lambda e: e['timestamp'] or '', reverse=True)[:150])

    @app.get('/shop/<shop_id>/package')
    @auth_required
    def owned_package(shop_id):
        with SessionLocal() as db:
            _, error = owned_shop_or_response(db, shop_id)
            if error:
                return error
            hub = db.query(Esp32Hub).filter_by(shop_id=shop_id).first()
            return jsonify(ok=True, package=package_for_hub(db, hub.serial) if hub else None)

    from factory_api import register_factory_routes
    register_factory_routes(app, private, issue_identity)
    from ota_api import register_ota_routes
    register_ota_routes(app, private, storage_client)
