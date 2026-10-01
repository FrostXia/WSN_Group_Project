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

