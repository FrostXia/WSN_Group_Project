# 检查记录

## 已完成

- Python 3.11环境：20项unittest通过。包含原始包到API到数据库查询、动态节点、房间变化、空传感器包、故障值、乱序、重启序号、去重、HTTP重试及永久拒绝。
- Node：4种UI逻辑场景通过（空库、在线无传感器、有效读数、服务不可用），并检查DOM引用ID存在。
- `node --check ui/app.js`通过。
- 本机独立测试端口5058：页面HTTP200，/api/health返回SQLite正常。
- 固件使用Espressif RISC-V GCC，C++17、-Wall -Wextra -Werror，在硬件API声明测试桩下通过语法检查。

## 未完成

- 新版v2固件没有完整Arduino核心链接构建，也没有真实烧录测试；旧版曾由用户截图验证直传/中继，但不能替代新版验证。
- UI未做真实浏览器视觉和交互验收；Node测试不等于浏览器渲染测试。
- 未连接实体串口、实际PostgreSQL或用户原数据库进行自动测试。
- 没有实测能耗、续航、最大节点数、无线丢包率或故障恢复上界。

## 用户复测

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\test.ps1
```

如果安装了Node，可额外运行：

```powershell
node tests/ui_smoke.cjs
```

测试使用临时数据库和内存HTTP模拟，不给运行数据库写入模拟传感器数据。上板步骤和验收清单见README。

requirements-tested.txt记录本次测试环境依赖版本。日常安装用requirements.txt；需要尽量复现实验环境时可安装requirements-tested.txt。
