<#
    工程环境自检：一条条告诉你缺什么、去哪装。

    用法:
        powershell -ExecutionPolicy Bypass -File .\tools\check_env.ps1

    只读检查，不会修改任何东西。
#>

$ErrorActionPreference = 'Continue'

$projRoot = Split-Path -Parent $PSScriptRoot
$nodeRoot = Join-Path $projRoot 'node_ch573f'
$gwDir    = Join-Path $projRoot 'gateway_esp32s3'
$srvDir   = Join-Path $projRoot 'server'

$script:rows = @()

function Add-Row {
    param([string]$Item, [string]$Ok, [string]$Detail, [string]$Fix)
    $script:rows += [pscustomobject]@{ 项目 = $Item; 结果 = $Ok; 说明 = $Detail; 处理 = $Fix }
}

function Test-AsciiPath {
    param([string]$p)
    return ($p -match '^[\x20-\x7E]+$')
}

Write-Host "===== 家庭室内多节点烟雾报警系统 · 环境自检 =====" -ForegroundColor Cyan
Write-Host "工程目录: $projRoot`n"

# ---- 1. ESP-IDF -------------------------------------------------------------
$idf = $null
foreach ($c in @($env:IDF_PATH, 'D:\Espressif\frameworks\esp-idf-v5.1.2',
                 'C:\Espressif\frameworks\esp-idf-v5.1.2', (Join-Path $env:USERPROFILE 'esp\esp-idf'))) {
    if ($c -and (Test-Path -LiteralPath (Join-Path $c 'export.ps1') -ErrorAction SilentlyContinue)) { $idf = $c; break }
}
if (-not $idf) {
    foreach ($base in @('D:\Espressif\frameworks', 'C:\Espressif\frameworks')) {
        if (Test-Path -LiteralPath $base) {
            $hit = Get-ChildItem -LiteralPath $base -Directory -ErrorAction SilentlyContinue |
                   Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'export.ps1') } |
                   Select-Object -First 1
            if ($hit) { $idf = $hit.FullName; break }
        }
    }
}
if ($idf) { Add-Row 'ESP-IDF (编译网关)' 'OK' $idf '—' }
else { Add-Row 'ESP-IDF (编译网关)' '缺少' '没找到 esp-idf 的 export.ps1' 'VS Code 装 Espressif IDF 扩展，版本选 v5.1.2，路径填 D:\Espressif' }

# ---- 2. MounRiver ----------------------------------------------------------
$mrs = $null
foreach ($base in @('D:\MounRiver', 'C:\MounRiver')) {
    if (Test-Path -LiteralPath $base) {
        $hit = Get-ChildItem -LiteralPath $base -Recurse -Filter 'riscv-none-embed-gcc.exe' -ErrorAction SilentlyContinue |
               Select-Object -First 1
        if ($hit) { $mrs = $hit.DirectoryName; break }
    }
}
if ($mrs) { Add-Row 'MounRiver 工具链 (编译节点)' 'OK' (Split-Path (Split-Path (Split-Path $mrs -Parent) -Parent) -Leaf) '—' }
else { Add-Row 'MounRiver 工具链 (编译节点)' '缺少' '没找到 riscv-none-embed-gcc.exe' '安装 MounRiver Studio 2: http://www.mounriver.com/download' }

# ---- 3. Python -------------------------------------------------------------
$py = $null
foreach ($cand in @('D:\Python\python26_4_25\python.exe',
                    "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe",
                    "$env:LOCALAPPDATA\Programs\Python\Python311\python.exe")) {
    if (Test-Path -LiteralPath $cand) { $py = $cand; break }
}
if (-not $py) { $cmd = Get-Command python -ErrorAction SilentlyContinue; if ($cmd) { $py = $cmd.Source } }
if ($py) {
    $ver = & $py -c "import sys;print('%d.%d.%d'%sys.version_info[:3])" 2>$null
    Add-Row 'Python 3.8+ (跑服务器)' 'OK' ("Python {0} @ {1}" -f $ver, $py) '—'
} else {
    Add-Row 'Python 3.8+ (跑服务器)' '缺少' 'PATH 里没有 python' 'https://www.python.org/downloads/ （安装时勾 Add python to PATH）'
}

# ---- 4. Mosquitto ----------------------------------------------------------
$mosq = 'C:\Program Files\mosquitto\mosquitto.exe'
if (Test-Path -LiteralPath $mosq) { Add-Row 'Mosquitto (MQTT 服务器)' 'OK' $mosq '—' }
else { Add-Row 'Mosquitto (MQTT 服务器)' '缺少' 'C:\Program Files\mosquitto 下没有 mosquitto.exe' ("双击安装: " + (Join-Path $srvDir 'tools\mosquitto-install-x64.exe')) }

# ---- 5. cloudflared --------------------------------------------------------
$cfd = Join-Path $srvDir 'tools\cloudflared.exe'
if (Test-Path -LiteralPath $cfd) { Add-Row 'cloudflared (内网穿透)' 'OK' '已在 server\tools\ 里' '—' }
else { Add-Row 'cloudflared (内网穿透)' '缺少' 'server\tools\cloudflared.exe 不存在' 'https://github.com/cloudflare/cloudflared/releases' }

