"""Owner-scoped outbound recipients; no SMS control privileges are granted."""
import re
from uuid import uuid4
from flask import jsonify, request
from database import Esp32ContactConfig, Esp32Command, SessionLocal
from operations_api import audit


def validate_contacts(value):
    if not isinstance(value, list) or not 1 <= len(value) <= 3:
        raise ValueError('Keep between one and three emergency contacts')
    result = []
    for item in value:
        if not isinstance(item, dict):
            raise ValueError('Invalid emergency contact')
        name, phone, slot = item.get('name', ''), item.get('phone'), item.get('slot')
        if not isinstance(name, str) or len(name) > 80:
            raise ValueError('Contact name must be at most 80 characters')
        if not isinstance(phone, str) or not re.fullmatch(r'\+[1-9][0-9]{7,14}', phone):
            raise ValueError('Use an international phone number beginning with + and country code')
        if type(slot) is not int or not 1 <= slot <= 3 or any(c['slot'] == slot or c['phone'] == phone for c in result):
            raise ValueError('Contact slots and phone numbers must be unique')
        result.append(dict(slot=slot, name=name.strip(), phone=phone))
    result.sort(key=lambda c: c['slot'])
    # The modem list is compact; slots identify each displayed recipient.
    return [dict(c, slot=i+1) for i, c in enumerate(result)]


def supports_contacts(state):
    return (state or {}).get('contact_protocol_version') == 1


def sync_contacts(db, hub):
    state = hub.reported_state or {}
    if not supports_contacts(state) or state.get('arm_state') != 'disarmed' or state.get('siren_active'):
        return None
    row = db.get(Esp32ContactConfig, hub.serial)
    if row is None or state.get('contact_config_revision') == row.revision:
        return None
    return dict(revision=row.revision, contacts=row.contacts)


def register_contact_routes(app, auth_required, owned_shop_or_response):
    @app.route('/shop/<shop_id>/esp32/contacts', methods=['GET', 'PUT'])
    @auth_required
    def contacts(shop_id):
        from esp32_api import ACTIVE, hub_for_shop, online
        with SessionLocal() as db:
            shop, error = owned_shop_or_response(db, shop_id)
            if error:
                return error
            hub = hub_for_shop(db, shop_id, lock=True)
            if hub is None:
                return jsonify(ok=False, error='ESP32 hub required'), 409
            state = hub.reported_state or {}
            row = db.get(Esp32ContactConfig, hub.serial)
            if request.method == 'GET':
                active = [dict(c, name='') for c in state.get('alert_contacts', [])]
                target = row.contacts if row else active
                consistent = [c['phone'] for c in target] == [c['phone'] for c in active]
                active_revision = state.get('contact_config_revision', 'factory')
                error=state.get('contact_config_error')
                if row and active_revision==row.revision and not consistent:
                    error='Hub contact readback differs from the requested numbers. Reload and save again.'
                return jsonify(ok=True, shop_id=shop_id, supported=supports_contacts(state),
                               revision=row.revision if row else 'factory', active_revision=active_revision,
                               emergency_contacts=target, applied_contacts=active,
                               applied=bool(not row or (active_revision == row.revision and consistent)),
                               config_error=error,
                               can_edit=online(hub) and state.get('arm_state') == 'disarmed' and not state.get('siren_active'))
            if not supports_contacts(state):
                return jsonify(ok=False, error='Update the hub firmware before editing contacts'), 409
            if not online(hub) or state.get('arm_state') != 'disarmed' or state.get('siren_active'):
                return jsonify(ok=False, error='Contacts require an online, disarmed, idle hub'), 409
            if db.query(Esp32Command).filter(Esp32Command.serial == hub.serial, Esp32Command.status.in_(ACTIVE)).first():
                return jsonify(ok=False, error='Wait for the current command to finish'), 409
            payload = request.get_json(silent=True)
            if request.content_length and request.content_length > 4096:
                return jsonify(ok=False, error='Contact configuration too large'), 413
            if not isinstance(payload, dict):
                return jsonify(ok=False,error='A JSON contact configuration is required'),400
            if payload.get('expected_revision') != (row.revision if row else 'factory'):
                return jsonify(ok=False, error='Contacts changed; reload before saving'), 409
            if row and state.get('contact_config_revision') != row.revision and not state.get('contact_config_error'):
                return jsonify(ok=False, error='Wait for the hub to confirm the previous contact change'), 409
            try:
                value = validate_contacts(payload.get('contacts'))
            except ValueError as exc:
                return jsonify(ok=False, error=str(exc)), 400
            if row is None:
                row = Esp32ContactConfig(serial=hub.serial)
                db.add(row)
            row.revision, row.contacts = uuid4().hex, value
            audit(db, 'contacts.updated', 'Outbound emergency contacts changed', hub.serial,
                  dict(revision=row.revision, count=len(value)))
            db.commit()
            return jsonify(ok=True, revision=row.revision, applied=False)
