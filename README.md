> Wi-Fi 版请先阅读 [先读我_WiFi烧录与启动.md](先读我_WiFi烧录与启动.md)。三块板均需重新烧录，电脑使用 scripts/start-wifi-app.ps1，不运行串口网关。下方为旧版参考说明。

> **GP2Y v2 完整包：请先阅读 [开始使用_中文版.md](开始使用_中文版.md)。粉尘接线以 [BC337新图](docs/GP2Y_BC337.svg) 为准，旧文档中的BC549/1k+560Ω接法不适用于本版。**

\## Requirements



\- Windows PowerShell

\- Python 3.11

\- Git

\- Arduino IDE with Arduino-ESP32 3.x

\- Arduino libraries: DHT sensor library by Adafruit, Adafruit Unified Sensor, BH1750 by Christopher Laws

\- ESP32-S3 boards with node firmware installed

\- USB data connection between the root board and the computer



\## Installation



```powershell

git clone https://github.com/FrostXia/WSN\_Group\_Project.git

cd WSN\_Group\_Project

powershell -ExecutionPolicy Bypass -File .\\scripts\\setup.ps1

```



\## Run the Server — Terminal 1



```powershell

powershell -ExecutionPolicy Bypass -File .\\scripts\\start-app.ps1

```



\## Run the Gateway — Terminal 2



```powershell

powershell -ExecutionPolicy Bypass -File .\\scripts\\start-gateway.ps1

```



\## Open the Dashboard — Terminal 3



```powershell

Start-Process "http://127.0.0.1:5050"

```

