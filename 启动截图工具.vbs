' 无窗口后台启动截图工具（常驻，快捷键 Ctrl+Alt+A）
' 双击本文件即可启动；把它放进「启动」文件夹可实现开机自启
Set sh = CreateObject("WScript.Shell")
scriptDir = CreateObject("Scripting.FileSystemObject").GetParentFolderName(WScript.ScriptFullName)
pyw = "C:\Users\kebia\miniconda3\pythonw.exe"
target = """" & pyw & """ """ & scriptDir & "\screenshot_tool.py"""
sh.Run target, 0, False
