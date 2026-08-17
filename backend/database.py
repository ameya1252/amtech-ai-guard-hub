import os
from datetime import datetime, timezone

from sqlalchemy import Boolean, Column, DateTime, ForeignKey, Integer, String, UniqueConstraint, create_engine, inspect, text
from sqlalchemy.orm import declarative_base, relationship, sessionmaker


Base = declarative_base()


class User(Base):
    __tablename__ = "users"

    id = Column(String(128), primary_key=True)
    email = Column(String(255), nullable=False, unique=True, index=True)
    password_hash = Column(String(255), nullable=False)
    phone_number = Column(String(64), nullable=True)
    created_at = Column(DateTime(timezone=True), nullable=False, default=lambda: datetime.now(timezone.utc))

    shops = relationship("Shop", back_populates="user")
    push_tokens = relationship("PushToken", back_populates="user")


class Shop(Base):
    __tablename__ = "shops"

    id = Column(String(128), primary_key=True)
    user_id = Column(String(128), ForeignKey("users.id"), nullable=True, index=True)
    shop_name = Column(String(255), nullable=False)
    owner_name = Column(String(255), nullable=True)
    address = Column(String(1024), nullable=True)
    owner_phone = Column(String(64), nullable=True)
    owner_email = Column(String(255), nullable=True)
    auth_id = Column(String(255), nullable=True, index=True)
    armed = Column(Boolean, nullable=False, default=False, server_default="false")
    created_at = Column(DateTime(timezone=True), nullable=False, default=lambda: datetime.now(timezone.utc))

    user = relationship("User", back_populates="shops")
    devices = relationship("Device", back_populates="shop")
    cameras = relationship("Camera", back_populates="shop")
    alerts = relationship("Alert", back_populates="shop")
    device_schedule = relationship("ShopDeviceSchedule", back_populates="shop", uselist=False)
    emergency_contacts = relationship("ShopEmergencyContact", back_populates="shop")


class Device(Base):
    __tablename__ = "devices"

    id = Column(String(128), primary_key=True)
    shop_id = Column(String(128), ForeignKey("shops.id"), nullable=False, index=True)
    device_serial = Column(String(255), nullable=False, unique=True)
    last_seen_at = Column(DateTime(timezone=True), nullable=True)
    status = Column(String(32), nullable=False, default="offline")

    shop = relationship("Shop", back_populates="devices")


class CameraInventory(Base):
    __tablename__ = "camera_inventory"

    camera_serial = Column(String(255), primary_key=True)
    camera_ip = Column(String(255), nullable=False)
    camera_username = Column(String(255), nullable=False)
    camera_password = Column(String(255), nullable=False)
    created_at = Column(DateTime(timezone=True), nullable=False, default=lambda: datetime.now(timezone.utc))


class Camera(Base):
    __tablename__ = "cameras"
    __table_args__ = (
        UniqueConstraint("shop_id", "slot_number", name="uq_cameras_shop_slot"),
    )

    id = Column(String(128), primary_key=True)
    shop_id = Column(String(128), ForeignKey("shops.id"), nullable=False, index=True)
    camera_serial = Column(String(255), ForeignKey("camera_inventory.camera_serial"), nullable=False)
    slot_number = Column(Integer, nullable=False)
    enabled = Column(Boolean, nullable=False, default=True, server_default="true")
    created_at = Column(DateTime(timezone=True), nullable=False, default=lambda: datetime.now(timezone.utc))

    shop = relationship("Shop", back_populates="cameras")
    inventory = relationship("CameraInventory")


class Alert(Base):
    __tablename__ = "alerts"

    id = Column(String(128), primary_key=True)
    shop_id = Column(String(128), ForeignKey("shops.id"), nullable=False, index=True)
    event_type = Column(String(32), nullable=False)
    timestamp = Column(DateTime(timezone=True), nullable=False)
    media_url = Column(String(2048), nullable=True)
    # Legacy compatibility column for existing Neon tables. WhatsApp sending has
    # been removed; new API responses do not expose this field.
    whatsapp_sent = Column(Boolean, nullable=False, default=False)

    shop = relationship("Shop", back_populates="alerts")


class PushToken(Base):
    __tablename__ = "push_tokens"
    __table_args__ = (
        UniqueConstraint("expo_push_token", name="uq_push_tokens_expo_push_token"),
    )

    id = Column(String(128), primary_key=True)
    user_id = Column(String(128), ForeignKey("users.id"), nullable=False, index=True)
    expo_push_token = Column(String(255), nullable=False)
    platform = Column(String(32), nullable=True)
    created_at = Column(DateTime(timezone=True), nullable=False, default=lambda: datetime.now(timezone.utc))
    updated_at = Column(DateTime(timezone=True), nullable=False, default=lambda: datetime.now(timezone.utc))

    user = relationship("User", back_populates="push_tokens")


class ShopDeviceSchedule(Base):
    __tablename__ = "shop_device_schedules"

    shop_id = Column(String(128), ForeignKey("shops.id"), primary_key=True)
    arm_hour = Column(Integer, nullable=False, default=23, server_default="23")
    arm_minute = Column(Integer, nullable=False, default=0, server_default="0")
    disarm_hour = Column(Integer, nullable=False, default=6, server_default="6")
    disarm_minute = Column(Integer, nullable=False, default=0, server_default="0")
    updated_at = Column(DateTime(timezone=True), nullable=False, default=lambda: datetime.now(timezone.utc))

    shop = relationship("Shop", back_populates="device_schedule")


