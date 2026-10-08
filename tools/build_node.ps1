<#
    命令行编译 CH573F 烟雾节点固件（不需要打开 MounRiver Studio）

    用法（在本目录或任意位置均可）:
        powershell -ExecutionPolicy Bypass -File .\tools\build_node.ps1
        powershell -ExecutionPolicy Bypass -File .\tools\build_node.ps1 -Clean
        powershell -ExecutionPolicy Bypass -File .\tools\build_node.ps1 -Verify

    一次出三块板子的固件（不改任何源码，-Tag/-Model 覆盖 node_cfg.h 的默认值）:
        powershell -ExecutionPolicy Bypass -File .\tools\build_node.ps1 -Tag N01 -Model "MQ-135" -Out .\deploy\fw\N01_MQ-135.hex
        powershell -ExecutionPolicy Bypass -File .\tools\build_node.ps1 -Tag N02 -Model "MQ-137" -Out .\deploy\fw\N02_MQ-137.hex
        powershell -ExecutionPolicy Bypass -File .\tools\build_node.ps1 -Tag N03 -Model "MQ-2"   -Out .\deploy\fw\N03_MQ-2.hex

    产物:
        node_ch573f\build\SmokeNode.hex   ← 用 WCHISPTool / WCH-LinkE 下载
        node_ch573f\build\SmokeNode.elf
        node_ch573f\build\SmokeNode.map

    目录结构（与沁恒 EVT 例程一致，不能随意挪动）:
        node_ch573f\node\SmokeNode\      ← MounRiver 工程 (APP / Profile)
        node_ch573f\node\HAL             ← 公共 HAL        (PARENT-1-PROJECT_LOC/HAL)
        node_ch573f\node\LIB             ← BLE 协议栈库     (PARENT-1-PROJECT_LOC/LIB)
        node_ch573f\SRC                  ← Ld/RVMSIS/Startup/StdPeriphDriver
                                            (PARENT-2-PROJECT_LOC/SRC)

    说明: 本机实时防护软件偶发会改写刚生成的 .o / .elf，表现为 ld 报
          "file format not recognized" / "invalid string offset" / 莫名的
          relocation 错误——与源码无关。本脚本会校验每个 .o，失败自动重来。
#>

param(
    [switch]$Clean,
    # 连续两轮编译产物完全一致才认为可信（对抗实时防护导致的文件损坏）
    [switch]$Verify,
    # 工程名（默认 SmokeNode），也可以传工程目录的绝对路径
    [string]$Project = 'SmokeNode',
    # MounRiver 工具链位置（一般不用改，脚本会自动探测）
    [string]$MrsRoot = '',
    # 节点编号 / 传感器型号：覆盖 node_cfg.h 里的 NODE_TAG / NODE_MODEL，
    # 不改源码就能给多块板子出多份固件（例：-Tag N02 -Model "MQ-137"）
    [string]$Tag = '',
    [string]$Model = '',
    # 额外把生成的 hex 复制到指定文件（相对路径按项目根目录解析）
    [string]$Out = '',
    # 追加任意 -D 宏，用来覆盖 node_cfg.h 里带 #ifndef 保护的默认值。
    # 例（模块接 5V + 10k/10k 分压）：-Define NODE_MODULE_VCC_MV=5000,NODE_AO_DIV_X100=200
    [string[]]$Define = @(),
    # MQ 模块 AO 接在哪个引脚（默认按 node_cfg.h 的 PA4）。脚本会自动带出对应的 ADC 通道号，
    # 因为 CH573 的 ADC 通道和引脚是固定的：可选 PA4/PA5/PA8/PA9/PA12/PA13/PA14/PA15
    [string]$AoPin = ''
)

# 注意: 工具链是原生 exe, 它们往 stderr 输出告警时 PowerShell 会包装成
# ErrorRecord; 若这里是 Stop 就会误判成致命错误。统一用 Continue,
# 真正的失败一律靠 $LASTEXITCODE 判断。
$ErrorActionPreference = 'Continue'

# ---- 定位工程 ----------------------------------------------------------------
$projRoot = Split-Path -Parent $PSScriptRoot          # 04_毕设项目_家庭室内多节点烟雾报警系统
$nodeRoot = Join-Path $projRoot 'node_ch573f'
$halDir   = Join-Path $nodeRoot 'node\HAL'
$libDir   = Join-Path $nodeRoot 'node\LIB'
$srcRoot  = Join-Path $nodeRoot 'SRC'

