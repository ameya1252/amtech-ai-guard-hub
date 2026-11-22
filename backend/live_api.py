"""Owner-authorized live viewing; independent of security commands and evidence."""
import os
import re
from urllib.parse import urlsplit

from flask import g, jsonify, request
import requests

from database import SessionLocal, EvidenceRecorder
from esp32_api import hub_for_shop
from esp32_camera_api import revisions

SESSION = re.compile(r'^[A-Za-z0-9_-]{43}$')


def media_configuration():
    origin = os.getenv('NETTRA_LIVE_MEDIA_ORIGIN','').rstrip('/')
    key = os.getenv('NETTRA_LIVE_CONTROL_KEY','')
    url = urlsplit(origin)
    if (url.scheme != 'https' or not url.hostname or url.username or url.password or url.path
            or url.query or url.fragment or len(key) < 32):
        return None
    return origin, key


def register_live_routes(app, auth_required, owned_shop_or_response):
    @app.route('/shop/<shop_id>/cameras/<int:camera_id>/live', methods=['POST'])
    @app.route('/shop/<shop_id>/cameras/<int:camera_id>/live/<session_id>', methods=['DELETE'])
    @auth_required
    def live_view(shop_id, camera_id, session_id=None):
        with SessionLocal() as db:
            shop, error = owned_shop_or_response(db, shop_id)
            if error: return error
            if camera_id not in (1,2) or (session_id is not None and not SESSION.fullmatch(session_id)):
                return jsonify(ok=False,error='Invalid camera or live session.'),400
            hub = hub_for_shop(db, shop_id)
            if not hub: return jsonify(ok=False,error='Live viewing requires a paired ESP32 hub.'),409
            state = hub.reported_state or {}
            camera = next((c for c in state.get('cameras',[]) if c.get('camera_id') == camera_id), None)
            enabled = camera.get('enabled',False) if camera else camera_id == 1
            recorder = db.get(EvidenceRecorder,hub.serial)
            if request.method == 'POST' and (not enabled or not recorder or not recorder.enabled
                    or recorder.shop_id != shop_id or camera_id not in recorder.camera_ids):
                return jsonify(ok=False,error='Live viewing is not available for this camera yet.'),409
            scope = dict(user_id=g.user_id,shop_id=shop_id,serial=hub.serial,camera_id=camera_id,
                         revision=revisions(state,camera_id)[0])
        config = media_configuration()
        if not config: return jsonify(ok=False,error='Live viewing is not available yet.'),503
        origin,key = config
        try:
            result = requests.request(request.method,origin+'/internal/live/sessions'+('/'+session_id if session_id else ''),
                json=scope,headers={'Authorization':'Bearer '+key},timeout=(2,5),allow_redirects=False)
            if result.status_code not in (200,201):
                status = result.status_code if result.status_code in (409,429,503) else 503
                message = 'Live viewing is busy. Close another viewer and retry.' if status == 429 else 'Live video is unavailable. Try again shortly.'
                return jsonify(ok=False,error=message),status
            if request.method == 'DELETE': response=jsonify(ok=True)
            else:
                value = result.json()
                if (not isinstance(value.get('session_id'),str) or not SESSION.fullmatch(value['session_id'])
                        or value.get('duration_seconds') != 180 or not isinstance(value.get('expires_at'),str)):
                    raise ValueError('Invalid media grant')
                response = jsonify(ok=True,session_id=value['session_id'],camera_id=camera_id,
                    expires_at=value['expires_at'],duration_seconds=180,
                    url=origin+'/live/'+value['session_id']+'/index.m3u8')
            response.headers['Cache-Control']='private, no-store'
            return response,201 if request.method == 'POST' else 200
        except (requests.RequestException,ValueError,TypeError):
            return jsonify(ok=False,error='Live video is unavailable. Try again shortly.'),503
