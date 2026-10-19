#pragma once
// Edit these three values, then run scripts/prepare-firmware.py before flashing ALL boards.
constexpr char LAB_WIFI_SSID[] = "YOUR_WIFI_NAME";
constexpr char LAB_WIFI_PASSWORD[] = "YOUR_WIFI_PASSWORD";
// Computer IPv4 address; no trailing slash. NOT 127.0.0.1.
constexpr char LAB_SERVER[] = "http://192.168.1.100:5050";
// Optional: lock every board to the SAME 2.4GHz AP when using mesh/multiple APs.
constexpr bool LAB_LOCK_BSSID = false;
constexpr uint8_t LAB_AP_BSSID[6] = {0,0,0,0,0,0};
