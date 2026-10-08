# 用法（在 PowerShell 里）：
#   . .\idf-env.ps1          # 之后就能直接用 idf.py / esptool / idf.py monitor

$env:IDF_PATH            = 'D:\Espressif\frameworks\esp-idf-v5.1.2'
$env:IDF_TOOLS_PATH      = 'C:\Users\yourname\.espressif'
$env:IDF_PYTHON_ENV_PATH = 'C:\Users\yourname\.espressif\python_env\idf5.1_py3.11_env'

$extraTools = @(
    "$env:IDF_TOOLS_PATH\tools\xtensa-esp32-elf\esp-12.2.0_20230208\xtensa-esp32-elf\bin",
    "$env:IDF_TOOLS_PATH\tools\xtensa-esp32s2-elf\esp-12.2.0_20230208\xtensa-esp32s2-elf\bin",
    "$env:IDF_TOOLS_PATH\tools\xtensa-esp32s3-elf\esp-12.2.0_20230208\xtensa-esp32s3-elf\bin",
    "$env:IDF_TOOLS_PATH\tools\riscv32-esp-elf\esp-12.2.0_20230208\riscv32-esp-elf\bin",
    "$env:IDF_TOOLS_PATH\tools\esp32ulp-elf\2.35_20220830\esp32ulp-elf\bin",
    "$env:IDF_TOOLS_PATH\tools\cmake\3.24.0\bin",
    "$env:IDF_TOOLS_PATH\tools\ninja\1.10.2",
    "$env:IDF_TOOLS_PATH\tools\ccache\4.8\ccache-4.8-windows-x86_64",
    "$env:IDF_TOOLS_PATH\tools\dfu-util\0.11\dfu-util-0.11-win64",
    "$env:IDF_TOOLS_PATH\tools\openocd-esp32\v0.12.0-esp32-20230921\openocd-esp32\bin"
)

$env:ESP_ROM_ELF_DIR   = "$env:IDF_TOOLS_PATH\tools\esp-rom-elfs\20230320\"
$env:OPENOCD_SCRIPTS   = "$env:IDF_TOOLS_PATH\tools\openocd-esp32\v0.12.0-esp32-20230921\openocd-esp32\share\openocd\scripts"
$env:IDF_CCACHE_ENABLE = "1"

$env:PATH = (($extraTools + "$env:IDF_PYTHON_ENV_PATH\Scripts") -join ';') + ';' + $env:IDF_PATH + '\tools;' + $env:PATH

function idf.py { & "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe" "$env:IDF_PATH\tools\idf.py" @args }
function esptool.py { & "$env:IDF_PYTHON_ENV_PATH\Scripts\esptool.exe" @args }

Write-Host "ESP-IDF environment ready:" -ForegroundColor Green
idf.py --version
