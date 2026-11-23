@echo off
chcp 65001 >nul
REM =====================================================================
REM  start_server.bat  -  一键启动后端服务并打开浏览器 UI
REM =====================================================================
cd /d "%~dp0server"
set "PY=C:\Users\Administrator\.workbuddy\binaries\python\versions\3.13.12\python.exe"
if not exist "%PY%" set "PY=python"
echo [CommLink] 启动服务...
start "CommLink Server" "%PY%" app.py
timeout /t 2 >nul
start "" http://127.0.0.1:8765/
echo [CommLink] 已在浏览器打开 http://127.0.0.1:8765/
echo [CommLink] 关闭 "CommLink Server" 窗口即可停止服务。
