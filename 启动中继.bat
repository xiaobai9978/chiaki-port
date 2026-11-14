@echo off
chcp 65001 >nul
title Chiaki 中继（掌机 -> PS5）
cd /d "%~dp0"

set PY=C:\Users\jhgroup\.workbuddy\binaries\python\versions\3.13.12\python.exe
if not exist "%PY%" set PY=python

echo ============================================
echo   Chiaki 中继服务
echo   掌机(办公室) --TCP/UDP--> 本机 --VPN--> PS5
echo   PS5: 192.168.50.116   本机: 10.1.5.137
echo   保持本窗口开着，电脑不能睡眠，VPN 不能断
echo ============================================
echo.

"%PY%" relay.py --ps5 192.168.50.116 --port 9295 --ports 9295,9296,9297

echo.
echo 中继已退出。
pause
