<div align="center">

# 📸 SnapMark

**一款轻量、顺滑、颜值在线的 Windows 截图标注小工具**

按一个快捷键，框选任意区域，即刻标注 —— 矩形、椭圆、彗星箭头、画笔、马赛克、文字，一键复制或保存。

[![Platform](https://img.shields.io/badge/platform-Windows-0078D6?logo=windows&logoColor=white)](#)
[![Python](https://img.shields.io/badge/Python-3.9%2B-3776AB?logo=python&logoColor=white)](#)
[![Dependencies](https://img.shields.io/badge/依赖-仅%20Pillow-brightgreen)](#)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![Style](https://img.shields.io/badge/UI-Snipaste%20风格-13C060)](#)

<img src="assets/hero.png" width="820" alt="SnapMark 界面预览">

</div>

---

## ✨ 为什么选择 SnapMark

- 🪶 **极轻量**：纯 Python 实现，除 [Pillow](https://python-pillow.org/) 外**零第三方依赖**，`tkinter` 用系统自带。
- ⚡ **调用顺滑**：常驻后台（无控制台黑框），一个冷门全局快捷键即按即弹，几乎零延迟。
- 🎨 **抗锯齿渲染**：图标与标注全部用 4× 超采样 + LANCZOS 抗锯齿绘制，**所见即所得**，导出无锯齿。
- ☄️ **彗星箭头**：独特的扫尾箭头，尾部收成尖点、颜色由头到尾渐变，比普通箭头更有质感。
- 🖐️ **随手可改**：标注加完还能拖动位置、文字可二次编辑，不满意随时调。
- 🔒 **隐私友好**：完全本地运行，不联网、不上传、不采集任何数据。

---

## 🖼️ 界面预览

<div align="center">
<img src="assets/toolbar.png" width="440" alt="工具栏">
</div>

顶部圆角工具栏（Mac 风格柔和阴影 + 线条图标），下方是 **6 种预设颜色**与 **3 级粗细**。

---

## 🚀 快速开始

### 环境要求
- Windows 10 / 11
- Python 3.9+（已安装 Pillow）

```bash
pip install pillow
```

### 运行

```bash
python screenshot_tool.py
```

启动后常驻后台，按 **`Ctrl + Alt + `` **（反引号，Tab 上方那个键）即可截图。

> 💡 想无黑框静默运行，用 `pythonw.exe screenshot_tool.py`（Windows 下不弹控制台）。

### 立即截一次（不常驻）

```bash
python screenshot_tool.py --shot
```

### 自定义快捷键

```bash
python screenshot_tool.py --hotkey "ctrl+alt+q"
```

支持 `ctrl` / `alt` / `shift` / `win` 组合 + 一个键；键可为：字母数字、`f1`~`f12`、
以及 `` ` ``（反引号）、`pause`、`insert`、`scrolllock`、`space`、`printscreen` 等冷门键。

---

## 📖 使用说明

### 全局

| 操作 | 说明 |
|------|------|
| `Ctrl + Alt + `` | 唤起截图（默认，可自定义） |
| 拖动鼠标 | 框选任意区域 |
| 拖动绿色手柄 / 边框 | 缩放 / 移动选区 |

### 标注工具

| 工具 | 说明 |
|------|------|
| ▭ 矩形 · ◯ 椭圆 | 框选区域绘制 |
| ↗ 箭头 | **彗星扫尾 + 头浓尾淡渐变** |
| ✎ 画笔 | 自由手绘 |
| ▨ 马赛克 | 打码遮挡敏感信息 |
| T 文字 | 点击输入，回车确认；支持中文 |

### 二次编辑

| 操作 | 说明 |
|------|------|
| 悬停元素（手型光标）→ 拖动 | 移动已添加的元素 |
| 文字右上角铅笔按钮 / 双击文字 | 重新编辑文字内容 |
| 二级栏圆点 / 色块 | 切换粗细（细/中/粗）/ 颜色 |

### 输出

| 操作 | 说明 |
|------|------|
| ✓ 完成 / `Ctrl + C` | 复制到剪贴板，随处 `Ctrl + V` 粘贴 |
| 💾 保存 / `Ctrl + S` | 另存为 PNG / JPG |
| ↶ 撤销 / `Ctrl + Z` | 撤销上一步 |
| ✕ 关闭 / `Esc` | 取消退出 |

---

## ⚙️ 常驻与开机自启

- **无窗口启动**：双击 `启动截图工具.vbs`（用 `pythonw` 静默运行，无黑框）。
- **单实例设计**：程序只允许一个后台实例。已运行时再次启动（如双击快捷方式）会**直接触发一次截图**，不会产生重复进程。
- **开机自启**：把 `启动截图工具.vbs` 的快捷方式放进启动文件夹
  （`Win + R` → 输入 `shell:startup` → 回车 → 放入快捷方式）。
- **干净重启**：双击 `重启截图工具.vbs`，结束旧实例并启动一个新的。

> ⚠️ 仓库内的 `.vbs` 启动器中的 Python 路径为示例，请按你本机的 `pythonw.exe` 路径修改
> （命令行执行 `where pythonw` 可查看）。

---

## ❓ 常见问题

<details>
<summary><b>启动出现黑色命令框？</b></summary>

用 `python.exe` 或 `.bat` 启动会带控制台。请改用 `pythonw.exe` 或 `.vbs` 启动，即无黑框。
</details>

<details>
<summary><b>按快捷键没反应？</b></summary>

该组合可能被其它软件占用。命令行运行会打印「注册失败」提示，换一个 `--hotkey` 即可。
</details>

<details>
<summary><b>高分屏 / 多显示器坐标错位？</b></summary>

程序已做 Per-Monitor DPI 感知，并按虚拟屏坐标截取，支持多屏与缩放显示。
</details>

<details>
<summary><b>只能在 Windows 用吗？</b></summary>

是。全局快捷键、剪贴板写入、DPI 感知均调用 Windows API，暂不支持 macOS / Linux。
</details>

---

## 🧩 技术亮点

- **DPI 感知**：`SetProcessDpiAwareness` 保证 tkinter 坐标 = 物理像素 = 截图像素，高分屏不偏移。
- **抗锯齿合成**：矢量标注在 4× 超采样图层绘制后 LANCZOS 缩小合成；文字用 1× 直绘保持字体自带抗锯齿。
- **描边居中修正**：Pillow 描边向内扩半个线宽，与画布预览完全一致（真·所见即所得）。
- **彗星箭头**：局部坐标逐列 alpha 渐变 + 旋转合成，尾部平滑淡出。
- **剪贴板写入**：`ctypes` 直接写入 `CF_DIB`，无需额外依赖。
- **全局快捷键**：`RegisterHotKey` + 消息循环独立线程，不抢占前台焦点。
- **单实例通信**：回环 socket 加锁，第二次启动即通知主实例截图。

---

## 📂 目录结构

```
SnapMark/
├── screenshot_tool.py       # 主程序（单文件）
├── requirements.txt
├── assets/                  # 预览图
├── 启动截图工具.vbs          # 无窗口启动
├── 重启截图工具.vbs          # 干净重启
├── 截图一次.bat             # 截一次即退出
├── 截图工具(带窗口).bat      # 带控制台，调试用
└── README.md
```

---

## 🤝 贡献

欢迎 Issue 与 PR：Bug 反馈、新工具（如 序号标签、长截图、OCR）、跨平台适配都很欢迎。

## 📄 许可证

本项目基于 [MIT License](LICENSE) 开源，可自由使用、修改、分发。

<div align="center">

如果 SnapMark 帮到了你，欢迎点一个 ⭐ Star 支持一下！

</div>
