# Lab Safety Complete — 从烧录到网页的完整项目

**先读本文件。完整设计见 [PROJECT_DESIGN.md](PROJECT_DESIGN.md)。**

本包包含固件、网关、后端、数据库建表、新版UI、启动脚本及测试。默认全部在电脑本地运行，不需要云账号。

## 一、你将运行什么

```text
ESP32 1、3、4…… -- ESP-NOW直接或多跳 --> ESP32 2（汇总，也接传感器）
                                                    |
                                                 USB串口
                                                    |
                                    Python网关（本地队列、重试）
                                                    |
                                             HTTP上传后端
                                                    |
                                      SQLite数据库 + 新版网页
```

每天使用只需要两个终端：**后端窗口 + 网关窗口**。网页由后端提供，不再运行 `http.server 8000`。默认端口是 **5050**，与之前的5000分开。

## 二、首次安装：只做一次

1. 解压整个ZIP，例如放到 `D:\WSN\A1\LabSafetyComplete`。不要只复制某个文件。
2. 在该文件夹打开PowerShell。
3. 执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\setup.ps1
```

它会建立 `.venv` 并安装Flask、pyserial。此命令只对本次脚本设置执行策略，不修改系统全局策略。

**默认不需要安装PostgreSQL，也不用输入数据库密码。**首次启动会创建 `data/lab.sqlite3`。需要PostgreSQL时再看 [docs/POSTGRESQL.md](docs/POSTGRESQL.md)。

安装依赖需要网络。如果失败，保留报错重试，不要在另一个Python里安装后误以为本项目已装好。

## 三、烧录三块板

### 1. Arduino环境

- Arduino IDE 2.x。
- Boards Manager：安装 `esp32 by Espressif Systems` 的3.x稳定版（ESP-IDF >=5.1）。全组使用同一版本。
- 选择实际开发板；你截图中的板型为 `ESP32S3 Dev Module`，其他板须按实物确认。
- Library Manager：安装 `DHT sensor library`（Adafruit）、`Adafruit Unified Sensor`、`BH1750`（Christopher Laws）。

`WiFi.h`、`esp_now.h`、`esp_wifi.h`、`Wire.h`、FreeRTOS随开发板包提供，不单独安装。

### 2. 每块板打开自己的工程

| 板 | 打开的Arduino文件 | 默认房间 |
|---|---|---|
| 1号 | `firmware/nodes/Node1/Node1.ino` | Room 1 |
| 2号汇总 | `firmware/nodes/Node2/Node2.ino` | Room 1 |
| 3号 | `firmware/nodes/Node3/Node3.ino` | Room 2 |

节点ID已分别设置，不要把同一个Node2工程烧给所有板。

每个工程的 `config.h` 检查：

```cpp
#ifndef ENABLE_SENSORS
#define ENABLE_SENSORS 1
#endif

#ifndef SENSOR_MASK
#define SENSOR_MASK 7
#endif
```

- `ENABLE_SENSORS=0`：只测通信，UI仍显示节点在线但无读数。
- `ENABLE_SENSORS=1`：采集真实传感器。
- `SENSOR_MASK=7`：DHT11+BH1750+PIR；只有DHT11设1，只有BH1750设2，只有PIR设4，DHT11+PIR设5。

默认信号引脚来自你原来的1号ESP32-S3代码：

| 传感器 | 信号GPIO |
|---|---:|
| DHT11 DATA | 5 |
| PIR OUT | 15 |
| BH1750 SDA | 8 |
| BH1750 SCL | 9 |

2、3号按实际接线修改；电源和模块电压依照实物，不把信号引脚表当作完整接线图。

保持所有板的ROOT_ID、NETWORK_ID、信道、成本模式相同。默认ROOT_ID=2、信道6、ETX模式，不需要Wi-Fi密码。

### 3. 上传

逐块选择正确COM口 → 点击验证 → 点击上传。先上电2号，再上电1、3号便于首次观察。串口波特率115200。

**本包为无线协议v2，三块板必须统一烧录，不可与之前v1固件混用。**新网关能读取旧根节点的串口JSON，适合临时观察，但无法给旧包补出真实采样年龄和明确传感器配置。

## 四、打开网页：终端一

在项目目录执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\start-app.ps1
```

浏览器打开：

**http://127.0.0.1:5050**

首次出现 `Waiting for nodes` 正常。可访问 `http://127.0.0.1:5050/api/health`，应返回status=ok。

保持这个终端运行。

## 五、接收硬件：终端二