class ShopEmergencyContact(Base):
    __tablename__ = "shop_emergency_contacts"
    __table_args__ = (
        UniqueConstraint("shop_id", "slot_number", name="uq_shop_emergency_contacts_shop_slot"),
    )

    id = Column(String(128), primary_key=True)
    shop_id = Column(String(128), ForeignKey("shops.id"), nullable=False, index=True)
    slot_number = Column(Integer, nullable=False)
    name = Column(String(255), nullable=True)
    phone = Column(String(64), nullable=False)
    updated_at = Column(DateTime(timezone=True), nullable=False, default=lambda: datetime.now(timezone.utc))

    shop = relationship("Shop", back_populates="emergency_contacts")


def database_url():
    url = os.getenv("DATABASE_URL")
    if not url:
        raise RuntimeError("DATABASE_URL environment variable is required")
    if url.startswith("postgres://"):
        return url.replace("postgres://", "postgresql://", 1)
    return url


def engine_kwargs(url):
    kwargs = {"pool_pre_ping": True}
    if url.startswith("sqlite:"):
        kwargs["connect_args"] = {"check_same_thread": False}
    return kwargs


_database_url = database_url()
engine = create_engine(_database_url, **engine_kwargs(_database_url))
SessionLocal = sessionmaker(bind=engine, autoflush=False, autocommit=False, expire_on_commit=False)


def init_db():
    Base.metadata.create_all(bind=engine)
    run_migrations()
    seed_camera_inventory()


def camera_inventory_seed_rows():
    raw_seed = os.getenv("AMTECH_CAMERA_INVENTORY", "").strip()
    if raw_seed:
        rows = []
        for entry in raw_seed.split(";"):
            parts = [part.strip() for part in entry.split(",")]
            if len(parts) != 4 or not all(parts):
                print(f"Skipping invalid AMTECH_CAMERA_INVENTORY entry: {entry}", flush=True)
                continue
            rows.append({
                "camera_serial": parts[0].upper(),
                "camera_ip": parts[1],
                "camera_username": parts[2],
                "camera_password": parts[3],
            })
        return rows

    return [
        {
            "camera_serial": os.getenv("AMTECH_CAMERA_1_SERIAL", "CAM-0001").upper(),
            "camera_ip": os.getenv("AMTECH_CAMERA_1_IP", "192.168.0.4"),
            "camera_username": os.getenv("AMTECH_CAMERA_1_USERNAME", "Amtech"),
            "camera_password": os.getenv("AMTECH_CAMERA_1_PASSWORD", "Amtech123"),
        },
        {
            "camera_serial": os.getenv("AMTECH_CAMERA_2_SERIAL", "CAM-0002").upper(),
            "camera_ip": os.getenv("AMTECH_CAMERA_2_IP", "192.168.0.7"),
            "camera_username": os.getenv("AMTECH_CAMERA_2_USERNAME", "Amtech1"),
            "camera_password": os.getenv("AMTECH_CAMERA_2_PASSWORD", "Amtech1234"),
        },
    ]


def seed_camera_inventory():
    rows = camera_inventory_seed_rows()
    if not rows:
        return

    with engine.begin() as connection:
        for row in rows:
            existing = connection.execute(
                text("SELECT camera_serial FROM camera_inventory WHERE camera_serial = :camera_serial"),
                {"camera_serial": row["camera_serial"]},
            ).first()
            if existing is not None:
                continue

            connection.execute(
                text(
                    "INSERT INTO camera_inventory "
                    "(camera_serial, camera_ip, camera_username, camera_password, created_at) "
                    "VALUES (:camera_serial, :camera_ip, :camera_username, :camera_password, :created_at)"
                ),
                {
                    **row,
                    "created_at": datetime.now(timezone.utc),
                },
            )


def run_migrations():
    inspector = inspect(engine)
    shop_columns = {column["name"] for column in inspector.get_columns("shops")}
    alert_columns = {column["name"] for column in inspector.get_columns("alerts")}
    with engine.begin() as connection:
        if not _database_url.startswith("sqlite:"):
            connection.execute(text("ALTER TABLE cameras DROP CONSTRAINT IF EXISTS uq_cameras_camera_serial"))

        if "armed" not in shop_columns:
            default_value = "0" if _database_url.startswith("sqlite:") else "false"
            connection.execute(text(f"ALTER TABLE shops ADD COLUMN armed BOOLEAN NOT NULL DEFAULT {default_value}"))

        if "user_id" not in shop_columns:
            connection.execute(text("ALTER TABLE shops ADD COLUMN user_id VARCHAR(128)"))

        if "owner_name" not in shop_columns:
            connection.execute(text("ALTER TABLE shops ADD COLUMN owner_name VARCHAR(255)"))

        if "address" not in shop_columns:
            connection.execute(text("ALTER TABLE shops ADD COLUMN address VARCHAR(1024)"))

        if "media_url" not in alert_columns:
            connection.execute(text("ALTER TABLE alerts ADD COLUMN media_url VARCHAR(2048)"))


def db_session():
    db = SessionLocal()
    try:
        yield db
    finally:
        db.close()
