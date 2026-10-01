"""Local lab monitor. SQLite by default; DATABASE_URL enables PostgreSQL."""
import hashlib
import json
import math
import os
import re
import sqlite3
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
from pathlib import Path

from flask import Flask, jsonify, request, send_from_directory

ROOT = Path(__file__).resolve().parents[1]
SENSOR_UNITS = {"temperature": "°C", "humidity": "%", "light": "lux",
                "pir": "", "smoke": "ppm", "gas": "ppm", "mmwave": ""}


def utcnow():
    return datetime.now(timezone.utc)


def iso(value):
    return value.astimezone(timezone.utc).isoformat(timespec="milliseconds")


def timestamp(value):
    if not isinstance(value, str):
        raise ValueError("timestamp must be a timezone-aware ISO8601 string")
    parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        raise ValueError("timestamp requires timezone")
    if parsed > utcnow() + timedelta(minutes=5):
        raise ValueError("timestamp is too far in the future")
    return iso(parsed)


def integer(value, name, minimum=0, maximum=4294967295):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(f"invalid {name}")
    return value


class Database:
    def __init__(self, path, url=None):
        self.path, self.url = str(path), url
        if not url:
            Path(path).parent.mkdir(parents=True, exist_ok=True)

    @contextmanager
    def session(self):
        if self.url:
            import psycopg2
            conn = psycopg2.connect(self.url, connect_timeout=5)
        else:
            conn = sqlite3.connect(self.path, timeout=10)
            conn.execute("PRAGMA foreign_keys=ON")
            conn.execute("PRAGMA journal_mode=WAL")
        try:
            yield conn
            conn.commit()
        except Exception:
            conn.rollback()
            raise
        finally:
            conn.close()

    def execute(self, conn, statement, args=()):
        cur = conn.cursor()
        cur.execute(statement.replace("?", "%s") if self.url else statement, args)
        return cur

    def rows(self, conn, statement, args=()):
        cur = self.execute(conn, statement, args)
        names = [x[0] for x in cur.description]
        result = [dict(zip(names, row)) for row in cur.fetchall()]
        cur.close()
        return result

    def initialize(self):
        statements = (ROOT / "database" / "schema.sql").read_text(encoding="utf-8")
        with self.session() as conn:
            for statement in statements.split(";"):
                if statement.strip():
                    self.execute(conn, statement)


def normalize(data, idem=None):
    if not isinstance(data, dict):
        raise ValueError("JSON object required")
    supplied_id = data.get("node_id", "")
    match = re.fullmatch(r"esp32_([1-9][0-9]{0,4})(?:_room([0-9]{1,5}))?", str(supplied_id))
    if not match:
        raise ValueError("node_id must be esp32_<number> or legacy esp32_<number>_room<number>")
    radio = integer(int(match[1]), "radio_id", 1, 65535)
    node = f"esp32_{radio}"
    room = data.get("room_id", int(match[2]) if match[2] else 0)
    integer(room, "room_id", 0, 65535)
    observed = timestamp(data.get("observed_at", iso(utcnow())))
    basis = data.get("time_basis", "sender_supplied")
    if basis not in ("gateway_receive", "gateway_estimated", "sensor_synced", "sender_supplied"):
        raise ValueError("invalid time_basis")
    seq, boot = data.get("sequence"), data.get("boot_id")
    if seq is not None or boot is not None:
        integer(seq, "sequence"); integer(boot, "boot_id")
        key = f"{node}:{boot}:{seq}"
    else:
        digest = hashlib.sha256(json.dumps(data, sort_keys=True, allow_nan=False).encode()).hexdigest()
        key = f"{node}:legacy:{idem or digest}"
    if len(key) > 250:
        raise ValueError("packet key too long")
    samples = data.get("samples")
    if not isinstance(samples, list) or len(samples) > 16:
        raise ValueError("samples must be an array of at most 16 entries")
    parsed, types = [], set()
    for sample in samples:
        if not isinstance(sample, dict):
            raise ValueError("sample must be an object")
        kind = sample.get("sensor_type")
        if kind not in SENSOR_UNITS or kind in types:
            raise ValueError("unsupported or duplicate sensor_type")
        types.add(kind)
        quality = sample.get("quality", "valid")
        if quality not in ("valid", "fault", "unknown"):
            raise ValueError("invalid quality")
        value = sample.get("value")
        if quality == "valid":
            if type(value) not in (int, float) or not math.isfinite(value):
                raise ValueError("valid sample requires a finite number")
            if kind in ("pir", "mmwave") and value not in (0, 1):
                raise ValueError("motion value must be 0 or 1")
        else:
            value = None
        unit = sample.get("unit", SENSOR_UNITS[kind])
        if not isinstance(unit, str) or len(unit) > 16:
            raise ValueError("invalid unit")
        recorded = timestamp(sample.get("recorded_at") or observed)
        if recorded > observed and basis.startswith("gateway"):
            raise ValueError("estimated sample time cannot follow gateway receipt")
        parsed.append(dict(sensor_type=kind, value=value, unit=unit,
                           recorded_at=recorded, quality=quality))
    network = data.get("network", {})
    if not isinstance(network, dict):
        raise ValueError("network must be an object")
    path = network.get("path", [])
    if not isinstance(path, list) or len(path) > 8:
        raise ValueError("path must contain at most 8 nodes")
    for entry in path:
        integer(entry, "path entry", 1, 65535)
    if len(set(path)) != len(path) or (path and path[0] != radio):
        raise ValueError("invalid path")
    clean_net = {"path": path}
    for name in ("path_cost", "link_q", "link_rssi"):
        v = network.get(name)
        if v is not None and (type(v) not in (int, float) or not math.isfinite(v)):
            raise ValueError(f"invalid {name}")
        clean_net[name] = v
    if clean_net["path_cost"] is not None and clean_net["path_cost"] < 0:
        raise ValueError("negative path cost")
    if clean_net["link_q"] is not None and not 0 <= clean_net["link_q"] <= 1:
        raise ValueError("invalid probability")
    clean_net["metric"] = network.get("metric", "etx_app")
    if clean_net["metric"] not in ("etx_app", "energy_estimated_mj"):
        raise ValueError("invalid route metric")
    return dict(key=key, node=node, radio=radio, room=room, observed=observed,
                basis=basis, boot=boot, seq=seq, samples=parsed, network=clean_net)