if (Test-Path -LiteralPath $Project) {
    $projDir = (Resolve-Path -LiteralPath $Project).Path
} else {
    $projDir = Join-Path $nodeRoot "node\$Project"
}
if (-not (Test-Path -LiteralPath (Join-Path $projDir 'APP'))) {
    throw "不是有效的 MounRiver 工程目录: $projDir"
}
$projName = Split-Path $projDir -Leaf
$buildDir = Join-Path $nodeRoot 'build'

if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
    Get-ChildItem -LiteralPath $buildDir -File | Remove-Item -Force
}
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

# ---- 定位工具链 --------------------------------------------------------------
function Get-ToolchainRoot {
    param([string]$Hint)
    $cands = @()
    if ($Hint) { $cands += $Hint }
    $cands += @(
        'D:\MounRiver\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC\bin',
        'C:\MounRiver\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC\bin'
    )
    foreach ($base in @('D:\MounRiver', 'C:\MounRiver')) {
        if (Test-Path -LiteralPath $base) {
            $hit = Get-ChildItem -LiteralPath $base -Recurse -Filter 'riscv-none-embed-gcc.exe' -ErrorAction SilentlyContinue |
                   Select-Object -First 1
            if ($hit) { $cands += $hit.DirectoryName }
        }
    }
    foreach ($c in $cands) {
        if ($c -and (Test-Path -LiteralPath (Join-Path $c 'riscv-none-embed-gcc.exe'))) { return $c }
    }
    return $null
}

$tc = Get-ToolchainRoot -Hint $MrsRoot
if (-not $tc) {
    throw "找不到 MounRiver 的 RISC-V 工具链(riscv-none-embed-gcc.exe)。请安装 MounRiver Studio 2，或用 -MrsRoot 指定 bin 目录。"
}
$gcc     = Join-Path $tc 'riscv-none-embed-gcc.exe'
$objcopy = Join-Path $tc 'riscv-none-embed-objcopy.exe'
$size    = Join-Path $tc 'riscv-none-embed-size.exe'
$objdump = Join-Path $tc 'riscv-none-embed-objdump.exe'

Write-Host "工具链 : $tc" -ForegroundColor DarkGray
Write-Host "工程   : $projDir" -ForegroundColor DarkGray
Write-Host "输出   : $buildDir" -ForegroundColor DarkGray
Write-Host ""

# ---- 源文件 / 头文件 / 宏 ----------------------------------------------------
$stdDir = Join-Path $srcRoot 'StdPeriphDriver'

# 工程自身的源文件: 编译 APP 与 Profile 下的全部 .c
# （APP 下不再有 Driver 子目录：本工程的节点只有 MCU + MQ 传感器 + 蜂鸣器，
#   没有 OLED、没有按键，所以也不需要那两套驱动）
$cSources = @()
foreach ($dir in @('APP', 'Profile')) {
    $full = Join-Path $projDir $dir
    if (Test-Path -LiteralPath $full) {
        $cSources += (Get-ChildItem -LiteralPath $full -Filter *.c -File |
                      Sort-Object Name | ForEach-Object { $_.FullName })
    }
}
$cSources += @(
    (Join-Path $halDir  'MCU.c'),
    (Join-Path $halDir  'RTC.c'),
    (Join-Path $halDir  'SLEEP.c'),
    (Join-Path $srcRoot 'RVMSIS\core_riscv.c'),
    # StdPeriphDriver: 与原工程剔除列表保持一致
    # (不含 usb / pwm / spi0 / timer1-3 / uart1,2,3；本节点调试串口用 UART0)
    (Join-Path $stdDir 'CH57x_adc.c'),
    (Join-Path $stdDir 'CH57x_clk.c'),
    (Join-Path $stdDir 'CH57x_flash.c'),
    (Join-Path $stdDir 'CH57x_gpio.c'),
    (Join-Path $stdDir 'CH57x_pwr.c'),
    (Join-Path $stdDir 'CH57x_sys.c'),
    (Join-Path $stdDir 'CH57x_timer0.c'),
    (Join-Path $stdDir 'CH57x_uart0.c')
)
$asmSources = @( (Join-Path $srcRoot 'Startup\startup_CH573.S') )

$includeDirs = @(
    (Join-Path $projDir 'APP'),
    (Join-Path $projDir 'APP\include'),
    (Join-Path $projDir 'Profile\include'),
    (Join-Path $halDir  'include'),
    $libDir,
    (Join-Path $srcRoot 'StdPeriphDriver\inc'),
    (Join-Path $srcRoot 'RVMSIS'),
    (Join-Path $srcRoot 'Startup'),
    (Join-Path $srcRoot 'Ld')
)

