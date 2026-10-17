"""Durable alarm pushes and phone-scoped logout; isolated SQLite, no network."""
import unittest
from datetime import datetime, timedelta, timezone
from unittest.mock import Mock, patch

import test_esp32 as fixture
from database import AlarmPushDelivery, Alert, Esp32EventReceipt, PushToken, SessionLocal, Shop, engine
from alarm_push import deliver_pending_alarm_pushes, MAX_ATTEMPTS

OK = {"data": [{"status": "ok", "id": "expo-ticket"}]}


class AlarmPushTests(unittest.TestCase):
    def setUp(self):
        self.fixture = fixture.Esp32Tests(methodName='test_offline_and_unsupported_configuration')
        self.fixture.setUp()
        self.client, self.auth, self.shop_id = self.fixture.client, self.fixture.auth, self.fixture.shop_id
        self.add_phone('one')

    def add_phone(self, name):
        with SessionLocal() as db:
            owner = db.get(Shop, self.shop_id).user_id
            db.add(PushToken(id=name, user_id=owner, expo_push_token=f'ExpoPushToken[{name}]'))
            db.commit()

    def event(self, kind='smoke', event_id='alarm-one'):
        return self.fixture.sync(events=[{'id': event_id, 'event_type': kind}])

    def make_due(self):
        with SessionLocal() as db:
            db.query(AlarmPushDelivery).filter(AlarmPushDelivery.status.in_(('pending', 'sending'))).update(
                {'next_attempt_at': datetime.now(timezone.utc) - timedelta(seconds=1)})
            db.commit()

    def test_failure_survives_restart_without_hub_replaying_event(self):
        response = self.event()
        self.assertEqual(response.json['accepted_event_ids'], ['alarm-one'])
        push = Mock(side_effect=TimeoutError)
        deliver_pending_alarm_pushes(push)
        with SessionLocal() as db:
            self.assertEqual(db.query(Alert).count(), 1)
            self.assertEqual(db.query(Esp32EventReceipt).count(), 1)
            self.assertEqual(db.query(AlarmPushDelivery).one().status, 'pending')
        deliver_pending_alarm_pushes(push)
        self.assertEqual(push.call_count, 1, 'backoff prevents immediate repeated sends')
        engine.dispose()  # Reopen persisted jobs, as a new process would.
        self.make_due()
        push.side_effect, push.return_value = None, OK
        deliver_pending_alarm_pushes(push)
        self.fixture.sync()  # No old event supplied.
        deliver_pending_alarm_pushes(push)
        self.assertEqual(push.call_count, 2)
        with SessionLocal() as db:
            row = db.query(AlarmPushDelivery).one()
            self.assertEqual((row.status, row.ticket_id), ('sent', 'expo-ticket'))

    def test_successful_phone_is_not_retried_when_another_phone_fails(self):
        self.add_phone('two')
        self.event('intrusion')
        calls = []
        def send(messages):
            calls.append(messages[0]['to'])
            return OK if messages[0]['to'].endswith('[one]') or len(calls) > 2 else {
                'data': [{'status': 'error', 'details': {'error': 'MessageRateExceeded'}}]}
        deliver_pending_alarm_pushes(send)
        self.make_due()
        deliver_pending_alarm_pushes(send)
        self.assertEqual(calls.count('ExpoPushToken[one]'), 1)
        self.assertEqual(calls.count('ExpoPushToken[two]'), 2)

    def test_http_success_with_rejected_or_missing_ticket_is_retried(self):
        for result in ({'data': [{'status': 'error', 'details': {'error': 'MessageRateExceeded'}}]},
                       {'data': [{'status': 'ok'}]}, {}, None):
            with self.subTest(result=result):
                self.setUp()
                self.event('panic')
                deliver_pending_alarm_pushes(Mock(return_value=result))
                with SessionLocal() as db:
                    self.assertEqual(db.query(AlarmPushDelivery).one().status, 'pending')

    def test_dead_token_removed_and_simulation_not_claimed_as_real_send(self):
        self.event()
        deliver_pending_alarm_pushes(Mock(return_value={
            'data': [{'status': 'error', 'details': {'error': 'DeviceNotRegistered'}}]}))
        with SessionLocal() as db:
            self.assertEqual(db.query(PushToken).count(), 0)
            self.assertEqual(db.query(AlarmPushDelivery).one().status, 'failed')
        self.add_phone('other')
        self.event(event_id='alarm-two')
        deliver_pending_alarm_pushes(Mock(return_value={'simulated': True}))
        with SessionLocal() as db:
            self.assertEqual(db.query(AlarmPushDelivery).filter_by(token_id='other').one().status, 'simulated')

    def test_claim_serializes_workers_and_abandoned_lease_is_recovered(self):
        self.event()
        nested = Mock(return_value=OK)
        def send(messages):
            deliver_pending_alarm_pushes(nested)
            return OK
        deliver_pending_alarm_pushes(send)
        nested.assert_not_called()
        self.event(event_id='abandoned')
        with SessionLocal() as db:
            row = db.query(AlarmPushDelivery).filter_by(status='pending').one()
            row.status, row.lease_id = 'sending', 'dead-process'
            row.next_attempt_at = datetime.now(timezone.utc) - timedelta(seconds=1)
            db.commit()
        deliver_pending_alarm_pushes(nested)
        self.assertEqual(nested.call_count, 1)

    def test_logout_cancels_only_this_phone_and_is_idempotent(self):
        self.add_phone('two')
        self.event()
        payload = {'expo_push_token': 'ExpoPushToken[one]'}
        self.assertEqual(self.client.delete('/me/push-token', json=payload).status_code, 401)
        for _ in range(2):
            self.assertEqual(self.client.delete('/me/push-token', headers=self.auth, json=payload).status_code, 200)
        push = Mock(return_value=OK)
        deliver_pending_alarm_pushes(push)
        self.assertEqual(push.call_args.args[0][0]['to'], 'ExpoPushToken[two]')
        self.assertEqual(push.call_count, 1)
        with SessionLocal() as db:
            self.assertIsNone(db.get(PushToken, 'one'))
            self.assertIsNotNone(db.get(PushToken, 'two'))
            self.assertEqual(db.query(AlarmPushDelivery).filter_by(token_id='one').one().status, 'cancelled')

    def test_other_owner_cannot_unregister_and_queued_push_does_not_follow_account_switch(self):
        other = self.client.post('/auth/signup', json=dict(email='other@example.com',
            password='correct-horse-battery-staple', phone_number='+918550991122'))
        other_auth = {'Authorization': f"Bearer {other.json['token']}"}
        self.event()
        payload = {'expo_push_token': 'ExpoPushToken[one]'}
        self.client.delete('/me/push-token', headers=other_auth, json=payload)
        with SessionLocal() as db:
            self.assertIsNotNone(db.get(PushToken, 'one'))
        self.client.post('/me/push-token', headers=other_auth, json=payload)
        push = Mock(return_value=OK)
        deliver_pending_alarm_pushes(push)
        push.assert_not_called()
        # Old owner cannot delete the new owner's registration either.
        self.client.delete('/me/push-token', headers=self.auth, json=payload)
        with SessionLocal() as db:
            self.assertIsNotNone(db.get(PushToken, 'one'))

    def test_logout_during_an_inflight_send_keeps_the_job_cancelled(self):
        self.event()
        def send(messages):
            response = self.client.delete('/me/push-token', headers=self.auth,
                                          json={'expo_push_token': 'ExpoPushToken[one]'})
            self.assertEqual(response.status_code, 200)
            return OK  # An already submitted notification cannot be recalled.
        deliver_pending_alarm_pushes(send)
        with SessionLocal() as db:
            self.assertEqual(db.query(AlarmPushDelivery).one().status, 'cancelled')
        push = Mock(return_value=OK)
        deliver_pending_alarm_pushes(push)
        push.assert_not_called()

    def test_unregister_validates_input_and_supports_browser_preflight(self):
        for payload in (None, {}, {'expo_push_token': 'bad-token'}):
            response = self.client.delete('/me/push-token', headers=self.auth, json=payload)
            self.assertEqual(response.status_code, 400)
        response = self.client.options('/me/push-token', headers={
            'Origin': 'http://localhost:4173', 'Access-Control-Request-Method': 'DELETE'})
        self.assertIn('DELETE', response.headers['Access-Control-Allow-Methods'])

    def test_retry_is_bounded_and_stale_jobs_expire(self):
        self.event()
        with SessionLocal() as db:
            db.query(AlarmPushDelivery).update({'attempts': MAX_ATTEMPTS - 1})
            db.commit()
        push = Mock(side_effect=TimeoutError)
        deliver_pending_alarm_pushes(push)
        self.make_due()
        deliver_pending_alarm_pushes(push)
        self.assertEqual(push.call_count, 1)
        self.event(event_id='expired')
        with SessionLocal() as db:
            db.query(AlarmPushDelivery).filter_by(status='pending').update({
                'expires_at': datetime.now(timezone.utc) - timedelta(seconds=1)})
            db.commit()
        deliver_pending_alarm_pushes(push)
        self.assertEqual(push.call_count, 1)

    def test_outbox_failure_rolls_back_event_receipt_and_history(self):
        self.client.application.config['TESTING'] = True
        try:
            with patch('esp32_api.enqueue_alarm_pushes', side_effect=RuntimeError('local simulated write failure')):
                with self.assertRaises(RuntimeError):
                    self.event()
            with SessionLocal() as db:
                self.assertEqual(db.query(Alert).count(), 0)
                self.assertEqual(db.query(Esp32EventReceipt).count(), 0)
        finally:
            self.client.application.config['TESTING'] = False


if __name__ == '__main__':
    unittest.main()
