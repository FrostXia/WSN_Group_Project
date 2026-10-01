# 多房间无线实验室监测系统设计

## 1. 目标与范围

多个ESP32采集实验室环境数据，自主选择通向汇总节点的下一跳。后端保存数据，网页显示节点、传感器和路由状态。三块板用于验证机制，代码不固定三节点。

当前传感器：DHT11温湿度、BH1750光照、PIR运动。氧气、烟雾、气体、摄像头可后续接入，但当前固件没有这些驱动。

## 2. 节点角色

| 角色 | 工作 |
|---|---|
| 根节点，默认2号 | 采集自己的传感器，接收其他节点包，输出串口JSON，发根回执 |
| 普通节点 | 采集、选父节点、转发、逐跳确认，可通过配置禁止承担中继 |
| PC网关 | 读取根串口，持久暂存，格式转换，HTTP重试 |
| 后端 | 原子入库、去重、动态注册、查询、同源提供UI |

单根故障会影响全网最终交付，本版不提供备用根自动接管。

## 3. 通信选择

当前无线底层：ESP-NOW，直接指定MAC发送，无AP关联。应用层自行实现多跳；使用同一信道，未加密，NETWORK_ID只是分组标识。

根到电脑：USB串口115200。使用串口可以保持无线信道固定，减少实验变量。需要脱离电脑USB时，可增加根节点HTTP/MQTT出口，但本包没有实现板上MQTT。

电脑到后端：HTTP POST `/api/upload`；根回执不等于HTTP落库回执。

替代方案：ESP-WIFI-MESH提供真实无线多跳并允许自定义父节点选择。普通同AP Wi-Fi+UDP通常经过AP，不能直接代替节点间无线链路能耗实验。当前提供的代码只实现ESP-NOW。

## 4. 建立与维护路由

每节点维护最多8个邻居，其状态包括MAC、ID、启动标识、RSSI、应用ACK成功率、到根累计代价、路径和通告年龄。

候选必须满足：

1. 近期收到应用ACK，确认本地往返链路。
2. 根路径通告未过期且允许转发。
3. 通告路径不含自己，路径长度没有超限。
4. ACK成功率及RSSI达到实验门槛。

选择代价：`C_i(j)=L_ij+C_j`。根代价0，无路由为无穷大。

### 默认ETX近似

`L_ij=1/q_ij`，成功 `q=0.9q+0.1`，失败 `q=0.9q`。这是应用层尝试概率，包含ACK结果，不能再乘一次反向交付率；也不是底层无线帧真实ETX。

ACK携带接收方测到的正向RSSI。不能仅凭本机收到邻居广播的RSSI认定正向链路等价。

### 能耗模式

`L_ij=e_ij/q_ij`，e为同一次应用尝试的通信能量mJ。由各板 `attemptEnergyMj(next)` 提供实测值。没有值的链路不可用，不伪造能耗。

该模型适合逐跳重试的近似比较。实际全网能耗还必须测量监听、探测、排队、传感器和电脑网关的统计范围；3号省电不一定等于全网省电。默认固定供电和发射设置，先不同时引入深睡与功率控制。

### 维护

正常数据直接复用已选父节点。每5秒比较缓存，不逐包扫描全网。

- 新路线至少便宜15%，连续3轮成立才正常切换。
- 原路线不可用时跳过正常滞回，在下一次可执行评估时修复。
- 通告窗口由2秒自适应增至32秒，随机发送；路由改变时缩短。
- 候选探测每2秒轮询一个，ACK有效45秒；路由通告100秒过期。

这些都是待标定的默认参数。通告借鉴Trickle，但没有完整冗余抑制；不能称为完整Trickle/RPL实现。固定探测轮询仍消耗无线能量，可作为下一阶段优化。

## 5. 包格式与可靠性

包种类：ADV、PROBE、ACK、DATA、RECEIPT。无线协议v2，固定结构小于250字节，仅面向一致版本ESP32固件。

主要字段：网络ID、版本、成本类型、发送者、原始来源、根ID、启动ID、序号、ACK令牌、路径、传感器有效位、传感器配置、读数、累计排队年龄。

- 原始来源不因转发改变。
- 通告中检查路径重复；DATA接收和发送时检查已访问节点及候选路径，拒绝循环。
- 最长8节点/7跳，每节点待发16条，去重128条。
- 每跳最多3次尝试，每次默认500ms超时。
- 数据每节点队列期限120秒；反向回执8秒。FIFO队列可能产生队头阻塞。
- ACK表示已接受到内存队列。中继随后掉电仍会丢包。
- 根回执向源返回；源目前不持久保留到最终回执，不提供完整端到端重传。
- 断电清空板上状态；根启动标识通过通告传播，但不是完整RPL版本一致性算法。

这是可运行研究原型，不保证零丢包或任意规模容量。

## 6. 时间与设备状态

固件累计本地排队及应用重试等待时间。网关按“收到时刻减累计年龄”估算采样UTC，标记`gateway_estimated`；未包含精确无线空口和USB缓冲延迟，不作为严格延迟测量时钟。

旧串口包没有年龄时标记`gateway_receive`。两者均不冒充经过同步的源时钟。

