"""Provision a hub using DATABASE_URL; credentials go only to a private output file."""
import argparse
import json
import os
import re
import secrets
from pathlib import Path

from database import Device, Esp32Hub, SessionLocal, init_db
from esp32_api import secret_hash


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("serial")
    parser.add_argument("--output", required=True)
    parser.add_argument("--backend-url", default="https://amtech-ai-guard-hub-production.up.railway.app")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", args.serial):
        parser.error("Invalid serial")
    if not args.backend_url.startswith("https://"):
        parser.error("A HTTPS backend is required")
    target = Path(args.output)
    token, pairing = secrets.token_urlsafe(32), secrets.token_urlsafe(20)
    init_db()
    with SessionLocal() as db:
        if db.get(Esp32Hub, args.serial) or db.query(Device).filter(Device.device_serial == args.serial).first():
            parser.error("Serial already exists; existing credentials will not be overwritten")
        # Exclusive creation prevents losing existing credentials. No secret is printed.
        descriptor = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            with os.fdopen(descriptor, "w") as output:
                json.dump({"device_serial": args.serial, "device_token": token, "pairing_code": pairing,
                           "backend_url": args.backend_url}, output, indent=2)
            db.add(Esp32Hub(serial=args.serial, token_hash=secret_hash(token), pairing_code_hash=secret_hash(pairing)))
            db.commit()
        except Exception:
            target.unlink(missing_ok=True)
            raise
    print(f"Provisioned {args.serial}. Private credentials saved to {target}.")


if __name__ == "__main__":
    main()
