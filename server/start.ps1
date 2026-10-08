<#
    一键启动 / 停止 PC 端全部服务

      .\start.ps1              启动 Mosquitto + 后端 + Cloudflare 隧道
      .\start.ps1 -Stop        停止由本脚本启动的进程
      .\start.ps1 -ShowLog     另开窗口实时看日志

    注意: 每次换网络环境(热点 IP 变化)后, ESP32 的 MQTT 地址要跟着改。
#>

param(
    [switch]$Stop,
    [switch]$ShowLog
)

$ErrorActionPreference = 'Stop'
$root    = Split-Path -Parent $MyInvocation.MyCommand.Definition
$cfgPath = Join-Path $root 'config.json'
$runDir  = Join-Path $root '.run'
$logDir  = Join-Path $root 'logs'
$pidFile = Join-Path $runDir 'pids.json'

New-Item -ItemType Directory -Force -Path $runDir, $logDir | Out-Null
$cfg = Get-Content -LiteralPath $cfgPath -Raw -Encoding UTF8 | ConvertFrom-Json

$mosqExe      = 'C:\Program Files\mosquitto\mosquitto.exe'
$venvPy       = Join-Path $root '.venv\Scripts\python.exe'
$backend      = Join-Path $root 'backend\app.py'
$cloudflared  = Join-Path $root 'tools\cloudflared.exe'
$tunnelToken  = $cfg.cloudflare.tunnel_token

# Mosquitto 不支持中文路径, 它的配置/密码/ACL/日志都在 setup.ps1 选定的
# 纯英文"运行目录"里（记在 runtime.json）。找不到就提示先跑 setup.ps1。
$runtimeDir = $null
$runtimeFile = Join-Path $root 'runtime.json'
if (Test-Path -LiteralPath $runtimeFile) {
    try { $runtimeDir = (Get-Content -LiteralPath $runtimeFile -Raw -Encoding UTF8 | ConvertFrom-Json).dir } catch { $runtimeDir = $null }
}
$mosqConf = if ($runtimeDir) { Join-Path $runtimeDir 'mosquitto.conf' } else { $null }

# ---------------------------------------------------------------------------
# 停止
# ---------------------------------------------------------------------------
if ($Stop) {
    if (-not (Test-Path $pidFile)) { Write-Host "没有找到运行时记录，可能没启动过。" -ForegroundColor Yellow; return }
    $rec = Get-Content -LiteralPath $pidFile -Raw | ConvertFrom-Json
    foreach ($name in @('mosquitto', 'backend', 'cloudflared')) {
        $procId = $rec.$name
        if ($procId) {
            try {
                Stop-Process -Id $procId -Force -ErrorAction Stop
                Write-Host "已停止 $name (PID $procId)" -ForegroundColor Green
            } catch {
                Write-Host "$name (PID $procId) 已不在运行" -ForegroundColor DarkGray
            }
        }
    }
    Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
    return
}

# ---------------------------------------------------------------------------
# 启动前的检查
# ---------------------------------------------------------------------------
Write-Host "== 启动物联网网关 PC 端 ==" -ForegroundColor Cyan

if (-not (Test-Path $mosqExe)) {
    Write-Host "还没有安装 Mosquitto（MQTT 服务器）。" -ForegroundColor Yellow
    Write-Host "  请先双击运行: $root\tools\mosquitto-install-x64.exe" -ForegroundColor Yellow
    Write-Host "  然后执行    : .\setup.ps1" -ForegroundColor Yellow
    return
}
if (-not (Test-Path $venvPy)) {
    Write-Host "还没初始化 Python 环境，请先执行: .\setup.ps1" -ForegroundColor Yellow
    return
}
if (-not (Test-Path $mosqConf)) {
    Write-Host "缺少 Mosquitto 配置（runtime.json / mosquitto.conf），请先执行: .\setup.ps1" -ForegroundColor Yellow
    return
}

function Test-PortInUse([int]$Port) {
    return [bool](Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue)
}

$started = [ordered]@{ mosquitto = $null; backend = $null; cloudflared = $null }

