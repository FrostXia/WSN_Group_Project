CREATE TABLE IF NOT EXISTS ls_nodes (
  node_id TEXT PRIMARY KEY,
  radio_id INTEGER NOT NULL UNIQUE,
  room_id INTEGER NOT NULL DEFAULT 0,
  last_seen TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS ls_packets (
  packet_key TEXT PRIMARY KEY,
  node_id TEXT NOT NULL REFERENCES ls_nodes(node_id),
  boot_id BIGINT,
  sequence BIGINT,
  observed_at TEXT NOT NULL,
  received_at TEXT NOT NULL,
  time_basis TEXT NOT NULL,
  network_json TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS ls_samples (
  packet_key TEXT NOT NULL REFERENCES ls_packets(packet_key),
  sensor_type TEXT NOT NULL,
  value DOUBLE PRECISION,
  unit TEXT NOT NULL,
  recorded_at TEXT NOT NULL,
  quality TEXT NOT NULL,
  PRIMARY KEY(packet_key,sensor_type)
);
CREATE INDEX IF NOT EXISTS ls_packet_node_time ON ls_packets(node_id,observed_at);
CREATE INDEX IF NOT EXISTS ls_sample_time ON ls_samples(recorded_at,sensor_type);
