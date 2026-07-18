import os
from datetime import datetime, timezone

from sqlalchemy import Boolean, Column, DateTime, ForeignKey, String, create_engine, inspect, text
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


class Shop(Base):
    __tablename__ = "shops"

    id = Column(String(128), primary_key=True)
    user_id = Column(String(128), ForeignKey("users.id"), nullable=True, index=True)
    shop_name = Column(String(255), nullable=False)
    owner_phone = Column(String(64), nullable=True)
    owner_email = Column(String(255), nullable=True)
    auth_id = Column(String(255), nullable=True, index=True)
    armed = Column(Boolean, nullable=False, default=False, server_default="false")
    created_at = Column(DateTime(timezone=True), nullable=False, default=lambda: datetime.now(timezone.utc))

    user = relationship("User", back_populates="shops")
    devices = relationship("Device", back_populates="shop")
    alerts = relationship("Alert", back_populates="shop")


class Device(Base):
    __tablename__ = "devices"

    id = Column(String(128), primary_key=True)
    shop_id = Column(String(128), ForeignKey("shops.id"), nullable=False, index=True)
    device_serial = Column(String(255), nullable=False, unique=True)
    last_seen_at = Column(DateTime(timezone=True), nullable=True)
    status = Column(String(32), nullable=False, default="offline")

    shop = relationship("Shop", back_populates="devices")


class Alert(Base):
    __tablename__ = "alerts"

    id = Column(String(128), primary_key=True)
    shop_id = Column(String(128), ForeignKey("shops.id"), nullable=False, index=True)
    event_type = Column(String(32), nullable=False)
    timestamp = Column(DateTime(timezone=True), nullable=False)
    whatsapp_sent = Column(Boolean, nullable=False, default=False)

    shop = relationship("Shop", back_populates="alerts")


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


def run_migrations():
    inspector = inspect(engine)
    shop_columns = {column["name"] for column in inspector.get_columns("shops")}
    with engine.begin() as connection:
        if "armed" not in shop_columns:
            default_value = "0" if _database_url.startswith("sqlite:") else "false"
            connection.execute(text(f"ALTER TABLE shops ADD COLUMN armed BOOLEAN NOT NULL DEFAULT {default_value}"))

        if "user_id" not in shop_columns:
            connection.execute(text("ALTER TABLE shops ADD COLUMN user_id VARCHAR(128)"))


def db_session():
    db = SessionLocal()
    try:
        yield db
    finally:
        db.close()
