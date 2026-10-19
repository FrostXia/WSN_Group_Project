# Sensor update verification — 2026-10-06

- Python integration tests: 27 passed using isolated SQLite databases.
- Node UI logic tests: 7 scenarios passed.
- Headless Chrome with actual Flask test-client responses: passed. Checked DHT-only node, hidden missing inputs, persisted PIR switch, no old value immediately after enable, valid No motion after a fresh packet, immediate hide after disable, desktop/mobile layout, and no browser JavaScript errors. Browser-to-Flask test traffic was bridged over stdio because this environment blocked localhost browser connections; live HTTP deployment was not tested.
- Firmware: sensor code shared by all three node sketches; source consistency and protocol version checked. Arduino CLI compilation attempted with installed Arduino-ESP32 3.3.11. The Xtensa compiler failed to launch with Windows Illegal System DLL Relocation, including --version. Compilation is NOT verified; no firmware binaries or physical test results are claimed.
- Physical checks still required: compile and flash all three boards; verify GPIO4 receives LD2410B TX at 256000 baud; test DHT11/BH1750 disconnect/reconnect, radar timeout, manual switches, ADC inputs, dust LED pulse width and ADC acquisition window, and routing with the new version-3 packet.
- The live databases were not modified by these tests. Server startup applies the additive control-table migration. Source backup was created before edits.
