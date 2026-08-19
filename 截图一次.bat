@echo off
rem 立即截图一次然后退出（适合绑定到自己的快捷方式/快捷键）
cd /d "%~dp0"
start "" "C:\Users\kebia\miniconda3\pythonw.exe" "%~dp0screenshot_tool.py" --shot
