#!/usr/bin/env python3
"""Provision/revoke a named installer's time-limited site access using operator DB access.

No public grant endpoint and no automatic owner-to-technician privilege upgrade.
DATABASE_URL must point to the intended backend database. Never put it in arguments.
"""
import argparse
from pathlib import Path
import sys
from datetime import datetime,timedelta,timezone
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'backend'))
from database import SessionLocal, User, Shop, TechnicianSiteAccess
from operations_api import audit
p=argparse.ArgumentParser();p.add_argument('--email',required=True);p.add_argument('--shop',required=True);p.add_argument('--hours',type=int,default=24);p.add_argument('--operator',required=True);p.add_argument('--revoke',action='store_true');a=p.parse_args()
if not 1<=a.hours<=168:p.error('Use 1–168 hours')
with SessionLocal() as db:
 user=db.query(User).filter(User.email==a.email.strip().lower()).first()
 if not user or not db.get(Shop,a.shop):raise SystemExit('Existing user and site required')
 row=db.get(TechnicianSiteAccess,(a.shop,user.id))
 if a.revoke:
  if row:db.delete(row)
 else:
  if row is None:row=TechnicianSiteAccess(shop_id=a.shop,user_id=user.id);db.add(row)
  row.expires_at=datetime.now(timezone.utc)+timedelta(hours=a.hours);row.granted_by=a.operator
 audit(db,'technician.access.revoked' if a.revoke else 'technician.access.granted','Installer camera access changed',details=dict(shop_id=a.shop,user_id=user.id,operator=a.operator,hours=None if a.revoke else a.hours))
 db.commit()
 print('Technician site access revoked.' if a.revoke else 'Time-limited technician site access granted.')
