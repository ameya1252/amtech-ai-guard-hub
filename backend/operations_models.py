"""Additive manufacturing records. No changes to alarm tables or policy."""
from datetime import datetime, timezone
from sqlalchemy import Column, DateTime, ForeignKey, Integer, JSON, String
from database import Base

def now():
    return datetime.now(timezone.utc)

class SerialCounter(Base):
    __tablename__ = 'ops_serial_counters'
    prefix = Column(String(32), primary_key=True)
    value = Column(Integer, nullable=False, default=0)

class LabelBatch(Base):
    __tablename__ = 'ops_label_batches'
    id = Column(String(64), primary_key=True)
    kind = Column(String(32), nullable=False)
    period = Column(String(4), nullable=False)
    count = Column(Integer, nullable=False)
    production = Column(JSON, nullable=True)
    created_at = Column(DateTime(timezone=True), nullable=False, default=now)

class Asset(Base):
    __tablename__ = 'ops_assets'
    serial = Column(String(128), primary_key=True)
    kind = Column(String(32), nullable=False, index=True)
    batch_id = Column(String(64), ForeignKey('ops_label_batches.id'), nullable=False, index=True)
    order_id = Column(String(64), nullable=True, index=True)
    hardware_id = Column(String(32), nullable=True, unique=True)
    pcb_revision = Column(String(64), nullable=True)
    firmware_version = Column(String(128), nullable=True)
    manufacturing_batch = Column(String(64), nullable=True)
    manufactured_on = Column(String(10), nullable=True)
    claim_cipher = Column(String(1024), nullable=True)
    token_cipher = Column(String(1024), nullable=True)
    test_status = Column(String(16), nullable=False, default='pending')
    test_results = Column(JSON, nullable=True)
    created_at = Column(DateTime(timezone=True), nullable=False, default=now)

class PackingOrder(Base):
    __tablename__ = 'ops_packing_orders'
    id = Column(String(64), primary_key=True)
    reference = Column(String(128), nullable=False, unique=True)
    expected = Column(JSON, nullable=False)
    hub_serial = Column(String(128), ForeignKey('ops_assets.serial'), nullable=True, unique=True)
    status = Column(String(16), nullable=False, default='draft')
    created_at = Column(DateTime(timezone=True), nullable=False, default=now)
    packed_at = Column(DateTime(timezone=True), nullable=True)
    dispatched_at = Column(DateTime(timezone=True), nullable=True)

class InstallationLocation(Base):
    __tablename__ = 'ops_installation_locations'
    shop_id = Column(String(128), ForeignKey('shops.id'), primary_key=True)
    latitude = Column(String(32), nullable=True)
    longitude = Column(String(32), nullable=True)
    source = Column(String(24), nullable=False, default='address')
    created_at = Column(DateTime(timezone=True), nullable=False, default=now)

class OperationsEvent(Base):
    __tablename__ = 'ops_events'
    id = Column(String(64), primary_key=True)
    serial = Column(String(128), nullable=True, index=True)
    kind = Column(String(64), nullable=False, index=True)
    message = Column(String(1024), nullable=False)
    details = Column(JSON, nullable=True)
    created_at = Column(DateTime(timezone=True), nullable=False, default=now, index=True)

class FactoryRun(Base):
    __tablename__ = 'ops_factory_runs'
    id = Column(String(64), primary_key=True)
    serial = Column(String(128), ForeignKey('ops_assets.serial'), nullable=False, index=True)
    hardware_id = Column(String(32), nullable=False)
    release_id = Column(String(64), nullable=False)
    operation = Column(String(16), nullable=False)
    image_sha256 = Column(String(64), nullable=False)
    status = Column(String(16), nullable=False, default='issued')
    created_at = Column(DateTime(timezone=True), nullable=False, default=now)
    verified_at = Column(DateTime(timezone=True), nullable=True)
