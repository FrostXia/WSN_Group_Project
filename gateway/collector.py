"""Root serial -> durable SQLite spool -> HTTP. No fabricated sensor values."""
import argparse
import json
import math
import sqlite3
import sys
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def iso(value):
    return value.astimezone(timezone.utc).isoformat(timespec="milliseconds")


def build_upload(raw, observed=None):
    if not isinstance(raw, dict) or raw.get("type") != "sample":
        return None
    for key in ("node", "boot", "seq"):
        if type(raw.get(key)) is not int or not 0 <= raw[key] <= 4294967295:
            raise ValueError(f"invalid {key}")
    if not 1 <= raw["node"] <= 65535:
        raise ValueError("invalid radio ID")
    now = observed or datetime.now(timezone.utc)
    valid = raw.get("valid", 0)
    mask = raw.get("sensor_mask", 7 if valid else 0)
    if type(valid) is not int or type(mask) is not int:
        raise ValueError("invalid sensor flags")
    age = raw.get("sample_age_ms")
    if age is not None and (type(age) is not int or not 0 <= age <= 4294967295):
        raise ValueError("invalid sample age")
    recorded = now - timedelta(milliseconds=age or 0)
    samples = []
    for field, kind, unit, bit in (("temperature", "temperature", "°C", 1),
                                  ("humidity", "humidity", "%", 1),
                                  ("lux", "light", "lux", 2), ("motion", "pir", "", 4)):
        if not mask & bit:
            continue
        value = raw.get(field)
        good = bool(valid & bit) and type(value) in (int, float) and math.isfinite(value)
        samples.append(dict(sensor_type=kind, value=value if good else None, unit=unit,
                            quality="valid" if good else "fault", recorded_at=iso(recorded)))
    def finite(name):
        v = raw.get(name)
        return v if type(v) in (int, float) and math.isfinite(v) else None
    room = raw.get("room", 0)
    if type(room) is not int or not 0 <= room <= 65535:
        raise ValueError("invalid room")
    return dict(node_id=f"esp32_{raw['node']}", room_id=room, boot_id=raw["boot"],
                sequence=raw["seq"], observed_at=iso(now),
                time_basis="gateway_estimated" if age is not None else "gateway_receive",
                samples=samples, network=dict(path=raw.get("path", []),
                path_cost=finite("path_cost"), link_q=finite("link_q"), link_rssi=finite("link_rssi"),
                metric="energy_estimated_mj" if raw.get("metric") == 2 else "etx_app"))


def database(path):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    db = sqlite3.connect(path, timeout=10)
    db.execute("PRAGMA journal_mode=WAL")
    db.execute("""CREATE TABLE IF NOT EXISTS spool (
        packet_key TEXT PRIMARY KEY, payload TEXT NOT NULL, state INTEGER NOT NULL DEFAULT 0,
        error TEXT, created_at TEXT NOT NULL)""")
    db.commit()
    return db


def store(db, raw, observed=None):
    packet = build_upload(raw, observed)
    if packet is None:
        return False
    key = f"{packet['node_id']}:{packet['boot_id']}:{packet['sequence']}"
    cur = db.execute("INSERT OR IGNORE INTO spool(packet_key,payload,created_at) VALUES (?,?,?)",
                     (key, json.dumps(packet, ensure_ascii=False, allow_nan=False), packet["observed_at"]))
    db.commit()
    return bool(cur.rowcount)


def upload_one(db, url):
    row = db.execute("SELECT packet_key,payload FROM spool WHERE state=0 ORDER BY created_at LIMIT 1").fetchone()
    if not row:
        return "empty"
    key, payload = row
    request = urllib.request.Request(url, data=payload.encode("utf-8"), method="POST",
        headers={"Content-Type": "application/json", "Idempotency-Key": key})
    try:
        with urllib.request.urlopen(request, timeout=3) as response:
            answer = json.loads(response.read().decode("utf-8"))
            if answer.get("status") not in ("success", "duplicate"):
                raise ValueError("unexpected response; check backend URL")
    except urllib.error.HTTPError as exc:
        if 400 <= exc.code < 500 and exc.code != 429:
            db.execute("UPDATE spool SET state=2,error=? WHERE packet_key=?", (str(exc), key))
            db.commit()
            print(f"Rejected {key}: HTTP {exc.code}; saved in spool for inspection", flush=True)
            return "rejected"
        return "retry"
    except (urllib.error.URLError, OSError, ValueError, AttributeError):
        return "retry"
    db.execute("UPDATE spool SET state=1,error=NULL WHERE packet_key=?", (key,))
    db.commit()
    print(f"Uploaded {key}", flush=True)
    return "sent"


def uploader(path, url, stop):
    db = database(path)
    wait = 1
    try:
        while not stop.is_set():
            result = upload_one(db, url)
            if result == "retry":
                print(f"Backend unavailable; data retained. Retry in {wait}s", flush=True)
                stop.wait(wait); wait = min(30, wait*2)
            else:
                wait = 1
                if result == "empty":
                    stop.wait(.5)
    finally:
        db.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="Root port, e.g. COM7")
    parser.add_argument("--list", action="store_true", help="List serial ports without opening them")
    parser.add_argument("--url", default="http://127.0.0.1:5050/api/upload")
    parser.add_argument("--spool", default=str(ROOT / "data" / "gateway.sqlite3"))
    args = parser.parse_args()
    import serial
    if args.list:
        from serial.tools import list_ports
        for port in list_ports.comports():
            print(port.device, port.description)
        return
    if not args.port:
        parser.error("--port is required; use --list to inspect ports")
    db = database(args.spool)
    stop = threading.Event()
    worker = threading.Thread(target=uploader, args=(args.spool, args.url, stop), daemon=True)
    worker.start()
    print(f"Root {args.port} -> {args.url}; close Arduino Serial Monitor first. Ctrl+C stops.", flush=True)
    try:
        while not stop.is_set():
            try:
                with serial.Serial(args.port, 115200, timeout=1) as stream:
                    print("Serial connected; opening the port may reset the board.", flush=True)
                    while not stop.is_set():
                        line = stream.readline(8192)
                        if not line:
                            continue
                        try:
                            raw = json.loads(line.decode("utf-8"))
                            if store(db, raw):
                                print(f"Stored node={raw['node']} seq={raw['seq']} valid={raw.get('valid')} path={raw.get('path')}", flush=True)
                        except (ValueError, UnicodeError, TypeError) as exc:
                            if line.lstrip().startswith(b'{'):
                                print(f"Ignored invalid packet: {exc}", flush=True)
            except (serial.SerialException, OSError) as exc:
                print(f"Serial unavailable: {exc}. Retrying in 3s.", flush=True)
                stop.wait(3)
    except KeyboardInterrupt:
        print("Stopping; queued data is retained.")
    finally:
        stop.set(); worker.join(timeout=5); db.close()


if __name__ == "__main__":
    main()
