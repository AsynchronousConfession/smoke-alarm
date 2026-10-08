<#
    把 server 目录打成可以上传到云服务器的部署包（tar.gz）

    用法（在本工程根目录）:
        powershell -ExecutionPolicy Bypass -File .\deploy\pack_for_upload.ps1

    产物: deploy\smoke_server_deploy.tar.gz

    会自动排除本机专用的东西：
        .venv\            本机 Python 虚拟环境（云上重新建）
        logs\  data\      本机日志与数据库（云上从空库开始）
        __pycache__\
        *.exe             Windows 专用的 cloudflared.exe / mosquitto 安装包
        mosquitto.conf / passwd / acl / runtime.json   本机路径相关，云上重新生成
#>

param(
    [string]$Out = (Join-Path $PSScriptRoot 'smoke_server_deploy.tar.gz')
)

$ErrorActionPreference = 'Stop'

$projRoot = Split-Path -Parent $PSScriptRoot
$src = Join-Path $projRoot 'server'

if (-not (Test-Path -LiteralPath $src)) { throw "找不到 server 目录: $src" }
if (-not (Get-Command tar -ErrorAction SilentlyContinue)) { throw "系统没有 tar 命令（Windows 10 1803+ 自带）" }

$staging = Join-Path $env:TEMP ("smoke_deploy_" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $staging | Out-Null

Write-Host "整理文件 -> $staging" -ForegroundColor Cyan
$robolog = & robocopy $src $staging /E `
    /XD '.venv' 'logs' 'data' '__pycache__' '.run' `
    /XF '*.exe' 'mosquitto.conf' 'passwd' 'acl' 'runtime.json' `
    /NFL /NDL /NJH /NJS /NP 2>&1
if ($LASTEXITCODE -ge 8) {
    $robolog | ForEach-Object { Write-Host $_ -ForegroundColor Red }
    throw "robocopy 失败 (exit=$LASTEXITCODE)"
}

# 公开仓库里的 server\config.json 是"占位符版本"（口令/域名都替换过），
# 真实配置放在 server\config.local.json（已被 .gitignore 排除）。
# 只要存在 config.local.json，就打它进包，避免把占位符口令部署到线上导致 MQTT 认证失败。
$realCfg = Join-Path $src 'config.local.json'
if (Test-Path -LiteralPath $realCfg) {
    Copy-Item -LiteralPath $realCfg -Destination (Join-Path $staging 'config.json') -Force
    Write-Host '  已用 config.local.json 覆盖包内的占位符配置' -ForegroundColor Yellow
} else {
    Write-Host '  [注意] 没有 config.local.json，包内用的是占位符配置（线上 MQTT 会认证失败）' -ForegroundColor Yellow
}

# 部署脚本本身在 deploy\ 目录里（不在 server\ 里），必须一起放进包，
# 否则服务器上解压后找不到 install_on_server.sh
# （部署手册 .md 文件名带中文，跨平台解压可能出现乱码，就不放进包里了，在本机看即可）
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'install_on_server.sh') -Destination $staging -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'server_selftest.sh')   -Destination $staging -Force

if (Test-Path -LiteralPath $Out) { Remove-Item -LiteralPath $Out -Force }
tar -czf $Out -C $staging .
if ($LASTEXITCODE -ne 0) { throw "tar 打包失败" }

Remove-Item -LiteralPath $staging -Recurse -Force

# 自检：确保关键文件真的进包了（之前就是这里漏了安装脚本，导致服务器上找不到）
$list = tar -tzf $Out
$must = @('./install_on_server.sh', './server_selftest.sh', './config.json', './backend/app.py',
          './web/app.js', './web/vendor/echarts.min.js')
foreach ($m in $must) {
    if (-not ($list -contains $m)) { throw "打包异常：$m 不在包里，请检查 pack_for_upload.ps1" }
}

$size = (Get-Item -LiteralPath $Out).Length
Write-Host ""
Write-Host ("部署包已生成: " + $Out) -ForegroundColor Green
Write-Host ("大小: {0:N0} KB" -f ($size / 1KB))
Write-Host ("包含 {0} 个条目（含 install_on_server.sh）" -f ($list | Measure-Object).Count)
Write-Host ""
Write-Host "下一步（在你自己的电脑上执行，把 IP 换成服务器公网 IP）:" -ForegroundColor Yellow
Write-Host ("  scp `"{0}`" root@<服务器公网IP>:/root/" -f $Out)
Write-Host "然后在服务器上执行:"
Write-Host "  mkdir -p /root/smoke_deploy && tar -xzf /root/smoke_server_deploy.tar.gz -C /root/smoke_deploy"
Write-Host "  sudo bash /root/smoke_deploy/install_on_server.sh"
