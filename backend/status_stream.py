"""Bounded status fan-out for the single-worker Railway pilot; no broker service.

Only authenticated site owners subscribe. Reconnect always takes an authoritative
snapshot. Queues coalesce updates instead of blocking the device sync request.
"""
import json
import queue
import threading
import time
from collections import defaultdict

_lock = threading.Lock()
_subscribers = defaultdict(set)
MAX_CONNECTIONS = 16
_connection_slots = threading.BoundedSemaphore(MAX_CONNECTIONS)


def publish_status(shop_id, status, owner_id=None):
    with _lock:
        listeners = list(_subscribers.get(shop_id, ()))
    for inbox in listeners:
        try:
            inbox.get_nowait()
        except queue.Empty:
            pass
        try:
            inbox.put_nowait(dict(status, _stream_owner_id=owner_id) if owner_id else status)
        except queue.Full:
            pass


def has_subscribers(shop_id):
    with _lock: return bool(_subscribers.get(shop_id))


def subscribe(shop_id):
    inbox = queue.Queue(maxsize=1)
    with _lock:
        if sum(map(len, _subscribers.values())) >= MAX_CONNECTIONS:
            raise RuntimeError('Status stream capacity reached')
        _subscribers[shop_id].add(inbox)
    return inbox


def unsubscribe(shop_id, inbox):
    with _lock:
        _subscribers[shop_id].discard(inbox)
        if not _subscribers[shop_id]:
            del _subscribers[shop_id]


def register_status_stream(app):
    import jwt
    from flask_sock import Sock
    from database import SessionLocal, Shop, User
    from esp32_api import hub_for_shop, status_dict
    import os
    from simple_websocket import ConnectionClosed
    sock = Sock(app)

    @sock.route('/shop/<shop_id>/status-stream')
    def stream(ws, shop_id):
        inbox = None
        if not _connection_slots.acquire(blocking=False):
            ws.close(reason=1013,message='Status stream busy'); return
        try:
            first = ws.receive(timeout=10)
            if not isinstance(first, str) or len(first) > 8192:
                ws.close(reason=1008, message='Authentication required'); return
            envelope=json.loads(first)
            if not isinstance(envelope,dict):
                ws.close(reason=1008,message='Authentication required'); return
            token = envelope.get('token')
            claims = jwt.decode(token, os.environ['JWT_SECRET'], algorithms=['HS256'])
            user_id = claims.get('sub')
            if not user_id:
                ws.close(reason=1008, message='Authentication required'); return
            def snapshot():
                with SessionLocal() as db:
                    shop = db.get(Shop, shop_id)
                    if not db.get(User, user_id) or not shop or shop.user_id != user_id:
                        return None
                    hub=hub_for_shop(db,shop_id,lock=True)
                    if hub is None: return None
                    value=status_dict(db,shop,hub)
                    db.commit()
                    return value
            state = snapshot()
            if state is None:
                ws.close(reason=1008, message='Site unavailable'); return
            inbox = subscribe(shop_id)
            ws.send(json.dumps({'type': 'status', 'status': state}))
            checked_at = time.monotonic()
            refresh_at = time.monotonic() + 1800
            while ws.connected:
                # Recheck expiry on every update; never retain an expired session.
                if 'exp' in claims and claims['exp'] <= time.time():
                    ws.close(reason=1008, message='Session expired'); break
                if time.monotonic() >= refresh_at:
                    ws.close(reason=1012, message='Reconnect to refresh session'); break
                try:
                    state = inbox.get(timeout=5)
                except queue.Empty:
                    state = snapshot()  # Also transitions a silent hub to offline.
                    checked_at = time.monotonic()
                if time.monotonic() - checked_at >= 5:
                    if snapshot() is None:
                        ws.close(reason=1008, message='Site unavailable'); break
                    checked_at = time.monotonic()
                if state is None:
                    ws.close(reason=1008, message='Site unavailable'); break
                owner_id=state.get('_stream_owner_id',user_id)
                if owner_id!=user_id:
                    ws.close(reason=1008,message='Site unavailable'); break
                public={key:value for key,value in state.items() if key!='_stream_owner_id'}
                ws.send(json.dumps({'type': 'status', 'status': public}))
        except ConnectionClosed:
            pass
        except (jwt.PyJWTError, ValueError, TypeError, RuntimeError):
            ws.close(reason=1008, message='Status stream unavailable')
        finally:
            if inbox is not None:
                unsubscribe(shop_id, inbox)
            _connection_slots.release()