# ---------------------------------------------------------------------------
# 1) Mosquitto
# ---------------------------------------------------------------------------
if (Test-PortInUse $cfg.mqtt.port) {
    Write-Host "[MQTT] 端口 $($cfg.mqtt.port) 已在监听，跳过启动" -ForegroundColor Yellow
} else {
    $p = Start-Process -FilePath $mosqExe `
            -ArgumentList @('-c', $mosqConf) `
            -WorkingDirectory $runtimeDir -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $runtimeDir 'logs\mosquitto.out.log') `
            -RedirectStandardError  (Join-Path $runtimeDir 'logs\mosquitto.err.log')
    $started.mosquitto = $p.Id
    Write-Host "[MQTT] Mosquitto 已启动 (PID $($p.Id))，端口 $($cfg.mqtt.port)" -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# 2) 后端 (含网页)
# ---------------------------------------------------------------------------
if (Test-PortInUse $cfg.web.port) {
    Write-Host "[WEB ] 端口 $($cfg.web.port) 已在监听，跳过启动" -ForegroundColor Yellow
} else {
    $p = Start-Process -FilePath $venvPy `
            -ArgumentList @('-u', $backend) `
            -WorkingDirectory (Join-Path $root 'backend') -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $logDir 'backend.out.log') `
            -RedirectStandardError  (Join-Path $logDir 'backend.err.log')
    $started.backend = $p.Id
    Write-Host "[WEB ] 后端已启动 (PID $($p.Id))，端口 $($cfg.web.port)" -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# 3) Cloudflare 隧道
# ---------------------------------------------------------------------------
if ([string]::IsNullOrWhiteSpace($tunnelToken)) {
    Write-Host "[隧道] 未填写 cloudflare.tunnel_token，跳过（手机暂时只能用局域网访问）" -ForegroundColor Yellow
} elseif (-not (Test-Path $cloudflared)) {
    Write-Host "[隧道] 找不到 tools\cloudflared.exe" -ForegroundColor Yellow
} else {
    $p = Start-Process -FilePath $cloudflared `
            -ArgumentList @('tunnel', '--no-autoupdate', 'run', '--token', $tunnelToken) `
            -WorkingDirectory $root -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $logDir 'cloudflared.out.log') `
            -RedirectStandardError  (Join-Path $logDir 'cloudflared.err.log')
    $started.cloudflared = $p.Id
    Write-Host "[隧道] cloudflared 已启动 (PID $($p.Id))" -ForegroundColor Green
}

$started | ConvertTo-Json | Set-Content -LiteralPath $pidFile -Encoding UTF8

# ---------------------------------------------------------------------------
# 提示信息
# ---------------------------------------------------------------------------
Start-Sleep -Seconds 2
Write-Host "`n---- 访问信息 ----" -ForegroundColor Cyan
Write-Host ("  本机网页 : http://127.0.0.1:{0}" -f $cfg.web.port)
Write-Host ("  访问口令 : {0}" -f $cfg.web.access_password)
if (-not [string]::IsNullOrWhiteSpace($tunnelToken)) {
    Write-Host "  公网地址 : 见 Cloudflare 后台配置的域名（手机用这个）"
}

Write-Host "`n  本机网卡地址（把 ESP32 所在网络的那个填进 app_config.h 的 APP_MQTT_URI）:" -ForegroundColor Yellow
Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } |
    Sort-Object InterfaceAlias |
    ForEach-Object {
        $tag = if ($_.IPAddress -like '192.168.137.*') { '  <== 移动热点，ESP32 通常用这个' } else { '' }
        Write-Host ("    {0,-16} {1}{2}" -f $_.IPAddress, $_.InterfaceAlias, $tag)
    }

Write-Host "`n  日志目录 : $logDir" -ForegroundColor DarkGray
if ($runtimeDir) { Write-Host "  Mosquitto 运行目录 : $runtimeDir" -ForegroundColor DarkGray }
Write-Host "  停止服务 : .\start.ps1 -Stop" -ForegroundColor DarkGray

if ($ShowLog) {
    $mosqLog = if ($runtimeDir) { Join-Path $runtimeDir 'logs\mosquitto.err.log' } else { Join-Path $logDir 'mosquitto.err.log' }
    Start-Process powershell -ArgumentList @(
        '-NoExit', '-Command',
        "Get-Content -Wait -Tail 20 '$logDir\backend.out.log','$logDir\backend.err.log','$mosqLog','$logDir\cloudflared.err.log'"
    )
}
