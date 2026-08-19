@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo 截图工具已启动（常驻）。按 Ctrl+Alt+A 截图，关闭本窗口即退出。
python screenshot_tool.py
pause
