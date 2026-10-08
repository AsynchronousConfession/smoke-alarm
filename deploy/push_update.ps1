<#
    一键把本地 server\ 的改动发布到云服务器

    用法（在本工程根目录，或者直接双击也行）：
        powershell -ExecutionPolicy Bypass -File .\deploy\push_update.ps1

    可选参数：
        -Server root@192.0.2.10                 服务器地址（默认就是你的）
        -Key    C:\Users\yourname\.ssh\codex_smoke_deploy   登录用的私钥
        -SkipBackup                                跳过"上传前先在服务器上打备份"

    它会依次做：
        1) 本机打包（复用 pack_for_upload.ps1，自动排除 .venv/日志/数据库）
        2) 在服务器上把当前 /opt/smoke-alarm 打个 tar 备份（保留最近 3 份）
        3) scp 上传新包
        4) 服务器上解压并重跑安装脚本（幂等：只覆盖代码，数据库不动）
        5) 自检：两个服务是否 active、网页是否 200、前端资源版本号

    ★ 改了网页样式/脚本后，记得先把 server\web\index.html 里的 ?v= 数字 +1，
      否则浏览器/Cloudflare 可能还在用旧缓存。
#>

param(
    [string]$Server = 'root@192.0.2.10',
    [string]$Key = "$env:USERPROFILE\.ssh\codex_smoke_deploy",
    [switch]$SkipBackup
)

$ErrorActionPreference = 'Stop'

# ssh/scp 的输出是 UTF-8，而 PowerShell 5.1 默认按 GBK 解码 → 中文会变成乱码。
# 统一切到 UTF-8（对脚本自身生成的中文提示没有影响）。
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
# 注意用不带 BOM 的 UTF-8：带 BOM 的话，管道送过去的脚本第一行会多出几个隐形字节，
# bash 会报 "line 1: #: command not found"
try { $OutputEncoding = New-Object System.Text.UTF8Encoding($false) } catch {}

$projRoot = Split-Path -Parent $PSScriptRoot
$packScript = Join-Path $PSScriptRoot 'pack_for_upload.ps1'
$tarball = Join-Path $PSScriptRoot 'smoke_server_deploy.tar.gz'

function Step($t) { Write-Host "`n>>> $t" -ForegroundColor Cyan }

# 远程执行一段脚本：写成临时 .sh 用管道送过去，
# 这样引号、括号、$() 都不会被 PowerShell / ssh 的参数解析吃掉
function Invoke-RemoteScript {
    param([string]$ScriptBody)
    $tmp = Join-Path $env:TEMP ("remote_" + [guid]::NewGuid().ToString('N') + ".sh")
    $text = ($ScriptBody -replace "`r`n", "`n")
    [System.IO.File]::WriteAllText($tmp, $text, (New-Object System.Text.UTF8Encoding($false)))
    # 把 ssh 的输出直接打印到屏幕，函数只返回退出码（避免调用方被输出干扰）
    Get-Content -LiteralPath $tmp -Raw | & ssh @sshOpts $Server "tr -d '\r' | bash -s" |
        ForEach-Object { Write-Host $_ }
    $code = $LASTEXITCODE
    Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    return $code
}

if (-not (Test-Path -LiteralPath $Key)) {
    throw "找不到私钥 $Key。请确认它还在（这是之前部署时用 ssh-keygen 生成的），或用 -Key 指定别的路径。"
}
if (-not (Test-Path -LiteralPath $packScript)) { throw "找不到 $packScript" }

$sshOpts = @('-i', $Key, '-o', 'IdentitiesOnly=yes', '-o', 'StrictHostKeyChecking=no', '-o', 'ConnectTimeout=15')

# ---------------------------------------------------------------- 1. 打包 ----
Step "1/5 本机打包"
& powershell -ExecutionPolicy Bypass -File $packScript
if ($LASTEXITCODE -ne 0) { throw "打包失败" }

# ---------------------------------------------------------------- 2. 备份 ----
if (-not $SkipBackup) {
    Step "2/5 服务器上打备份（出问题可以回滚）"
    $backupScript = @'
set -e
ts=$(date +%Y%m%d-%H%M%S)
f=/opt/smoke-alarm-backup-$ts.tar.gz
tar -czf "$f" -C /opt/smoke-alarm --exclude=.venv --exclude=data .
ls -t /opt/smoke-alarm-backup-*.tar.gz 2>/dev/null | tail -n +4 | xargs -r rm -f
echo "  备份: $f ($(du -h "$f" | cut -f1))"
'@
    if ((Invoke-RemoteScript $backupScript) -ne 0) {
        Write-Warning "备份失败（不影响发布，可加 -SkipBackup 跳过）"
    }
} else {
    Step "2/5 跳过备份"
}

# ---------------------------------------------------------------- 3. 上传 ----
Step "3/5 上传到服务器"
& scp @sshOpts $tarball "${Server}:/root/smoke_server_deploy.tar.gz"
if ($LASTEXITCODE -ne 0) { throw "上传失败（检查网络 / 私钥 / 服务器是否在跑）" }

# ------------------------------------------------------------ 4. 解压安装 ----
Step "4/5 服务器上安装（只覆盖代码，数据库保留）"
$installCmd = 'set -e; mkdir -p /root/smoke_deploy; ' +
              'tar -xzf /root/smoke_server_deploy.tar.gz -C /root/smoke_deploy; ' +
              'bash /root/smoke_deploy/install_on_server.sh'
& ssh @sshOpts $Server $installCmd
if ($LASTEXITCODE -ne 0) { throw "服务器安装失败（上面有日志；需要回滚见文末提示）" }

# ---------------------------------------------------------------- 5. 自检 ----
Step "5/5 自检"
$checkScript = @'
# 这里故意用英文标签：远程输出经过 ssh + PowerShell 两层编码，
# 中文很容易在不同控制台里显示成乱码，英文最稳。
echo -n "  services   : "
systemctl is-active smoke-mosquitto smoke-backend cloudflared | tr '\n' ' '
echo
echo -n "  web(tunnel): "
curl -sS -o /dev/null -w "%{http_code}\n" --max-time 20 https://smoke.example.com/
echo -n "  web(local) : "
curl -sS -o /dev/null -w "%{http_code}\n" --max-time 10 http://127.0.0.1:3000/
echo -n "  assets     : "
curl -sS --max-time 20 https://smoke.example.com/ | grep -oE '(style.css|app.js)\?v=[0-9]+' | tr '\n' ' '
echo
'@
Invoke-RemoteScript $checkScript | Out-Null

Write-Host ""
Write-Host "发布完成 ✅" -ForegroundColor Green
Write-Host "  * 手机/浏览器如果还是旧样式：刷新一次（或把 index.html 的 ?v= 再 +1 后重新发布）"
Write-Host "  * 需要回滚：ssh 到服务器执行"
Write-Host "      sudo systemctl stop smoke-backend smoke-mosquitto"
Write-Host "      sudo tar -xzf /opt/smoke-alarm-backup-<时间戳>.tar.gz -C /opt/smoke-alarm"
Write-Host "      sudo systemctl start smoke-mosquitto smoke-backend"
