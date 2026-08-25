@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo === 打包 SnapMark 免安装版 ===
echo.
where pyinstaller >nul 2>nul || (
  echo 未检测到 PyInstaller，正在安装...
  pip install pyinstaller || goto :err
)

rem 排除 numpy/MKL 等重型依赖（代码会自动降级），体积从 ~500MB 降到 ~60MB
pyinstaller --noconfirm --windowed --clean --name SnapMark ^
  --add-data "fonts\SourceHanSerifCN-Regular.ttf;fonts" ^
  --add-data "fonts\ZhanKuKuaiLeTi.ttf;fonts" ^
  --add-data "icon;icon" ^
  --exclude-module numpy --exclude-module scipy --exclude-module matplotlib ^
  --exclude-module pandas --exclude-module numexpr --exclude-module mkl ^
  --exclude-module PyQt5 --exclude-module PySide2 --exclude-module IPython ^
  screenshot_tool.py || goto :err

copy /Y config.ini "dist\SnapMark\config.ini" >nul

echo.
echo 完成！便携版位于  dist\SnapMark\
echo   - 双击 SnapMark.exe 即用（无需安装 Python）
echo   - 编辑同目录 config.ini 修改唤起快捷键
echo   - 整个 dist\SnapMark 文件夹压缩后即可分发
pause
exit /b 0

:err
echo.
echo 打包失败，请检查上面的错误信息。
pause
exit /b 1
