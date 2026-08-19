' 一键干净重启：结束所有旧实例，再启动一个干净的常驻进程（无控制台窗口）
Set sh = CreateObject("WScript.Shell")
scriptDir = CreateObject("Scripting.FileSystemObject").GetParentFolderName(WScript.ScriptFullName)
pyw = "C:\Users\kebia\miniconda3\pythonw.exe"
' 结束旧的 pythonw（隐藏窗口，等待完成）
sh.Run "taskkill /F /IM pythonw.exe", 0, True
WScript.Sleep 600
' 启动新的常驻实例
sh.Run """" & pyw & """ """ & scriptDir & "\screenshot_tool.py""", 0, False
