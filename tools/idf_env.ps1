<#
    ESP-IDF 环境激活脚本（本工程专用）

    用法（在本工程根目录）:
        . .\tools\idf_env.ps1                    # 只激活环境
        . .\tools\idf_env.ps1 gateway_esp32s3    # 激活并进入网关工程目录

    它会自动寻找 ESP-IDF 安装目录（要先装好 ESP-IDF v5.1.x），并设置
    IDF_PATH / IDF_TOOLS_PATH / IDF_TARGET=esp32s3。
#>

$ErrorActionPreference = 'Stop'

function Get-IdfPath {
    $cands = @()
    if ($env:IDF_PATH) { $cands += $env:IDF_PATH }
    $cands += @(
        'D:\Espressif\frameworks\esp-idf-v5.1.2',
        'C:\Espressif\frameworks\esp-idf-v5.1.2',
        (Join-Path $env:USERPROFILE 'esp\esp-idf'),
        (Join-Path $env:USERPROFILE 'esp\v5.1.2\esp-idf'),
        'C:\esp\esp-idf'
    )
    foreach ($c in $cands) {
        if ($c -and (Test-Path -LiteralPath (Join-Path $c 'export.ps1') -ErrorAction SilentlyContinue)) {
            return (Resolve-Path -LiteralPath $c).Path
        }
    }
    foreach ($base in @('D:\Espressif\frameworks', 'C:\Espressif\frameworks', (Join-Path $env:USERPROFILE 'esp'))) {
        if (Test-Path -LiteralPath $base) {
            $hit = Get-ChildItem -LiteralPath $base -Directory -ErrorAction SilentlyContinue |
                   Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'export.ps1') } |
                   Select-Object -First 1
            if ($hit) { return $hit.FullName }
        }
    }
    return $null
}

function Get-ToolsPath($idfPath) {
    $cands = @()
    if ($env:IDF_TOOLS_PATH) { $cands += $env:IDF_TOOLS_PATH }
    $cands += (Split-Path (Split-Path $idfPath -Parent) -Parent)
    $cands += @((Join-Path $env:USERPROFILE '.espressif'), 'D:\Espressif', 'C:\Espressif')
    foreach ($c in $cands) {
        if ($c -and (Test-Path -LiteralPath (Join-Path $c 'tools') -ErrorAction SilentlyContinue)) { return $c }
    }
    return $null
}

function Get-PyEnvPath($toolsPath) {
    if (-not $toolsPath) { return $null }
    $pe = Join-Path $toolsPath 'python_env'
    if (-not (Test-Path -LiteralPath $pe)) { return $null }
    $envs = Get-ChildItem -LiteralPath $pe -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -like 'idf*_py3*_env' -and
                           (Test-Path -LiteralPath (Join-Path $_.FullName 'Scripts\python.exe')) }
    if ($envs) { return $envs[0].FullName }
    return $null
}

$idfPath = Get-IdfPath
if (-not $idfPath) {
    Write-Host "===============================================" -ForegroundColor Yellow
    Write-Host " 没有找到 ESP-IDF" -ForegroundColor Yellow
    Write-Host "===============================================" -ForegroundColor Yellow
    Write-Host " 请先安装 ESP-IDF v5.1.x ："
    Write-Host "   1) VS Code 装 Espressif IDF 扩展，按向导安装（推荐，安装路径填 D:\Espressif）"
    Write-Host "   2) 官方离线安装器： https://dl.espressif.com/dl/esp-idf/"
    Write-Host ""
    Write-Host " 装好后重新运行本脚本即可（脚本会自动找到它）。"
    return
}

$toolsPath = Get-ToolsPath $idfPath
$pyEnvPath = Get-PyEnvPath $toolsPath

$env:IDF_PATH = $idfPath
if ($toolsPath) { $env:IDF_TOOLS_PATH = $toolsPath }
if ($pyEnvPath) { $env:IDF_PYTHON_ENV_PATH = $pyEnvPath }
$env:IDF_TARGET = 'esp32s3'

# 【很重要】本工程目录名里有中文。ESP-IDF 的 Python 脚本在 Windows 上默认按
# 系统区域编码(GBK)读写它自己生成的 JSON，遇到中文路径会报
#     'gbk' codec can't decode byte ... : illegal multibyte sequence
# 打开 Python 的 UTF-8 模式后一切正常（cmake/ninja 输出也统一成 UTF-8）。
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'

if (-not $pyEnvPath) {
    Write-Host "提示：没有找到 IDF 的 Python 虚拟环境（python_env\idf*_py3*_env）。" -ForegroundColor Yellow
    Write-Host "      如果下面报 'Cannot import module yaml'，请在 IDF 目录下执行 install.ps1 再试。" -ForegroundColor Yellow
}

Write-Host "ESP-IDF : $idfPath"
if ($toolsPath) { Write-Host "工具链  : $toolsPath" }
if ($pyEnvPath) { Write-Host "Python  : $pyEnvPath" }

. "$idfPath\export.ps1"

if ($args.Count -gt 0) {
    $projRoot = Split-Path -Parent $PSScriptRoot          # 工程根目录（tools 的上一级）
    $target = Join-Path $projRoot $args[0]
    if (-not (Test-Path -LiteralPath $target)) { $target = Join-Path $projRoot 'gateway_esp32s3' }
    if (Test-Path -LiteralPath $target) {
        Set-Location $target
        Write-Host ">>> 当前目录: $target" -ForegroundColor Green
    } else {
        Write-Warning "目录不存在: $($args[0])"
    }
}
