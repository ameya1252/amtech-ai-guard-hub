"""Owner-scoped Camera 2 setup. No camera connections or alarm commands on the server."""
import ipaddress
import json
import re
from datetime import datetime, timezone
from uuid import uuid4

from cryptography import x509
from cryptography.fernet import InvalidToken
from flask import jsonify, request
from sqlalchemy.exc import IntegrityError

from database import Esp32CameraConfig, Esp32Command, SessionLocal
from operations_api import cipher

PRIVATE_NETWORKS = tuple(ipaddress.ip_network(n) for n in ('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16'))


def supports_cameras(state):
    return (state or {}).get('camera_protocol_version') == 1 and (state or {}).get('camera_capacity') == 2


def seal(serial, payload):
    return cipher().encrypt(b'NETTRA-CAMERA2\0' + serial.encode() + b'\0' + json.dumps(payload).encode()).decode()


def unseal(row):
    prefix = b'NETTRA-CAMERA2\0' + row.serial.encode() + b'\0'
    data = cipher().decrypt(row.encrypted_config.encode())
    if not data.startswith(prefix):
        raise ValueError('Camera configuration identity mismatch')
    return json.loads(data[len(prefix):])


def sync_camera_config(db, hub):
    state = hub.reported_state or {}
    if not supports_cameras(state) or state.get('arm_state') != 'disarmed' or state.get('siren_active'):
        return None
    row = db.get(Esp32CameraConfig, hub.serial)
    if row is None or state.get('camera_config_staged_revision') == row.revision:
        return None
    # Only a device-authenticated HTTPS sync receives the private credentials.
    return unseal(row)


def validate_input(payload, previous):
    if not isinstance(payload, dict) or type(payload.get('enabled')) is not bool:
        raise ValueError('Choose whether Camera 2 is enabled')
    result = {'camera_id': 2, 'enabled': payload['enabled']}
    if not result['enabled']:
        return result
    host, port = payload.get('host'), payload.get('port', 443)
    if not isinstance(host, str):
        raise ValueError('Enter the camera local IPv4 address')
    try:
        address = ipaddress.IPv4Address(host)
    except ipaddress.AddressValueError:
        raise ValueError('Enter the camera local IPv4 address') from None
    if not any(address in network for network in PRIVATE_NETWORKS) or int(str(address).split('.')[-1]) in (0, 255):
        raise ValueError('Camera address must be on a private IPv4 network')
    if type(port) is not int or not 1 <= port <= 65535:
        raise ValueError('Camera HTTPS port must be 1–65535')
    username = payload.get('username')
    if not isinstance(username, str) or not username.strip() or len(username.encode()) > 64 or any(ord(c) < 32 for c in username):
        raise ValueError('Enter the camera username (up to 64 bytes)')
    result.update(host=str(address), port=port, username=username.strip())
    same_camera = bool(previous and previous.get('enabled') and
                       all(previous.get(k) == result[k] for k in ('host', 'port', 'username')))
    password = payload.get('password')
    if password in (None, '') and same_camera:
        password = previous.get('password')
    if not isinstance(password, str) or not password or len(password.encode()) > 128 or any(ord(c) < 32 for c in password):
        raise ValueError('Enter the camera password (up to 128 bytes); re-enter it when changing the camera address or username')
    pem = payload.get('ca_pem')
    if pem in (None, '') and same_camera:
        pem = previous.get('ca_pem')
    if not isinstance(pem, str) or len(pem.encode()) > 3072 or not re.fullmatch(
            r'-----BEGIN CERTIFICATE-----\s+[A-Za-z0-9+/=\s]+-----END CERTIFICATE-----\s*', pem):
        raise ValueError('Provide one trusted camera certificate in PEM format (up to 3072 bytes)')
    try:
        x509.load_pem_x509_certificate(pem.encode())
    except ValueError:
        raise ValueError('The camera certificate could not be read') from None
    result.update(password=password, ca_pem=pem)
    return result


def register_camera_config_routes(app, auth_required, owned_shop_or_response):
    @app.route('/shop/<shop_id>/esp32/camera-2', methods=['GET', 'PUT'])
    @auth_required
    def camera_config(shop_id):
        from esp32_api import ACTIVE, hub_for_shop, online
        with SessionLocal() as db:
            shop, error = owned_shop_or_response(db, shop_id)
            if error:
                return error
            hub = hub_for_shop(db, shop_id, lock=request.method == 'PUT')
            if hub is None:
                return jsonify(ok=False, error='This camera setup requires an ESP32 hub'), 409
            state = hub.reported_state or {}
            supported = supports_cameras(state)
            row = db.get(Esp32CameraConfig, hub.serial)
            try:
                previous = unseal(row) if row else None
            except (InvalidToken, ValueError, KeyError):
                return jsonify(ok=False, error='Camera configuration storage needs attention'), 503
            if request.method == 'GET':
                value = previous or {'camera_id': 2, 'enabled': False}
                public = {k: v for k, v in value.items() if k != 'password'}
                public.update(password_saved=bool(value.get('password')), revision=row.revision if row else 'factory',
                              supported=supported, hub_online=online(hub),
                              active_revision=state.get('camera_config_revision', 'factory'),
                              staged_revision=state.get('camera_config_staged_revision', 'factory'),
                              config_error=state.get('camera_config_error'),
                              can_edit=supported and online(hub) and state.get('arm_state') == 'disarmed' and not state.get('siren_active'))
                return jsonify(ok=True, camera=public)
            if not supported:
                return jsonify(ok=False, error='Update the hub firmware before configuring Camera 2'), 409
            if not online(hub) or state.get('arm_state') != 'disarmed' or state.get('siren_active'):
                return jsonify(ok=False, error='The hub must be online, disarmed and idle before camera setup'), 409
            if db.query(Esp32Command).filter(Esp32Command.serial == hub.serial, Esp32Command.status.in_(ACTIVE)).first():
                return jsonify(ok=False, error='Wait for the current hub command to finish'), 409
            if request.content_length and request.content_length > 8192:
                return jsonify(ok=False, error='Camera configuration is too large'), 413
            payload = request.get_json(silent=True)
            if not isinstance(payload, dict) or payload.get('expected_revision') != (row.revision if row else 'factory'):
                return jsonify(ok=False, error='Camera settings changed; reload before saving'), 409
            try:
                config = validate_input(payload, previous)
                primary = next((c for c in state.get('cameras', []) if c.get('camera_id') == 1), {})
                if config['enabled'] and config['host'] == primary.get('host') and config['port'] == primary.get('port'):
                    raise ValueError('Camera 2 must use a different address and port from Camera 1')
                config['revision'] = uuid4().hex
                encrypted = seal(hub.serial, config)
            except ValueError as exc:
                return jsonify(ok=False, error=str(exc)), 400
            except KeyError:
                return jsonify(ok=False, error='Camera configuration encryption needs attention'), 503
            if row is None:
                row = Esp32CameraConfig(serial=hub.serial)
                db.add(row)
            row.revision, row.encrypted_config = config['revision'], encrypted
            row.updated_at = datetime.now(timezone.utc)
            try:
                db.commit()
            except IntegrityError:
                return jsonify(ok=False, error='Camera settings changed; reload before saving'), 409
            return jsonify(ok=True, revision=config['revision'])
