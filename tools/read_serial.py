#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""读串口日志（用于烧写后/排查问题时看网关输出）

用法（用 ESP-IDF 自带的 Python，它带 pyserial）：
    D:\Espressif\python_env\idf5.1_py3.11_env\Scripts\python.exe .\tools\read_serial.py COM6 15
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 15

s = serial.Serial(port, 115200, timeout=1)
end = time.time() + secs
try:
    while time.time() < end:
        data = s.readline()
        if data:
            print(data.decode("utf-8", "replace").rstrip())
finally:
    s.close()