2号用数据USB线接电脑，其他板保持供电。**关闭Arduino串口监视器**。

另开PowerShell，进入同一项目目录，执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\start-gateway.ps1
```

脚本列出COM口后，输入**2号板**的端口，如 `COM7`。也可直接指定：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\start-gateway.ps1 -Port COM7
```

正常日志：

```text
Serial connected ...
Stored node=3 seq=... valid=7 path=[3, 1, 2]
Uploaded esp32_3:...:...
```

这些是日志形状，实际数值以你的设备为准。网页每5秒刷新，自动出现节点，无需手动增加NODES数组。

## 六、怎样看网页

- **Online**：近期收到该节点数据包，不保证所有传感器正常。
- **Online · no fresh readings**：通信正常，但传感器关闭或没有新鲜有效读数。
- **Sensor fault / Fault**：对应传感器读取失败。
- **No recent packets**：超过120秒没有近期包；不能仅凭此断定设备断电。
- **Stale**：保留的读数已过期。
- **Network**：查看实际到达根节点的路径、源节点第一跳RSSI和ACK概率。
- **Trends**：选一个节点，分别看各指标历史曲线。

页面不生成模拟数据。没有硬件时，空状态是正确结果。

报警默认关闭。要使用，请编辑 `settings.json` 中的阈值并设置 `alerts_enabled=true`，然后重启后端。40℃、20%只是沿用的示例参数，不是已验证的实验室安全标准。

## 七、房间和增加设备

房间不参与路由，设备身份为 `esp32_3`，不再把房间拼入主ID。

不想在板上区分房间，可以把ROOM_ID设0。也可以在 `settings.json` 中配置显示位置，修改后重启后端：

```json
"node_rooms": {
  "esp32_1": "Room A",
  "esp32_3": "Room B"
}
```

增加4、5号：

```powershell
.\.venv\Scripts\python.exe scripts\prepare-firmware.py --nodes 4 5 --root 2
```

打开生成的Node4/Node5工程，核对引脚后上传。此生成脚本会覆盖所选节点工程里的本地配置，修改过的工程先备份。新节点到达后端后自动显示；不要复制相同NODE_ID。

## 八、最终验收

| 测试 | 通过表现 |
|---|---|
| 三块板供电 | 网页自动显示三个节点 |
| 传感器打开 | 温湿度/光照为真实值，PIR对运动有反应 |
| 遮住光照传感器 | Illuminance变化 |
| 3号走中继 | 路径出现3→1→2 |
| 3号断电再恢复 | 其他节点继续上传，3号随后恢复 |
| 当前中继断电 | 有可用备用无线链路时重新选路；没有则暂时无数据 |
| 后端停止再启动 | 网关保留并重传，历史时间不被改成重传时间 |
| 相同包重传 | 数据库不重复插入 |

## 九、关闭与下次启动

两个终端分别按Ctrl+C。下次不必重新安装或烧录，直接重复第四、五步。

数据文件：

- `data/lab.sqlite3`：后端数据库。
- `data/gateway.sqlite3`：网关持久队列，state=0待上传，1已上传，2被后端拒绝。

要备份，先停止两项程序，再复制整个data目录。没有自动删数据任务。

## 十、常见问题

| 问题 | 处理 |
|---|---|
| No module named serial/flask | 运行setup；使用脚本中的.venv，不用不确定的全局Python |
| COM不存在/被占用 | 查看2号当前端口；关闭Arduino监视器；换数据线 |
| Waiting for nodes | 确认网关有Stored和Uploaded；确认接的是2号 |
| 在线但没有读数 | 检查ENABLE_SENSORS、SENSOR_MASK和真实接线；上传修改后的固件 |
| Backend unavailable | 确认后端运行，网址为5050，不是旧版5000 |
| HTTP400 / Rejected | 队列保留错误，检查是否仍使用旧collector或错误后端 |
| 页面仍是旧样式 | 打开5050根地址，Ctrl+F5；不再访问旧8000网页 |
| 只看到直传 | 直传可能成本更低；不能强制认为中继才是成功 |
| 所有板互相收不到 | 检查三块板均为v2、信道和NETWORK_ID一致 |

## 验证说明

20项Python测试、4种UI逻辑测试和本机HTTP检查通过。固件通过API测试桩下的C++语法检查；新版没有完成实体烧录验证。PostgreSQL适配提供代码，未连接实库验证。详细范围见 [docs/VALIDATION.md](docs/VALIDATION.md)。
#   W S N _ G r o u p _ P r o j e c t  
 