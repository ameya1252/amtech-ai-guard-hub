# Reliability and technician setup candidate

Adds `/shop/<id>/status-stream` using Flask-Sock on the existing single-worker
Railway service. Authenticate with the first JSON message `{ "token": "..." }`;
never put a bearer token in the URL. Only site owners receive full status frames.
Queues coalesce updates; every reconnect gets an authoritative snapshot. Use
`status_revision` to reject delayed frames and `last_seen_at`/negotiated heartbeat
timeout for freshness. Pilot capacity is 16 streams; HTTP remains the fallback.
Keep one worker and 32 threads. Multiple workers require shared fan-out first.

`/shop/<id>/esp32/contacts` GET/PUT is owner-only. PUT requires JSON with
`expected_revision` and one-to-three `contacts` (`slot`, `name`, E.164 `phone`).
A successful save requests configuration; it does not prove modem application.
The next authenticated hub report must confirm revision and number readback.
Only new contact-protocol firmware receives this configuration. Legacy endpoints
and SMS-control authorization are unchanged.

Camera routes accept `camera-1` and `camera-2`; writes require a nonexpired
`TechnicianSiteAccess` row. Primary setup requires configuration protocol 2.
Encrypted Camera 2 rows and old device protocol remain compatible. Technician
access does not grant alarm controls, contact edits, or evidence access on another
owner's site. Only basic site/status reads and camera setup are allowed.

An operator provisions/revokes a grant using `tools/technician_access.py`, with
DATABASE_URL supplied through the existing private environment:

    python tools/technician_access.py --email INSTALLER_EMAIL --shop SHOP_ID --hours 24 --operator OPERATOR_NAME
    python tools/technician_access.py --email INSTALLER_EMAIL --shop SHOP_ID --operator OPERATOR_NAME --revoke

The user must already exist. Grants are site-specific, audited, and bounded to
1–168 hours. No public role-assignment API is added. Do not expose operator DB
credentials or add these operations to an unauthenticated portal proxy.

New SQLAlchemy tables are additive and created by existing initialization. Deploy
backend before firmware/app. Older firmware keeps its 45-second online window;
status-protocol-2 hubs use 20 seconds. Update the external recorder before using
primary-camera reconfiguration. Recorder poll protocol 2 receives only applied
camera credentials; older recorders are excluded from a changed primary feed.

No tamper escalation, new ARM policy, manual siren, new cloud subscription, or
public app-store release is included. Physical fault injection and 72-hour
endurance remain gates before production qualification.