def create_app(db_path=None, database_url=None):
    app = Flask(__name__, static_folder=None)
    app.config["MAX_CONTENT_LENGTH"] = 65536
    db = Database(db_path or ROOT / "data" / "lab.sqlite3", database_url)
    db.initialize()
    app.extensions["lab_db"] = db
    settings = json.loads((ROOT / "settings.json").read_text(encoding="utf-8"))
    offline_seconds = settings["offline_seconds"]

    @app.errorhandler(ValueError)
    def invalid(exc):
        return jsonify(error=str(exc)), 400

    @app.errorhandler(500)
    def failure(exc):
        return jsonify(error="Backend error; inspect the server console"), 500

    @app.get("/")
    def index():
        return send_from_directory(ROOT / "ui", "index.html")

    @app.get("/<name>")
    def asset(name):
        if name not in ("app.js", "styles.css"):
            return jsonify(error="not found"), 404
        return send_from_directory(ROOT / "ui", name)

    @app.get("/api/health")
    def health():
        with db.session() as conn:
            db.execute(conn, "SELECT 1")
        return jsonify(status="ok", database="postgresql" if db.url else "sqlite")

    @app.get("/api/settings")
    def get_settings():
        return jsonify(settings)

    @app.post("/api/upload")
    def upload():
        packet = normalize(request.get_json(silent=True), request.headers.get("Idempotency-Key"))
        now = iso(utcnow())
        with db.session() as conn:
            db.execute(conn, """INSERT INTO ls_nodes(node_id,radio_id,room_id,last_seen)
                VALUES (?,?,?,?) ON CONFLICT(node_id) DO NOTHING""",
                (packet["node"], packet["radio"], packet["room"], packet["observed"]))
            cur = db.execute(conn, """INSERT INTO ls_packets
                (packet_key,node_id,boot_id,sequence,observed_at,received_at,time_basis,network_json)
                VALUES (?,?,?,?,?,?,?,?) ON CONFLICT(packet_key) DO NOTHING""",
                (packet["key"], packet["node"], packet["boot"], packet["seq"], packet["observed"], now,
                 packet["basis"], json.dumps(packet["network"], allow_nan=False)))
            if cur.rowcount == 0:
                return jsonify(status="duplicate", packet_key=packet["key"])
            # Replay must not advance node freshness to the upload/retry time.
            db.execute(conn, """UPDATE ls_nodes SET room_id=?,last_seen=?
                WHERE node_id=? AND last_seen<=?""",
                (packet["room"], packet["observed"], packet["node"], packet["observed"]))
            for s in packet["samples"]:
                db.execute(conn, """INSERT INTO ls_samples
                    (packet_key,sensor_type,value,unit,recorded_at,quality) VALUES (?,?,?,?,?,?)""",
                    (packet["key"], s["sensor_type"], s["value"], s["unit"], s["recorded_at"], s["quality"]))
        return jsonify(status="success", packet_key=packet["key"], received_at=now)

    def nodes():
        with db.session() as conn:
            result = db.rows(conn, "SELECT * FROM ls_nodes ORDER BY radio_id")
        for n in result:
            override = settings.get("node_rooms", {}).get(n["node_id"])
            n.update(id=n["node_id"], label=f"ESP32 · {n['radio_id']:02d}",
                     room=override or (f"Room {n['room_id']}" if n["room_id"] else "Room unassigned"),
                     online=(utcnow()-datetime.fromisoformat(n["last_seen"])).total_seconds() <= offline_seconds)
        return result

    @app.get("/api/nodes")
    def get_nodes():
        return jsonify(data=nodes())

    @app.get("/api/latest/<node_id>")
    def latest(node_id):
        node_id = re.sub(r"_room[0-9]+$", "", node_id)
        with db.session() as conn:
            rows = db.rows(conn, """SELECT * FROM (
                SELECT s.*,p.node_id,p.observed_at,p.time_basis,
                    ROW_NUMBER() OVER(PARTITION BY s.sensor_type ORDER BY s.recorded_at DESC,p.observed_at DESC) AS rn
                FROM ls_samples s JOIN ls_packets p ON p.packet_key=s.packet_key WHERE p.node_id=?
                ) ranked WHERE rn=1 ORDER BY sensor_type""", (node_id,))
        for row in rows:
            row.pop("rn", None)
        return jsonify(node_id=node_id, data=rows, count=len(rows))

    @app.get("/api/history")
    def history():
        try:
            hours = float(request.args.get("hours", 1))
            limit = int(request.args.get("limit", 2000))
            before = timestamp(request.args["before"]) if "before" in request.args else None
        except (ValueError, OverflowError):
            raise ValueError("invalid history parameters")
        if not math.isfinite(hours) or not 0 < hours <= 168 or not 1 <= limit <= 5000:
            raise ValueError("hours must be <=168 and limit <=5000")
        query = """SELECT s.*,p.node_id,p.observed_at,p.time_basis FROM ls_samples s
            JOIN ls_packets p ON p.packet_key=s.packet_key WHERE s.recorded_at>=?"""
        args = [iso(utcnow()-timedelta(hours=hours))]
        for name, column in (("node_id", "p.node_id"), ("sensor_type", "s.sensor_type")):
            if request.args.get(name):
                query += f" AND {column}=?"; args.append(request.args[name])
        if before:
            query += " AND s.recorded_at<?"; args.append(before)
        query += " ORDER BY s.recorded_at DESC,p.packet_key DESC LIMIT ?"; args.append(limit+1)
        with db.session() as conn:
            rows = db.rows(conn, query, args)
        return jsonify(data=rows[:limit], truncated=len(rows)>limit)

    @app.get("/api/network")
    def network():
        with db.session() as conn:
            rows = db.rows(conn, """SELECT * FROM (
                SELECT node_id,observed_at,network_json,
                  ROW_NUMBER() OVER(PARTITION BY node_id ORDER BY observed_at DESC,received_at DESC) AS rn
                FROM ls_packets) ranked WHERE rn=1 ORDER BY node_id""")
        return jsonify(data=[dict(node_id=r["node_id"], observed_at=r["observed_at"],
                                  **json.loads(r["network_json"])) for r in rows])

    @app.get("/api/snapshot")
    def snapshot():
        with db.session() as conn:
            rows = db.rows(conn, """SELECT * FROM (
                SELECT s.*,p.node_id,p.observed_at,p.time_basis,
                  ROW_NUMBER() OVER(PARTITION BY p.node_id,s.sensor_type ORDER BY s.recorded_at DESC,p.observed_at DESC) AS rn
                FROM ls_samples s JOIN ls_packets p ON p.packet_key=s.packet_key
                ) ranked WHERE rn=1""")
        grouped = {}
        for row in rows:
            row.pop("rn", None)
            grouped.setdefault(row["node_id"], []).append(row)
        return jsonify(nodes=nodes(), latest=grouped, network=network().get_json()["data"], settings=settings)

    return app


if __name__ == "__main__":
    app = create_app(os.environ.get("LAB_DB_PATH"), os.environ.get("DATABASE_URL"))
    port = int(os.environ.get("LAB_PORT", "5050"))
    print(f"Open http://127.0.0.1:{port} — local lab server; Ctrl+C to stop")
    app.run(host="127.0.0.1", port=port, debug=False, threaded=True)