网关重试保持原`observed_at`和样本时间。数据库的`received_at`为后端提交时刻。网页分别用近期包判断可达、样本时间判断新鲜、quality判断传感器故障。

## 7. 数据库设计

默认SQLite。相同DDL兼容可选PostgreSQL，使用新表前缀ls_，不会修改旧sensor_nodes/sensor_data表。

| 表 | 键 | 内容 |
|---|---|---|
| ls_nodes | node_id | 稳定设备ID、无线编号、房间、最近观测 |
| ls_packets | packet_key | 原始来源、boot/seq、观测/接收时间、时间依据、路径及第一跳质量JSON |
| ls_samples | packet_key+sensor_type | 值、单位、记录时间、质量 |
| 网关spool（独立SQLite） | packet_key | 完整待上传JSON、状态、错误、首次接收时间 |

时间统一为带时区的UTC ISO8601文本，便于SQLite/PostgreSQL同一代码路径；正式大规模PostgreSQL可迁移为TIMESTAMPTZ/JSONB，但当前代码不依赖它们。

包键为`esp32_<id>:<boot>:<sequence>`。一个包及其样本在同一事务插入；重复包不推进last_seen。旧格式通过`Idempotency-Key`或内容摘要兼容去重。

不自动删除历史。备份前停止程序；长期部署需添加容量、保留和迁移策略。

## 8. API契约

保留参考格式node_id+samples，并增加独立元数据：

```json
{
  "node_id": "esp32_3",
  "room_id": 2,
  "boot_id": 123,
  "sequence": 10,
  "observed_at": "2026-09-06T00:00:00.000+00:00",
  "time_basis": "gateway_estimated",
  "samples": [
    {"sensor_type":"temperature","value":25.5,"unit":"°C","quality":"valid","recorded_at":"2026-09-05T23:59:59.900+00:00"}
  ],
  "network": {"path":[3,1,2],"path_cost":2.1,"link_q":0.95,"link_rssi":-65,"metric":"etx_app"}
}
```

上述是接口示例，不会默认插入运行库。sensor_mask=0时samples可为空，节点仍注册在线。启用但读取失败时quality=fault，value=null。

| 接口 | 用途 |
|---|---|
| GET / | 新版UI |
| GET /api/health | 数据库与服务检查 |
| POST /api/upload | 新/旧格式上报、校验、原子去重 |
| GET /api/nodes | 动态节点列表 |
| GET /api/latest/esp32_3 | 每类传感器最新一条，含故障 |
| GET /api/history?node_id=esp32_3&hours=1 | 指定节点历史；limit上限5000，有truncated提示 |
| GET /api/network | 最后收到的路径与第一跳质量 |
| GET /api/settings | 阈值与显示设置 |
| GET /api/snapshot | 页面一次获取节点、最新值、网络和设置 |

历史hours限制为0到168小时。UI只显示最近5000条，不宣称覆盖全部长时间历史。

## 9. UI

沿用用户新版深色/青绿色布局和单节点多指标趋势。改动：动态节点、光照、故障与过期读数、实际路径表、同源API、无外部字体依赖。图表横轴按时间，故障和长间断处断线。

根收到的包仅说明该条数据的实际路径；RSSI/q为源节点采样时的第一跳快照，成本也可能早于最终转发路径。页面不是全网邻居图，不应据此声称实时完整拓扑。

阈值配置集中于settings.json，默认关闭告警；页面不将无数据判为安全。

## 10. 部署与扩展

默认本机127.0.0.1:5050，无公网暴露、无登录配置。需要其他电脑访问时，应另行配置合适的服务进程、监听地址、访问控制及HTTPS，而不是直接公开本实验端口。

目前无线容量限制来自邻居表、单信道、根附近负载、队列和控制流量。新增节点自动注册只代表软件身份层扩展，不等于实测支持数百节点。

建议下一阶段：源端最终回执重传、中继持久队列、低电量测量及中继退出、协调休眠、更多节点压力测试、ESP-WIFI-MESH对照。

## 11. 参考依据

- [Espressif ESP-NOW](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32/api-reference/network/esp_now.html)：收发、RSSI元数据、应用ACK与peer限制。
- [ESP-WIFI-MESH](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/esp-wifi-mesh.html)：真实多跳与父节点选择替代方案。
- [De Couto等，ETX，MobiCom 2003](https://www.cs.cmu.edu/~dga/15-849/papers/decouto-etx2003.pdf)：累计预期发送成本。
- [RFC 6550：RPL](https://www.rfc-editor.org/rfc/rfc6550.html)：向根汇聚、防环与修复思路。
- [RFC 6719：MRHOF](https://www.rfc-editor.org/rfc/rfc6719.html)：最小累计成本和滞回。
- [RFC 6206：Trickle](https://www.rfc-editor.org/rfc/rfc6206.html)：稳定期低频维护。
- [RFC 6551](https://www.rfc-editor.org/rfc/rfc6551.html)：节点能量与约束。
- [Banerjee与Misra，MobiHoc 2002](https://research.ibm.com/publications/minimum-energy-paths-for-reliable-communication-in-multi-hop-wireless-networks)：可靠交付的重传感知能耗。

代码是简化实现，参数不是上述标准的默认值，也没有复制成完整RPL实现。
