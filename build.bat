@echo off
rem 编译 TatoMark 的 Direct2D 原生(GPU 合成)版
rem 需要现代 g++（conda 自带的 mingw 5.3 太老，会在 d2d1.h 上崩溃）。
rem 推荐 w64devkit（便携版 GCC，已解压到 C:\Users\kebia\tools\w64devkit）。
setlocal
set W64=C:\Users\kebia\tools\w64devkit\bin
rem 关键：把 w64devkit 的 bin 放到 PATH 最前，确保用它自己的 ld 链接器；
rem 否则若系统里有别的旧 mingw ld，会链接出无法运行的 exe（WinError 193）。
set PATH=%W64%;%PATH%
rem 先编译清单资源(声明 Per-Monitor-V2 DPI 感知，避免高分屏被系统放大)
windres app.rc -O coff -o app_res.o
g++ -std=c++17 -O2 main.cpp app_res.o -o TatoMarkD2D.exe -mwindows -static ^
  -ld2d1 -ldwrite -lwindowscodecs -ldwmapi -lcomdlg32 -lgdi32 -luser32 -lole32 -luuid
if %errorlevel%==0 (echo 编译成功: TatoMarkD2D.exe) else (echo 编译失败)
endlocal
