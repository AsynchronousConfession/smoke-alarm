<#
    编译 / 烧写 ESP32-S3 网关固件

    用法:
        powershell -ExecutionPolicy Bypass -File .\tools\build_gateway.ps1
        powershell -ExecutionPolicy Bypass -File .\tools\build_gateway.ps1 -FullClean
        powershell -ExecutionPolicy Bypass -File .\tools\build_gateway.ps1 -Port COM8 -Flash
        powershell -ExecutionPolicy Bypass -File .\tools\build_gateway.ps1 -Port COM8 -Flash -Monitor

    【脚本会自动选择编译位置】
      · 工程路径是纯英文（例如 D:\smoke-alarm\04）→ 直接在 gateway_esp32s3 里编译，
        和 idf.py 手工编译完全一样；
      · 工程路径含中文 → ESP-IDF 的 ninja 1.10.2 用的是 ANSI 版文件 API
        （FindFirstFileExA），会报"文件名、目录名或卷标语法不正确"，
        这时脚本先把 gateway_esp32s3 同步到一个纯英文镜像目录再编译。

    ★ 无论哪种情况，源码永远以工程目录里的 gateway_esp32s3 为准；
      只有在"中文路径"模式下才会用到镜像目录，改完跑一次本脚本即可。

    镜像目录默认: D:\A_ESP32_project\smoke_gateway_build
    （可用环境变量 SMOKE_GW_MIRROR 指定别的位置，必须是纯英文路径）
#>

param(
    [switch]$FullClean,     # 删除镜像里的 build 目录后重新配置（换 sdkconfig 时必须）
    [switch]$Clean,         # 让 idf.py 重新编译（不删 build 目录）
    [switch]$NoSync,        # 不同步源码，直接用镜像里现有的代码编译（调试脚本时用）
    [string]$Port = '',     # 串口，例如 COM8
    [switch]$Flash,         # 编译并烧写
    [switch]$Monitor        # 烧写后打开串口监视器（Ctrl+] 退出）
)

$ErrorActionPreference = 'Continue'

$projRoot = Split-Path -Parent $PSScriptRoot
$srcDir   = Join-Path $projRoot 'gateway_esp32s3'

if (-not (Test-Path -LiteralPath $srcDir)) { throw "找不到工程目录: $srcDir" }

# ---- 1. 决定编译位置：纯英文路径就原地编译，中文路径才用镜像 -----------------
function Test-AsciiPath {
    param([string]$p)
    return ($p -match '^[\x20-\x7E]+$')
}

$useMirror = -not (Test-AsciiPath $srcDir)
$buildDir  = $srcDir

if (-not $useMirror) {
    Write-Host "工程路径是纯英文，直接在工程目录里编译: $srcDir" -ForegroundColor Cyan
} else {
    Write-Host "工程路径含中文，将同步到英文镜像目录后编译" -ForegroundColor Yellow
    $mirrorRoot = $null
    foreach ($cand in @($env:SMOKE_GW_MIRROR, 'D:\A_ESP32_project', $env:LOCALAPPDATA, $env:TEMP)) {
        if ($cand -and (Test-AsciiPath $cand) -and (Test-Path -LiteralPath (Split-Path $cand -Parent))) {
            $mirrorRoot = $cand
            break
        }
    }
    if (-not $mirrorRoot) { throw "找不到可用的英文路径作为编译镜像目录，请设置环境变量 SMOKE_GW_MIRROR" }

    $mirrorDir = Join-Path $mirrorRoot 'smoke_gateway_build'
    New-Item -ItemType Directory -Force -Path $mirrorDir | Out-Null
    $buildDir = $mirrorDir

    # ---- 2. 同步源码到镜像 --------------------------------------------------
    if (-not $NoSync) {
        Write-Host "同步源码 -> $mirrorDir" -ForegroundColor Cyan
        # /MIR: 让镜像和源码完全一致（会删除镜像里多余的文件）
        # /XD build: 不覆盖镜像里的 build 目录，避免每次都要全量重编
        $robolog = & robocopy $srcDir $mirrorDir /MIR /XD build /XF sdkconfig /NFL /NDL /NJH /NJS /NP 2>&1
        if ($LASTEXITCODE -ge 8) {
            $robolog | ForEach-Object { Write-Host $_ -ForegroundColor Red }
            throw "robocopy 同步失败 (exit=$LASTEXITCODE)"
        }
        Write-Host "同步完成" -ForegroundColor DarkGray
    }
}

# ---- 3. 激活 ESP-IDF 并编译 -------------------------------------------------
. (Join-Path $PSScriptRoot 'idf_env.ps1')
Set-Location $buildDir

function Invoke-Idf {
    param([string[]]$IdfArgs)
    Write-Host (">>> idf.py " + ($IdfArgs -join ' ')) -ForegroundColor Cyan
    idf.py @IdfArgs
    if ($LASTEXITCODE -ne 0) { throw ("idf.py 失败: " + ($IdfArgs -join ' ')) }
}

if ($FullClean) {
    $outDir = Join-Path $buildDir 'build'
    if (Test-Path -LiteralPath $outDir) {
        Write-Host "删除旧的 build 目录: $outDir" -ForegroundColor Yellow
        [System.IO.Directory]::Delete($outDir, $true)
    }
}

if ($Clean) { Invoke-Idf @('fullclean') }

if ($Flash) {
    if (-not $Port) { throw "烧写需要指定串口，例如 -Port COM8（设备管理器里能看到）" }
    if ($Monitor) {
        Invoke-Idf @('-p', $Port, 'flash', 'monitor')
    } else {
        Invoke-Idf @('-p', $Port, 'flash')
    }
} else {
    Invoke-Idf @('build')
}

Write-Host ""
Write-Host "完成。" -ForegroundColor Green
Write-Host "  源码目录: $srcDir"
if ($useMirror) {
    Write-Host "  编译镜像: $buildDir"
}
Write-Host "  固件    : $buildDir\build\smoke_gateway.bin"
