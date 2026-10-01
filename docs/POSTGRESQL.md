# 可选：使用PostgreSQL

默认SQLite已足够完成本机实验。只有需要沿用PostgreSQL时做下面步骤。

1. 在pgAdmin创建一个独立数据库，例如 `labmesh_v2`。不会自动创建数据库。
2. 项目目录执行：

```powershell
.\.venv\Scripts\python.exe -m pip install -r requirements-postgres.txt
```

3. 在启动后端的同一个PowerShell中设置连接。建议密码用PGPASSWORD输入，避免写进项目文件或连接URL：

```powershell
$env:DATABASE_URL = 'postgresql://postgres@localhost:5432/labmesh_v2'
$env:PGPASSWORD = Read-Host 'PostgreSQL password'
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\start-app.ps1
```

本次输入会显示在你自己的终端上。不要把包含密码的截图或环境变量列表发给别人。

后端自动执行database/schema.sql，在新数据库建ls_nodes、ls_packets、ls_samples。网关仍使用SQLite暂存队列。

验证：`http://127.0.0.1:5050/api/health` 应显示database=postgresql。

切回SQLite前停止后端，在其启动终端执行：

```powershell
Remove-Item Env:DATABASE_URL -ErrorAction SilentlyContinue
Remove-Item Env:PGPASSWORD -ErrorAction SilentlyContinue
```

再启动后端。两种数据库之间不会自动搬迁历史数据。

当前已自动测试SQLite路径；PostgreSQL分支和DDL未连接真实PostgreSQL验证。原先lab_monitor数据库不改动、不清理。
