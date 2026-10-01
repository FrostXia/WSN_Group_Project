param([string]$Port, [string]$Url = 'http://127.0.0.1:5050/api/upload')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $projectRoot
if (-not (Test-Path -LiteralPath '.venv\Scripts\python.exe')) { throw 'Run scripts\setup.ps1 first.' }
if (-not $Port) {
    & '.\.venv\Scripts\python.exe' -m gateway.collector --list
    $Port = Read-Host 'Enter ROOT board port (e.g. COM7)'
}
& '.\.venv\Scripts\python.exe' -m gateway.collector --port $Port --url $Url
