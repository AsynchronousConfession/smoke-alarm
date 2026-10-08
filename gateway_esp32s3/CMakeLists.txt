# ESP32-S3 烟雾报警网关 (BLE Central) —— 自动连接多台 CH573F 烟雾节点并上云
# 说明: 本目录名里有中文, ESP-IDF 的 ninja 不支持中文路径,
#       请用 tools\build_gateway.ps1 编译（它会自动同步到英文镜像目录再编译）。
cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# 中文路径保护（提前给子进程打开 Python 的 UTF-8 模式）
#   ESP-IDF 的 kconfig 工具是 Python 写的，在中文 Windows 上默认按 GBK 读写它
#   自己生成的 build/config.env、project_description.json 等文件，路径里有中文时
#   会报：
#       UnicodeDecodeError: 'gbk' codec can't decode byte 0xae ...
#       Failed to run kconfgen
#   这里在 cmake 里先把 PYTHONUTF8 设进环境，之后 cmake 启动的所有 Python
#   子进程（kconfgen / confgen / esptool ...）都会按 UTF-8 处理路径。
#   注意：这只解决"编码"问题；ninja 在 Windows 上用的是 ANSI 文件 API，
#   中文路径仍会失败，所以正常编译还是请用 tools\build_gateway.ps1（英文镜像目录）。
# ---------------------------------------------------------------------------
set(ENV{PYTHONUTF8} 1)
set(ENV{PYTHONIOENCODING} utf-8)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(smoke_gateway)
