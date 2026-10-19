"""Configure all sketches locally; password is not echoed or logged."""
import getpass
import ipaddress
import json
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
ssid = input('2.4 GHz Wi-Fi name (SSID): ').strip()
password = getpass.getpass('Wi-Fi password (hidden): ')
ip = input('Computer LAN IPv4 (for example 192.168.1.100): ').strip()
bssid = input('Optional same AP BSSID (AA:BB:CC:DD:EE:FF), Enter to skip: ').strip()
address = ipaddress.IPv4Address(ip)
if address.is_loopback or address.is_unspecified or address.is_multicast:
    raise SystemExit('Use the computer LAN address, not localhost/0.0.0.0.')
if not ssid or len(ssid.encode('utf-8')) > 32:
    raise SystemExit('Invalid SSID')
if bssid and not re.fullmatch(r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}', bssid):
    raise SystemExit('Invalid BSSID')
mac = ','.join('0x'+p for p in bssid.split(':')) if bssid else '0,0,0,0,0,0'
quote = lambda s: json.dumps(s, ensure_ascii=False)
config = f'''#pragma once
// Local credentials: do not publish this file or your configured package.
constexpr char LAB_WIFI_SSID[] = {quote(ssid)};
constexpr char LAB_WIFI_PASSWORD[] = {quote(password)};
constexpr char LAB_SERVER[] = "http://{address}:5050";
constexpr bool LAB_LOCK_BSSID = {'true' if bssid else 'false'};
constexpr uint8_t LAB_AP_BSSID[6] = {{{mac}}};
'''
(root/'firmware/LabMesh/wifi_config.h').write_text(config, encoding='utf-8')
subprocess.run([sys.executable, str(root/'scripts/prepare-firmware.py')], check=True)
print('Configured all 3 sketches. Flash Node1, Node2, Node3 to the matching boards.')