$commonFlags = @(
    '-march=rv32imac', '-mabi=ilp32', '-mcmodel=medany',
    '-msmall-data-limit=8', '-mno-save-restore',
    '-fmax-errors=20', '-Os', '-fmessage-length=0', '-fsigned-char',
    '-ffunction-sections', '-fdata-sections', '-fno-common', '-g'
)
# DEBUG=0 -> 调试串口用 UART0(PB4/PB7)，与 CH57x_sys.c 里的 _write 定义对应
$cFlags   = $commonFlags + @('-DDEBUG=0', '-std=gnu99')
$cFlags  += ($includeDirs | ForEach-Object { "-I$_" })

# 节点身份：命令行覆盖 node_cfg.h 的默认值。
# 注意 Windows PowerShell 5.1 调原生 exe 时会吃掉参数里的双引号，所以这里写成 \" 转义形式
# （gcc 的运行时再把 \" 还原成 " ），否则 -DNODE_TAG=N01 会被当成未声明的标识符。
$q = [string][char]92 + [char]34
if ($Tag)   { $cFlags += "-DNODE_TAG=$q$Tag$q" }
if ($Model) { $cFlags += "-DNODE_MODEL=$q$Model$q" }
foreach ($d in $Define) { if ($d) { $cFlags += "-D$d" } }

# AO 引脚 -> (ADC 通道号, GPIO 位)，映射见 CH573SFR.h 的 bAINx
if ($AoPin) {
    $ao = $AoPin.ToUpper()
    $aomap = @{
        'PA4'  = 'CH_EXTIN_0,GPIO_Pin_4'
        'PA5'  = 'CH_EXTIN_1,GPIO_Pin_5'
        'PA12' = 'CH_EXTIN_2,GPIO_Pin_12'
        'PA13' = 'CH_EXTIN_3,GPIO_Pin_13'
        'PA14' = 'CH_EXTIN_4,GPIO_Pin_14'
        'PA15' = 'CH_EXTIN_5,GPIO_Pin_15'
        'PA8'  = 'CH_EXTIN_12,GPIO_Pin_8'
        'PA9'  = 'CH_EXTIN_13,GPIO_Pin_9'
    }
    if (-not $aomap.ContainsKey($ao)) {
        throw "不认识的 AO 引脚: $AoPin（可选 PA4 / PA5 / PA8 / PA9 / PA12 / PA13 / PA14 / PA15）"
    }
    $ap = $aomap[$ao].Split(',')
    $cFlags += "-DNODE_AO_CH=$($ap[0])"
    $cFlags += "-DNODE_AO_PIN=$($ap[1])"
    $cFlags += "-DNODE_AO_PIN_NAME=$q$ao$q"
}

$ldFlags = @(
    '-march=rv32imac', '-mabi=ilp32', '-mcmodel=medany',
    '-msmall-data-limit=8', '-mno-save-restore',
    '-fmax-errors=20', '-Os', '-fmessage-length=0', '-fsigned-char',
    '-ffunction-sections', '-fdata-sections', '-fno-common', '-g',
    "-T", (Join-Path $srcRoot 'Ld\Link.ld'),
    '-nostartfiles', '-Xlinker', '--gc-sections',
    "-L$libDir", "-L$stdDir",
    '-Xlinker', '--print-memory-usage',
    "-Wl,-Map,$buildDir\$projName.map",
    '--specs=nano.specs', '--specs=nosys.specs'
)

$elf     = Join-Path $buildDir "$projName.elf"
$hexFile = Join-Path $buildDir "$projName.hex"

# ---- 编译 / 链接（带损坏目标文件的检测与重试） --------------------------------
$corruptPattern = 'file format not recognized|invalid string offset|file truncated|Malformed|symbol needs debug section|dangerous relocation|relocation truncated to fit|access beyond end of merged section \([0-9]{10,}'
$realErrorPattern = 'undefined reference to|multiple definition of|cannot find -l|No such file or directory|will not fit in region|overflowed by'

function Test-ObjectFile {
    param([string]$ObjFile)
    $out = & $objdump -t $ObjFile 2>&1
    return -not [bool]($out | Select-String -Pattern 'invalid string offset|file format not recognized|file truncated|Malformed')
}

