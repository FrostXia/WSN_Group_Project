"""Generate separate Arduino sketch folders; default matches the 3-node lab."""
import argparse
import re
from pathlib import Path

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--nodes", type=int, nargs="+", default=[1, 2, 3])
parser.add_argument("--root", type=int, default=2)
args = parser.parse_args()
if not 1 <= args.root <= 65535 or any(not 1 <= n <= 65535 for n in args.nodes):
    parser.error("IDs must be 1..65535")
base = root / "firmware" / "LabMesh"
for node in args.nodes:
    folder = root / "firmware" / "nodes" / f"Node{node}"
    folder.mkdir(parents=True, exist_ok=True)
    config = (base / "config.h").read_text(encoding="utf-8")
    for key, value in {"NODE_ID": node, "ROOT_ID": args.root, "ROOM_ID": 2 if node == 3 else 1}.items():
        config = re.sub(rf"(constexpr uint16_t {key} = )\d+;", rf"\g<1>{value};", config)
    (folder / "config.h").write_text(config, encoding="utf-8")
    (folder / "app.cpp").write_bytes((base / "app.cpp").read_bytes())
    (folder / f"Node{node}.ino").write_bytes((base / "LabMesh.ino").read_bytes())
    print(folder)
