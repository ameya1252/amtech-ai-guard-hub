"""Technician-scoped setup of two camera slots, with legacy Camera 2 compatibility."""
import ipaddress
import json
import re
from datetime import datetime, timezone
from uuid import uuid4
from cryptography import x509
from flask import g, jsonify, request
from sqlalchemy.exc import IntegrityError
from database import Esp32CameraConfig, Esp32PrimaryCameraConfig, Esp32Command, SessionLocal
from operations_api import cipher, audit
from technician_access import technician_access, readable_shop
PRIVATE_NETWORKS = tuple(ipaddress.ip_network(n) for n in ('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16'))


def supports_cameras(state):
    return (state or {}).get('camera_protocol_version') == 1 and (state or {}).get('camera_capacity') == 2


def model(camera_id):
    return Esp32PrimaryCameraConfig if camera_id == 1 else Esp32CameraConfig


def seal(serial, payload):
    prefix = b'NETTRA-CAMERA1\0' if payload.get('camera_id') == 1 else b'NETTRA-CAMERA2\0'
    return cipher().encrypt(prefix + serial.encode() + b'\0' + json.dumps(payload).encode()).decode()


def unseal(row):
    camera_id = 1 if isinstance(row, Esp32PrimaryCameraConfig) else 2
    prefix = (b'NETTRA-CAMERA1\0' if camera_id == 1 else b'NETTRA-CAMERA2\0') + row.serial.encode() + b'\0'
    data = cipher().decrypt(row.encrypted_config.encode())
    if not data.startswith(prefix): raise ValueError('Camera configuration identity mismatch')
    value = json.loads(data[len(prefix):])
    if value.get('camera_id') != camera_id: raise ValueError('Camera slot mismatch')
    return value


def revisions(state, camera_id):
    if camera_id == 1:
        return state.get('camera_1_config_revision','factory'), state.get('camera_1_config_staged_revision','factory')
    return state.get('camera_config_revision','factory'), state.get('camera_config_staged_revision','factory')


def supported(state, camera_id):
    return supports_cameras(state) and (camera_id == 2 or state.get('camera_config_protocol_version') == 2)


def sync_camera_config(db, hub):
    state = hub.reported_state or {}
    if state.get('arm_state') != 'disarmed' or state.get('siren_active'): return None
    for camera_id in (1,2):
        if not supported(state,camera_id): continue
        row=db.get(model(camera_id),hub.serial)
        if row and revisions(state,camera_id)[1] != row.revision: return unseal(row)
    return None


def validate_input(payload, previous, camera_id=2):
    if not isinstance(payload, dict) or type(payload.get('enabled')) is not bool:
        raise ValueError('Choose whether Camera 2 is enabled')
    result = {'camera_id': camera_id, 'enabled': payload['enabled']}
    if not result['enabled']:
        if camera_id == 1: raise ValueError('Camera 1 must remain enabled')
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
    @app.route('/shop/<shop_id>/esp32/camera-<int:camera_id>', methods=['GET','PUT'])
    @auth_required
    def camera_config(shop_id, camera_id):
        from esp32_api import ACTIVE, hub_for_shop, online
        if camera_id not in (1,2): return jsonify(ok=False,error='Invalid camera slot'),404
        with SessionLocal() as db:
            shop=readable_shop(db,shop_id,g.user_id)
            if not shop: return jsonify(ok=False,error='Site access required'),403
            hub=hub_for_shop(db,shop_id,lock=True)
            if not hub: return jsonify(ok=False,error='ESP32 hub required'),409
            state=hub.reported_state or {}
            row=db.get(model(camera_id),hub.serial)
            previous=unseal(row) if row else None
            permission=technician_access(db,shop_id,g.user_id)
            compatible=supported(state,camera_id)
            if request.method=='GET':
                fallback=next((c for c in state.get('cameras',[]) if c['camera_id']==camera_id),{})
                value=previous or dict(camera_id=camera_id,enabled=camera_id==1,host=fallback.get('host',''),port=fallback.get('port',443))
                public={k:v for k,v in value.items() if k!='password'}
                active,staged=revisions(state,camera_id)
                public.update(password_saved=bool(value.get('password')),revision=row.revision if row else 'factory',
                              supported=compatible,hub_online=online(hub),active_revision=active,staged_revision=staged,
                              config_error=state.get('camera_config_error'),technician_access=permission,
                              can_edit=permission and compatible and online(hub) and state.get('arm_state')=='disarmed' and not state.get('siren_active'))
                return jsonify(ok=True,camera=public)
            if not permission: return jsonify(ok=False,error='AMTECH technician access is required for camera changes'),403
            if not compatible: return jsonify(ok=False,error='Update hub firmware before configuring this camera'),409
            if not online(hub) or state.get('arm_state')!='disarmed' or state.get('siren_active'):
                return jsonify(ok=False,error='The hub must be online, disarmed and idle before camera setup'),409
            if db.query(Esp32Command).filter(Esp32Command.serial==hub.serial,Esp32Command.status.in_(ACTIVE)).first():
                return jsonify(ok=False,error='Wait for the current hub command to finish'),409
            if request.content_length and request.content_length>8192: return jsonify(ok=False,error='Camera configuration too large'),413
            payload=request.get_json(silent=True)
            if not isinstance(payload,dict) or payload.get('expected_revision')!=(row.revision if row else 'factory'):
                return jsonify(ok=False,error='Camera settings changed; reload before saving'),409
            try:
                config=validate_input(payload,previous,camera_id)
                other_id=3-camera_id
                other=next((c for c in state.get('cameras',[]) if c['camera_id']==other_id),{})
                other_row=db.get(model(other_id),hub.serial)
                candidates=[other]+([unseal(other_row)] if other_row else [])
                if config['enabled'] and any(c.get('enabled') and config['host']==c.get('host') and config['port']==c.get('port') for c in candidates):
                    raise ValueError('Each camera must use a different address and port')
                mac=payload.get('mac_address','')
                if not isinstance(mac,str) or (mac and not re.fullmatch(r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}',mac)):
                    raise ValueError('Use a MAC address such as AA:BB:CC:DD:EE:FF, or leave it blank')
                config.update(revision=uuid4().hex,mac_address=mac.upper())
                encrypted=seal(hub.serial,config)
            except ValueError as exc: return jsonify(ok=False,error=str(exc)),400
            except KeyError: return jsonify(ok=False,error='Camera encryption requires service'),503
            if row is None: row=model(camera_id)(serial=hub.serial);db.add(row)
            row.revision,row.encrypted_config=config['revision'],encrypted
            row.updated_at=datetime.now(timezone.utc)
            audit(db,'camera.updated','Technician changed camera configuration',hub.serial,
                  dict(camera_id=camera_id,revision=row.revision,technician_id=g.user_id))
            try: db.commit()
            except IntegrityError: return jsonify(ok=False,error='Camera settings changed; reload before saving'),409
            return jsonify(ok=True,revision=config['revision'])
