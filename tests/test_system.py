"""Isolated tests: no radio port, real user DB, or production API is touched."""
import json
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest.mock import patch
import urllib.error

from backend.app import create_app
from gateway.collector import build_upload, database, store, upload_one


class SystemTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.app = create_app(Path(self.temp.name) / "server.sqlite3")
        self.app.testing = True
        self.client = self.app.test_client()
        self.spool = database(Path(self.temp.name) / "spool.sqlite3")

    def tearDown(self):
        self.spool.close()
        self.temp.cleanup()

    def raw(self, **changes):
        value = dict(type="sample", node=3, boot=45, seq=1, room=2, valid=7, sensor_mask=7,
                     temperature=25.5, humidity=60.0, lux=100.0, motion=1,
                     path=[3, 1, 2], path_cost=2.1, link_q=.95, link_rssi=-65,
                     metric=1, sample_age_ms=10)
        value.update(changes)
        return value

    def upload(self, raw=None, observed=None):
        return self.client.post("/api/upload", json=build_upload(raw or self.raw(), observed))

    def test_empty_ui_and_health(self):
        for url in ("/", "/app.js", "/styles.css", "/api/health", "/api/snapshot"):
            with self.client.get(url) as response:
                self.assertEqual(response.status_code, 200)
        self.assertEqual(self.client.get("/api/nodes").json["data"], [])
        self.assertEqual(self.client.get("/settings.json").status_code, 404)

    def test_raw_to_database_to_ui(self):
        self.assertEqual(self.upload().status_code, 200)
        snapshot = self.client.get("/api/snapshot").json
        self.assertEqual(snapshot["nodes"][0]["id"], "esp32_3")
        self.assertEqual(snapshot["nodes"][0]["room"], "Room 2")
        self.assertEqual(snapshot["network"][0]["path"], [3, 1, 2])
        self.assertEqual(len(snapshot["latest"]["esp32_3"]), 4)

    def test_duplicate_is_idempotent(self):
        self.upload()
        self.assertEqual(self.upload().json["status"], "duplicate")
        self.assertEqual(len(self.client.get("/api/history").json["data"]), 4)

    def test_reboot_sequence_is_distinct(self):
        self.upload()
        self.upload(self.raw(boot=46))
        self.assertEqual(len(self.client.get("/api/history").json["data"]), 8)

    def test_room_change_preserves_identity(self):
        self.upload()
        self.upload(self.raw(seq=2, room=5))
        nodes = self.client.get("/api/nodes").json["data"]
        self.assertEqual(len(nodes), 1)
        self.assertEqual(nodes[0]["room"], "Room 5")

    def test_disabled_sensors_still_register_online(self):
        self.upload(self.raw(valid=0, sensor_mask=0))
        snapshot = self.client.get("/api/snapshot").json
        self.assertTrue(snapshot["nodes"][0]["online"])
        self.assertEqual(snapshot["latest"], {})

    def test_fault_replaces_latest_valid_reading(self):
        first = datetime.now(timezone.utc) - timedelta(seconds=10)
        self.upload(observed=first)
        self.upload(self.raw(seq=2, valid=6, temperature=None, humidity=None))
        items = self.client.get("/api/latest/esp32_3").json["data"]
        t = next(x for x in items if x["sensor_type"] == "temperature")
        self.assertEqual(t["quality"], "fault")
        self.assertIsNone(t["value"])

    def test_replay_does_not_mark_old_node_online(self):
        old = datetime.now(timezone.utc) - timedelta(minutes=10)
        self.upload(observed=old)
        self.assertFalse(self.client.get("/api/nodes").json["data"][0]["online"])

    def test_replay_does_not_overwrite_newer_reading(self):
        self.upload(self.raw(seq=2, temperature=26))
        self.upload(self.raw(seq=1, temperature=20), datetime.now(timezone.utc)-timedelta(minutes=10))
        items = self.client.get("/api/latest/esp32_3").json["data"]
        self.assertEqual(next(x["value"] for x in items if x["sensor_type"]=="temperature"), 26)

    def test_estimated_time_excludes_wireless_queue_age(self):
        now = datetime.now(timezone.utc)
        packet = build_upload(self.raw(sample_age_ms=120000), now)
        delta = datetime.fromisoformat(packet["observed_at"])-datetime.fromisoformat(packet["samples"][0]["recorded_at"])
        self.assertEqual(delta.total_seconds(), 120)
        self.assertEqual(packet["time_basis"], "gateway_estimated")

    def test_legacy_upload_shape(self):
        payload = {"node_id":"esp32_1_room1", "samples":[{"sensor_type":"temperature",
            "value":24, "unit":"℃", "recorded_at":datetime.now(timezone.utc).isoformat()}]}
        self.assertEqual(self.client.post("/api/upload", json=payload).status_code, 200)
        self.assertEqual(self.client.get("/api/latest/esp32_1_room1").json["count"], 1)

    def test_invalid_packet_is_atomic(self):
        p = build_upload(self.raw())
        p["samples"][1]["value"] = None
        self.assertEqual(self.client.post("/api/upload", json=p).status_code, 400)
        self.assertEqual(self.client.get("/api/nodes").json["data"], [])

    def test_invalid_path(self):
        p = build_upload(self.raw(path=[3, 1, 3, 2]))
        self.assertEqual(self.client.post("/api/upload", json=p).status_code, 400)

    def test_invalid_history_parameters(self):
        for query in ("hours=nan", "hours=-1", "hours=10000", "limit=0", "hours=bad"):
            self.assertEqual(self.client.get("/api/history?"+query).status_code, 400)

    def test_history_is_per_node(self):
        self.upload()
        self.upload(self.raw(node=4, path=[4,2]))
        data = self.client.get("/api/history?node_id=esp32_3").json["data"]
        self.assertTrue(all(x["node_id"]=="esp32_3" for x in data))

    def test_non_sample_serial_line_is_ignored(self):
        self.assertFalse(store(self.spool, {"type":"link", "node":2}))

    def test_spool_preserves_time_and_deduplicates(self):
        self.assertTrue(store(self.spool, self.raw()))
        self.assertFalse(store(self.spool, self.raw()))
        self.assertEqual(self.spool.execute("SELECT COUNT(*) FROM spool").fetchone()[0], 1)

    def test_http_retry_retains_data(self):
        store(self.spool, self.raw())
        with patch("urllib.request.urlopen", side_effect=urllib.error.URLError("offline")):
            self.assertEqual(upload_one(self.spool, "http://localhost/api/upload"), "retry")
        self.assertEqual(self.spool.execute("SELECT state FROM spool").fetchone()[0], 0)

    def test_http_success_marks_delivered(self):
        store(self.spool, self.raw())
        with patch("urllib.request.urlopen") as http:
            http.return_value.__enter__.return_value.read.return_value=b'{"status":"success"}'
            self.assertEqual(upload_one(self.spool, "http://localhost/api/upload"), "sent")
        self.assertEqual(self.spool.execute("SELECT state FROM spool").fetchone()[0], 1)

    def test_permanent_rejection_does_not_block_following_packets(self):
        store(self.spool, self.raw())
        with patch("urllib.request.urlopen", side_effect=urllib.error.HTTPError("url",400,"bad",None,None)):
            self.assertEqual(upload_one(self.spool, "http://localhost/api/upload"), "rejected")
        self.assertEqual(self.spool.execute("SELECT state FROM spool").fetchone()[0], 2)


if __name__ == "__main__":
    unittest.main()
