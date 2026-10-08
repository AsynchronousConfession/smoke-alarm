<#
    一次性初始化脚本（装好 Mosquitto 后跑一次即可）

    做三件事:
      1) 用 config.json 里的账号密码生成 Mosquitto 的密码文件与 ACL
      2) 按当前目录生成 mosquitto.conf（里面是绝对路径，所以换目录要重跑本脚本）
      3) 建 Python 虚拟环境并安装后端依赖（只有一个纯 Python 包 paho-mqtt）

    用法:  powershell -ExecutionPolicy Bypass -File .\setup.ps1
#>

param([switch]$Force)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Definition
$cfgPath = Join-Path $root 'config.json'
$mosqDir = Join-Path $root 'mosquitto'
$dataDir = Join-Path $root 'data'
$venvDir = Join-Path $root '.venv'

Write-Host "== 物联网网关 PC 端初始化 ==" -ForegroundColor Cyan
Write-Host "目录: $root`n"

if (-not (Test-Path $cfgPath)) { throw "找不到 config.json" }
$cfg = Get-Content -LiteralPath $cfgPath -Raw -Encoding UTF8 | ConvertFrom-Json

New-Item -ItemType Directory -Force -Path $mosqDir, $dataDir, (Join-Path $root 'logs') | Out-Null

# ---------------------------------------------------------------------------
# 1) Python 虚拟环境
# ---------------------------------------------------------------------------
function Find-Python {
    foreach ($cand in @(
        'D:\Python\python26_4_25\python.exe',
        "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe",
        "$env:LOCALAPPDATA\Programs\Python\Python311\python.exe")) {
        if (Test-Path $cand) { return $cand }
    }
    $c = Get-Command python -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    throw "找不到 Python，请先安装 Python 3.8+"
}

$py = Find-Python
Write-Host "[1/3] Python: $py"
& $py -c "import sys; print('      版本', sys.version.split()[0], '|', '64位' if sys.maxsize > 2**32 else '32位')"

if ($Force -and (Test-Path $venvDir)) { Remove-Item -LiteralPath $venvDir -Recurse -Force }
if (-not (Test-Path (Join-Path $venvDir 'Scripts\python.exe'))) {
    Write-Host "      创建虚拟环境 .venv ..."
    & $py -m venv $venvDir
}
$venvPy = Join-Path $venvDir 'Scripts\python.exe'
$wheelDir = Join-Path $root 'tools'
$reqFile  = Join-Path $root 'backend\requirements.txt'
$haveWheel = @(Get-ChildItem -LiteralPath $wheelDir -Filter 'paho_mqtt-*.whl' -ErrorAction SilentlyContinue).Count -gt 0

if ($haveWheel) {
    # 工程里已经带了 wheel, 离线安装最稳, 也绕开了本机系统代理导致的老版 pip 报错
    Write-Host "      离线安装依赖 (使用 tools\ 里自带的 wheel) ..."
    & $venvPy -m pip install --disable-pip-version-check -q --no-index --find-links $wheelDir -r $reqFile
} else {
    Write-Host "      在线安装依赖 (清华镜像, 已绕过系统代理) ..."
    $env:NO_PROXY = '*'; $env:no_proxy = '*'
    & $venvPy -m pip install --disable-pip-version-check -q -r $reqFile -i https://pypi.tuna.tsinghua.edu.cn/simple
}
if ($LASTEXITCODE -ne 0) { throw "pip 安装失败" }
& $venvPy -c "import paho.mqtt; print('      paho-mqtt 就绪')"

