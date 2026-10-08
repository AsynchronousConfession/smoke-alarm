# 烟雾报警网关 —— ESP32-2432S028R (CYD) 移植版
# 原工程: D:\smoke-alarm\04\gateway_esp32s3 (ESP32-S3)
# 本工程: 换成经典 ESP32 (CYD)，先跑通 BLE + WiFi + MQTT，再接屏幕
cmake_minimum_required(VERSION 3.16)

# 中文路径保护（本工程路径是纯英文，保留这段只是以防以后挪到中文目录）
set(ENV{PYTHONUTF8} 1)
set(ENV{PYTHONIOENCODING} utf-8)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cyd_gateway)