function Invoke-CompileAll {
    $objs   = @()
    $failed = $false

    foreach ($src in ($cSources + $asmSources)) {
        if (-not (Test-Path -LiteralPath $src)) {
            Write-Host "缺少源文件: $src" -ForegroundColor Red
            $failed = $true
            continue
        }
        $isAsm  = ($src -like '*.S')
        $flags  = if ($isAsm) { $commonFlags } else { $cFlags }
        $tag    = if ($isAsm) { 'AS' } else { 'CC' }
        $rel    = [System.IO.Path]::GetFileNameWithoutExtension($src)
        $obj    = Join-Path $buildDir "$rel.o"

        Write-Host ("[{0}] {1}" -f $tag, (Split-Path $src -Leaf)) -ForegroundColor DarkGray

        for ($try = 1; $try -le 3; $try++) {
            $out = & $gcc @flags -c $src -o $obj 2>&1
            if ($LASTEXITCODE -ne 0) {
                Write-Host "编译失败: $src" -ForegroundColor Red
                $out | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
                $failed = $true
                break
            }
            if ($out) { $out | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow } }
            if (Test-ObjectFile $obj) { break }
            Write-Host "    目标文件被外部程序改写, 重新编译 (第 $try 次)" -ForegroundColor Yellow
            Start-Sleep -Milliseconds 400
        }
        $objs += $obj
    }

    return @{ Objects = $objs; Failed = $failed }
}

$maxRounds = 8
$linkOut   = $null
$ok        = $false
$prevSig   = $null

for ($round = 1; $round -le $maxRounds; $round++) {
    if ($round -gt 1) {
        Write-Host "重新编译 (第 $round 轮)" -ForegroundColor Yellow
        Start-Sleep -Milliseconds 600
    }

    $result = Invoke-CompileAll
    if ($result.Failed) { throw "有源文件编译失败" }
    $objects = $result.Objects

    Write-Host "[LD] $projName.elf" -ForegroundColor DarkGray
    $linkOut = & $gcc @ldFlags -o $elf @objects -lISP573 -lCH57xBLE -lm 2>&1

    if ($LASTEXITCODE -eq 0) {
        if (Test-ObjectFile $elf) {
            & $objcopy -O ihex $elf $hexFile | Out-Null
            $hexLines = Get-Content -LiteralPath $hexFile -ErrorAction SilentlyContinue
            $hexOk = $hexLines -and $hexLines.Count -ge 10 -and
                     ($hexLines | Where-Object { $_ -notmatch '^:' }).Count -eq 0
            if ($hexOk) {
                if (-not $Verify) { $ok = $true; break }
                $sig = (($objects | Sort-Object | ForEach-Object {
                            "$(Split-Path $_ -Leaf)=$((Get-FileHash $_ -Algorithm MD5).Hash)"
                        }) -join ';') + "|hex=$((Get-FileHash $hexFile -Algorithm MD5).Hash)"
                if ($null -ne $prevSig -and $sig -eq $prevSig) {
                    Write-Host "两轮编译产物完全一致, 结果可信" -ForegroundColor Green
                    $ok = $true; break
                }
                $prevSig = $sig
                Write-Host "本轮产物已生成, 再编译一轮做一致性比对…" -ForegroundColor DarkGray
                continue
            }
            Write-Host "    hex 文件异常, 重新编译后再试" -ForegroundColor Yellow
            continue
        }
        Write-Host "    ELF 被外部程序改写, 重新编译后再试" -ForegroundColor Yellow
        continue
    }

    $text = ($linkOut | Out-String)
    if ((-not ($text -match $realErrorPattern)) -or ($text -match $corruptPattern)) { continue }
    break
}

if (-not $ok) {
    Write-Host "链接失败:" -ForegroundColor Red
    $linkOut | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
    throw "链接失败"
}

$linkOut | Where-Object { $_ -notmatch 'access beyond end of merged section' } |
    ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }

Write-Host ""
Write-Host "编译成功: $hexFile" -ForegroundColor Green
& $size $elf

if ($Tag -or $Model) {
    Write-Host ("身份   : NODE_TAG={0}  NODE_MODEL={1}" -f
        $(if ($Tag) { $Tag } else { '(node_cfg.h 默认)' }),
        $(if ($Model) { $Model } else { '(node_cfg.h 默认)' })) -ForegroundColor Cyan
}

if ($AoPin) {
    Write-Host "AO 引脚: $($AoPin.ToUpper())（连同对应 ADC 通道一起编进固件）" -ForegroundColor Cyan
}

if ($Out) {
    $outPath = $Out
    if (-not [System.IO.Path]::IsPathRooted($outPath)) { $outPath = Join-Path $projRoot $outPath }
    $outDir = Split-Path -Parent $outPath
    if ($outDir) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }
    Copy-Item -LiteralPath $hexFile -Destination $outPath -Force
    Write-Host "已另存固件: $outPath" -ForegroundColor Green
}