# ---------------------------------------------------------------------------
# 2) Mosquitto 密码文件与 ACL
# ---------------------------------------------------------------------------
Write-Host "`n[2/3] Mosquitto 配置"
$mosqExe   = 'C:\Program Files\mosquitto\mosquitto.exe'
$passwdExe = 'C:\Program Files\mosquitto\mosquitto_passwd.exe'
if (-not (Test-Path $mosqExe)) {
    Write-Host "      未检测到 Mosquitto，请先安装：" -ForegroundColor Yellow
    Write-Host "      双击运行  $root\tools\mosquitto-install-x64.exe" -ForegroundColor Yellow
    Write-Host "      装完后再跑一次本脚本。" -ForegroundColor Yellow
} else {
    # -----------------------------------------------------------------------
    # Mosquitto 在 Windows 上只认 ANSI 路径, 遇到中文目录会启动失败
    #   ("Unable to open pwfile ..." / "Unable to open log file ...")。
    # 所以运行目录分两种情况:
    #   · 工程路径是纯英文(例如 D:\smoke-alarm\04) -> 就放在 server\mosquitto\ 下,
    #     整个工程自包含, 换电脑直接复制;
    #   · 工程路径含中文 -> 退回到一个纯英文的公共运行目录
    #     (默认 D:\A_ESP32_project\smoke_alarm_server)。
    # 两种情况都由 runtime.json 记录, start.ps1 照它启动。
    # -----------------------------------------------------------------------
    function Test-AsciiPath {
        param([string]$p)
        return ($p -match '^[\x20-\x7E]+$')
    }

    $runtimeDir = $null
    if (Test-AsciiPath $root) {
        # 工程路径本身就是纯英文, 直接放在 server\mosquitto 下
        $try = Join-Path $root 'mosquitto'
        try {
            New-Item -ItemType Directory -Force -Path $try | Out-Null
            $runtimeDir = $try
        } catch { }
    }
    if (-not $runtimeDir) {
        foreach ($cand in @($env:SMOKE_SERVER_DIR, 'D:\A_ESP32_project',
                            $env:LOCALAPPDATA, $env:TEMP)) {
            if (-not $cand) { continue }
            if (-not (Test-AsciiPath $cand)) { continue }
            if (-not (Test-Path -LiteralPath (Split-Path $cand -Parent))) { continue }
            $try = Join-Path $cand 'smoke_alarm_server'
            try {
                New-Item -ItemType Directory -Force -Path $try | Out-Null
                $runtimeDir = $try
                break
            } catch { }
        }
    }
    if (-not $runtimeDir) {
        throw "找不到可用的英文路径存放 Mosquitto 运行文件，请设置环境变量 SMOKE_SERVER_DIR 指向一个纯英文目录"
    }

    New-Item -ItemType Directory -Force -Path (Join-Path $runtimeDir 'data'),
                                             (Join-Path $runtimeDir 'logs') | Out-Null
    Write-Host "      运行目录(必须纯英文): $runtimeDir"

    $passwdFile = Join-Path $runtimeDir 'passwd'
    if (Test-Path $passwdFile) { Remove-Item -LiteralPath $passwdFile -Force }

    & $passwdExe -c -b $passwdFile $cfg.mqtt.backend_user $cfg.mqtt.backend_password
    & $passwdExe    -b $passwdFile $cfg.mqtt.device_user  $cfg.mqtt.device_password
    Write-Host "      密码文件已生成: $passwdFile"

    $acl = @"
# 后端: 可以订阅全部主题, 并且只能向 cmd 主题下发
user $($cfg.mqtt.backend_user)
topic read home/#
topic write home/+/cmd/+

# 网关设备: 只能上报自己网关的数据, 并且只能读命令主题
user $($cfg.mqtt.device_user)
topic write home/+/tele/+
topic write home/+/event/+
topic write home/+/state/+
topic write home/+/ack/+
topic read  home/+/cmd/+
"@
    $aclPath = Join-Path $runtimeDir 'acl'
    [System.IO.File]::WriteAllText($aclPath, $acl, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "      ACL 已生成: $aclPath"

    $conf = @"
# 本文件由 setup.ps1 自动生成, 换电脑/换目录后请重新生成
# 注意: 里面必须是纯英文路径 —— Mosquitto 在 Windows 上不支持中文路径
listener $($cfg.mqtt.port) 0.0.0.0
allow_anonymous false
password_file $((Join-Path $runtimeDir 'passwd') -replace '\\','/')
acl_file $((Join-Path $runtimeDir 'acl') -replace '\\','/')
persistence true
persistence_location $((Join-Path $runtimeDir 'data') -replace '\\','/')/
log_dest file $((Join-Path $runtimeDir 'logs\mosquitto.log') -replace '\\','/')
log_type error
log_type warning
log_type notice
log_type information
connection_messages true
"@
# 注意：不要写 "max_keepalive 0" —— mosquitto 2.0.x（Ubuntu 22.04 自带）认为它是非法值，
# 会报 "Error: Invalid max_keepalive value (0)" 直接启动失败；不写就是不限制。
    $confPath = Join-Path $runtimeDir 'mosquitto.conf'
    [System.IO.File]::WriteAllText($confPath, $conf, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "      主配置已生成: $confPath"

    # 记下运行目录, start.ps1 从这里读
    $runtimeInfo = @{ dir = $runtimeDir } | ConvertTo-Json
    [System.IO.File]::WriteAllText((Join-Path $root 'runtime.json'), $runtimeInfo,
                                   (New-Object System.Text.UTF8Encoding($false)))
}

# ---------------------------------------------------------------------------
# 3) 提示
# ---------------------------------------------------------------------------
Write-Host "`n[3/3] 完成`n" -ForegroundColor Cyan
Write-Host "  网页访问口令 : $($cfg.web.access_password)"
Write-Host "  设备 MQTT 账号: $($cfg.mqtt.device_user) / $($cfg.mqtt.device_password)"
Write-Host "  后端 MQTT 账号: $($cfg.mqtt.backend_user) / $($cfg.mqtt.backend_password)"
Write-Host ""
Write-Host "  下一步:" -ForegroundColor Yellow
Write-Host "    1) 把上面的设备账号填进 ESP32 的 main\app_config.h"
Write-Host "    2) 运行 .\start.ps1 启动全部服务"
