"""Installation access is provisioned by an operator, never self-assigned in the app."""
from datetime import datetime, timezone
from database import TechnicianSiteAccess


def technician_access(db, shop_id, user_id):
    row = db.get(TechnicianSiteAccess, (shop_id, user_id))
    if row is None:
        return False
    until = row.expires_at
    if until.tzinfo is None:
        until = until.replace(tzinfo=timezone.utc)
    return until > datetime.now(timezone.utc)


def readable_shop(db, shop_id, user_id):
    from database import Shop
    shop = db.get(Shop, shop_id)
    return shop if shop and (shop.user_id == user_id or technician_access(db, shop_id, user_id)) else None