# ---- 6. 节点工程的相对依赖 --------------------------------------------------
$need = @((Join-Path $nodeRoot 'SRC\Ld\Link.ld'),
          (Join-Path $nodeRoot 'node\HAL\include\config.h'),
          (Join-Path $nodeRoot 'node\LIB\CH57xBLE_LIB.h'),
          (Join-Path $nodeRoot 'node\SmokeNode\APP\peripheral_main.c'))
$missing = @($need | Where-Object { -not (Test-Path -LiteralPath $_) })
if ($missing.Count -eq 0) { Add-Row '节点工程相对依赖 (SRC/HAL/LIB)' 'OK' '齐全' '注意：这三个目录的相对位置不能移动' }
else { Add-Row '节点工程相对依赖 (SRC/HAL/LIB)' '缺少' ($missing -join '; ') '把整个工程一起复制/移动，不要只拷 SmokeNode' }

# ---- 7. 网关工程文件 --------------------------------------------------------
$gwNeed = @((Join-Path $gwDir 'CMakeLists.txt'),
            (Join-Path $gwDir 'sdkconfig.defaults'),
            (Join-Path $gwDir 'main\gateway_main.c'),
            (Join-Path $gwDir 'main\wifi_mqtt.c'),
            (Join-Path $gwDir 'main\app_config.h'))
$gwMissing = @($gwNeed | Where-Object { -not (Test-Path -LiteralPath $_) })
if ($gwMissing.Count -eq 0) { Add-Row '网关工程文件' 'OK' '齐全' '—' }
else { Add-Row '网关工程文件' '缺少' ($gwMissing -join '; ') '检查文件是否被杀毒软件删除' }

# ---- 8. 端口 -----------------------------------------------------------------
$busy = @()
foreach ($port in @(1883, 3000)) {
    if (Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue) { $busy += $port }
}
if ($busy.Count -eq 0) { Add-Row '端口 1883 / 3000' 'OK' '都空闲' '—' }
else { Add-Row '端口 1883 / 3000' '被占用' ("端口 " + ($busy -join ', ') + " 正在被监听") '可能是已经启动过服务；要重启先执行 server\start.ps1 -Stop' }

# ---- 9. 中文路径提醒 ---------------------------------------------------------
if (Test-AsciiPath $projRoot) {
    Add-Row '工程路径' 'OK' '纯英文路径，所有工具都能直接用' '—'
} else {
    Add-Row '工程路径' '含中文' ($projRoot) '网关编译脚本会自动用英文镜像目录；Mosquitto 运行文件放在英文目录（setup.ps1 自动处理）'
}
$rtFile = Join-Path $srvDir 'runtime.json'
if (Test-Path -LiteralPath $rtFile) {
    $rt = (Get-Content -LiteralPath $rtFile -Raw -Encoding UTF8 | ConvertFrom-Json).dir
    Add-Row 'Mosquitto 运行目录' 'OK' $rt '—'
} elseif (Test-Path -LiteralPath (Join-Path $srvDir '.venv\Scripts\python.exe')) {
    Add-Row 'Mosquitto 运行目录' '未配置' '还没有 runtime.json' '在 server 目录执行一次 setup.ps1'
} else {
    Add-Row 'Mosquitto 运行目录 / .venv' '未初始化' '还没跑过 setup.ps1' '在 server 目录执行一次 setup.ps1'
}

# ---- 10. 网关怎么编译 --------------------------------------------------------
if (Test-AsciiPath $projRoot) {
    Add-Row '网关编译方式' 'OK' '工程在纯英文路径, 直接在 gateway_esp32s3 里编译' 'VS Code 里请打开 gateway_esp32s3 文件夹（不是工程根目录）'
} else {
    $mirrorRoot = if ($env:SMOKE_GW_MIRROR) { $env:SMOKE_GW_MIRROR } elseif (Test-Path 'D:\A_ESP32_project') { 'D:\A_ESP32_project' } else { $env:LOCALAPPDATA }
    Add-Row '网关编译方式' '镜像目录' (Join-Path $mirrorRoot 'smoke_gateway_build') '工程路径含中文, 由 tools\build_gateway.ps1 自动同步到英文镜像后编译'
}

# ---- 输出 -------------------------------------------------------------------
Write-Host "===== 检查结果 =====" -ForegroundColor Cyan
$script:rows | Format-Table -AutoSize | Out-String -Width 200 | Write-Host

$bad = @($script:rows | Where-Object { $_.结果 -notin @('OK', '含中文') })
if ($bad.Count -eq 0) {
    Write-Host "全部通过，可以直接开始编译烧写。" -ForegroundColor Green
    Write-Host "下一步: docs\04_编译烧写与联调步骤.md" -ForegroundColor Green
} else {
    Write-Host ("还有 {0} 项需要处理，请看上面『处理』列。" -f $bad.Count) -ForegroundColor Yellow
}
