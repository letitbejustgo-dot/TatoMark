# -*- coding: utf-8 -*-
"""
截图标注小工具 (Windows) —— 纯本地实现，依赖：Python + Pillow + tkinter
Snipaste 风格界面：绿色可缩放选区 + 顶部圆角药丸工具栏
所有图标 / 标注均用 Pillow 抗锯齿渲染，平滑无锯齿。

功能：
  1. 全局快捷键调取（默认 Ctrl+Alt+A），或用 --shot 直接截一次
  2. 鼠标框选任意区域，框选后可拖动手柄缩放 / 拖动内部移动
  3. 标注：矩形 / 椭圆 / 箭头(实心) / 画笔 / 马赛克 / 文字
     预设 6 种颜色、3 级粗细
  4. 加完标注后：鼠标悬停元素右上角出现「+」可拖动移动；双击文字可再次编辑
  5. ✓ 完成=复制到剪贴板；💾 =另存到本地；↶ 撤销

用法：
  python screenshot_tool.py            # 常驻后台，按 Ctrl+Alt+A 截图
  python screenshot_tool.py --shot     # 立即截图一次
  python screenshot_tool.py --hotkey "ctrl+shift+a"
"""

import sys
import time
import io
import math
import socket
import ctypes
import ctypes.wintypes as wintypes
import threading
from datetime import datetime

import tkinter as tk
import tkinter.font as tkfont
from tkinter import filedialog, colorchooser

from PIL import (Image, ImageDraw, ImageFont, ImageGrab, ImageTk, ImageFilter)

# --- DPI 感知：必须在创建 Tk 之前 ---
try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass

CN_FONT_PATH = "C:/Windows/Fonts/msyh.ttc"          # UI 文案（提示/尺寸/toast）
CN_FONT_FAMILY = "Microsoft YaHei"

# 截图标注「文字」工具：内置可下拉选择的字体（随项目私有注册，无需系统安装）
import os as _os


def _app_base():
    """资源目录：PyInstaller onefile 打包时为解压临时目录，否则为脚本目录。"""
    return getattr(sys, "_MEIPASS", _os.path.dirname(_os.path.abspath(__file__)))


def _exe_dir():
    """可执行文件/脚本所在目录（用于读取用户可编辑的 config.ini）。"""
    if getattr(sys, "frozen", False):
        return _os.path.dirname(sys.executable)
    return _os.path.dirname(_os.path.abspath(__file__))


_FONT_DIR = _os.path.join(_app_base(), "fonts")
# (显示名, 文件路径, tkinter 族名)
FONTS = [
    ("思源宋体", _os.path.join(_FONT_DIR, "SourceHanSerifCN-Regular.ttf"),
     "思源宋体 CN"),
    ("站酷快乐体", _os.path.join(_FONT_DIR, "ZhanKuKuaiLeTi.ttf"),
     "站酷快乐体2016修订版"),
]
DEFAULT_FONT = FONTS[0][1]
_FONT_CACHE = {}


def register_text_font():
    for _n, p, _f in FONTS:
        try:
            ctypes.windll.gdi32.AddFontResourceExW(ctypes.c_wchar_p(p), 0x10, 0)
        except Exception as ex:
            print("字体注册失败（将回退系统字体）：", ex)


_UI_FONT_CACHE = {}


def ui_font(px):
    """缓存 UI 字体（放大镜/尺寸标签等），避免每帧从磁盘重复加载。"""
    f = _UI_FONT_CACHE.get(px)
    if f is None:
        try:
            f = ImageFont.truetype(CN_FONT_PATH, px)
        except Exception:
            f = ImageFont.load_default()
        _UI_FONT_CACHE[px] = f
    return f


def get_font(path, size):
    key = (path, size)
    f = _FONT_CACHE.get(key)
    if f is None:
        try:
            f = ImageFont.truetype(path, size)
        except Exception:
            try:
                f = ImageFont.truetype(CN_FONT_PATH, size)
            except Exception:
                f = ImageFont.load_default()
        _FONT_CACHE[key] = f
    return f


def family_of(path):
    for _n, p, fam in FONTS:
        if p == path:
            return fam
    return CN_FONT_FAMILY


def name_of(path):
    for n, p, _f in FONTS:
        if p == path:
            return n
    return "字体"


def measure_text(text, size, path=DEFAULT_FONT):
    """单字体 / 多行 测量：返回 (宽, 高, 单行行高 lineh, 基线上高 ascent)。"""
    if text == "":
        text = " "
    f = get_font(path, size)
    asc, desc = f.getmetrics()
    lineh = asc + desc
    lines = text.split("\n")
    w = max((f.getlength(ln or " ") for ln in lines), default=1.0)
    return int(round(w)), lineh * len(lines), lineh, asc


def draw_text_multi(draw, x, y, text, fill, size, path=DEFAULT_FONT):
    """单字体 / 多行 绘制：(x, y) 为左上角。"""
    f = get_font(path, size)
    asc, desc = f.getmetrics()
    lineh = asc + desc
    for i, line in enumerate(text.split("\n")):
        draw.text((x, y + i * lineh), line, fill=fill, font=f)


# 兼容旧调用名（外部预览脚本用 5 参形式，走默认字体）
def measure_mixed(text, size):
    return measure_text(text, size, DEFAULT_FONT)


def draw_mixed(draw, x, y, text, fill, size):
    draw_text_multi(draw, x, y, text, fill, size, DEFAULT_FONT)


TEXT_FONT_FAMILY = family_of(DEFAULT_FONT)

register_text_font()

ACCENT = "#13C060"
ICON = "#41464B"
RED = "#F5453A"
PILL_BG = "#FFFFFF"

PRESET_COLORS = ["#3B9EFF", "#5FCE3B", "#FFB020", "#3A3F44", "#FFFFFF", "#FF5B5B"]
LEVELS = [(3, 27), (9, 51), (21, 87)]   # (线宽, 字号) —— 三档再扩 0.5 倍
MOSAIC_BRUSH = [27, 51, 84]             # 马赛克笔刷直径（三档，再扩 0.5 倍）
SS = 4                                   # 图标超采样
UI = 2                                    # 界面整体缩放倍数（工具栏/取色框放大）

TOOL_NAMES = {
    "rect": "矩形", "rrect": "圆角矩形", "ellipse": "椭圆", "arrow": "箭头",
    "mosaic": "马赛克", "text": "文字", "undo": "撤销", "save": "保存",
    "close": "取消", "confirm": "完成(复制)",
}

PINS = []
MOD_ALT, MOD_CONTROL, MOD_SHIFT, MOD_WIN = 0x0001, 0x0002, 0x0004, 0x0008
WM_HOTKEY = 0x0312
IPC_PORT = 49517          # 单实例通信端口（本机回环）


def acquire_lock():
    """尝试成为主实例：绑定回环端口成功=主实例，返回 socket；否则返回 None。"""
    sk = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sk.bind(("127.0.0.1", IPC_PORT))
        sk.listen(5)
        return sk
    except OSError:
        sk.close()
        return None


def notify_primary():
    """通知已在运行的主实例：立即截图。成功返回 True。"""
    try:
        with socket.create_connection(("127.0.0.1", IPC_PORT), timeout=1) as c:
            c.sendall(b"shot")
        return True
    except OSError:
        return False


def _hex(c):
    c = c.lstrip("#")
    return (int(c[0:2], 16), int(c[2:4], 16), int(c[4:6], 16))


NAMED_VK = {
    "space": 0x20, "pause": 0x13, "break": 0x13, "insert": 0x2D,
    "delete": 0x2E, "home": 0x24, "end": 0x23, "pageup": 0x21,
    "pagedown": 0x22, "scrolllock": 0x91, "numlock": 0x90,
    "printscreen": 0x2C, "prtsc": 0x2C, "tab": 0x09,
    "`": 0xC0, "~": 0xC0, "-": 0xBD, "=": 0xBB, "[": 0xDB, "]": 0xDD,
    ";": 0xBA, "'": 0xDE, ",": 0xBC, ".": 0xBE, "/": 0xBF, "\\": 0xDC,
}


def parse_hotkey(spec):
    mods, vk = 0, None
    # 按 + 分割，但保留末尾单独的 '+' / '`' 等符号键
    parts = [p for p in spec.strip().split("+")]
    # 处理 "ctrl+alt++" 之类：空片段代表 '+'
    tokens = []
    for i, p in enumerate(parts):
        if p == "" and i > 0:
            tokens.append("+")
        else:
            tokens.append(p.strip())
    for part in tokens:
        low = part.lower()
        if low in ("ctrl", "control"):
            mods |= MOD_CONTROL
        elif low == "alt":
            mods |= MOD_ALT
        elif low == "shift":
            mods |= MOD_SHIFT
        elif low in ("win", "super"):
            mods |= MOD_WIN
        elif low in NAMED_VK:
            vk = NAMED_VK[low]
        elif low.startswith("f") and low[1:].isdigit():
            vk = 0x70 + int(low[1:]) - 1
        elif len(part) == 1 and (part.isalnum()):
            vk = ord(part.upper())
    return mods, vk


def hotkey_listener(spec, on_press):
    mods, vk = parse_hotkey(spec)
    if vk is None:
        print("快捷键解析失败：", spec)
        return
    user32 = ctypes.windll.user32
    if not user32.RegisterHotKey(None, 1, mods, vk):
        print(f"注册全局快捷键失败（可能已被占用）：{spec}")
        return
    print(f"已注册全局快捷键：{spec}")
    msg = wintypes.MSG()
    while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) != 0:
        if msg.message == WM_HOTKEY:
            on_press()
        user32.TranslateMessage(ctypes.byref(msg))
        user32.DispatchMessageW(ctypes.byref(msg))


def image_to_clipboard(img):
    output = io.BytesIO()
    img.convert("RGB").save(output, "BMP")
    data = output.getvalue()[14:]
    output.close()
    kernel32 = ctypes.windll.kernel32
    user32 = ctypes.windll.user32
    kernel32.GlobalAlloc.restype = ctypes.c_void_p
    kernel32.GlobalAlloc.argtypes = [wintypes.UINT, ctypes.c_size_t]
    kernel32.GlobalLock.restype = ctypes.c_void_p
    kernel32.GlobalLock.argtypes = [ctypes.c_void_p]
    kernel32.GlobalUnlock.argtypes = [ctypes.c_void_p]
    user32.SetClipboardData.restype = ctypes.c_void_p
    user32.SetClipboardData.argtypes = [wintypes.UINT, ctypes.c_void_p]
    GMEM_MOVEABLE, CF_DIB = 0x0002, 8
    h = kernel32.GlobalAlloc(GMEM_MOVEABLE, len(data))
    if not h:
        return False
    p = kernel32.GlobalLock(h)
    ctypes.memmove(p, data, len(data))
    kernel32.GlobalUnlock(h)
    if not user32.OpenClipboard(None):
        return False
    try:
        user32.EmptyClipboard()
        user32.SetClipboardData(CF_DIB, h)
    finally:
        user32.CloseClipboard()
    return True


def text_to_clipboard(text):
    """把文本放入系统剪贴板（CF_UNICODETEXT，独立于 tk，覆盖窗关闭后仍在）。"""
    kernel32 = ctypes.windll.kernel32
    user32 = ctypes.windll.user32
    kernel32.GlobalAlloc.restype = ctypes.c_void_p
    kernel32.GlobalAlloc.argtypes = [wintypes.UINT, ctypes.c_size_t]
    kernel32.GlobalLock.restype = ctypes.c_void_p
    kernel32.GlobalLock.argtypes = [ctypes.c_void_p]
    kernel32.GlobalUnlock.argtypes = [ctypes.c_void_p]
    user32.SetClipboardData.restype = ctypes.c_void_p
    user32.SetClipboardData.argtypes = [wintypes.UINT, ctypes.c_void_p]
    data = text.encode("utf-16-le") + b"\x00\x00"
    GMEM_MOVEABLE, CF_UNICODETEXT = 0x0002, 13
    h = kernel32.GlobalAlloc(GMEM_MOVEABLE, len(data))
    if not h:
        return False
    p = kernel32.GlobalLock(h)
    ctypes.memmove(p, data, len(data))
    kernel32.GlobalUnlock(h)
    if not user32.OpenClipboard(None):
        return False
    try:
        user32.EmptyClipboard()
        user32.SetClipboardData(CF_UNICODETEXT, h)
    finally:
        user32.CloseClipboard()
    return True


def send_ctrl_v():
    """向当前前台窗口发送 Ctrl+V，在光标/焦点处粘贴剪贴板内容。"""
    try:
        u = ctypes.windll.user32
        VK_CONTROL, VK_V, KEYUP = 0x11, 0x56, 0x0002
        u.keybd_event(VK_CONTROL, 0, 0, 0)
        u.keybd_event(VK_V, 0, 0, 0)
        u.keybd_event(VK_V, 0, KEYUP, 0)
        u.keybd_event(VK_CONTROL, 0, KEYUP, 0)
    except Exception:
        pass


def _lerp(a, b, t):
    return a + (b - a) * t


def _blend_hex(color, target, t):
    r, g, b = _hex(color)
    tr, tg, tb = target
    return "#%02x%02x%02x" % (int(_lerp(r, tr, t)), int(_lerp(g, tg, t)),
                              int(_lerp(b, tb, t)))


def comet_params(x0, y0, x1, y1, w):
    dx, dy = x1 - x0, y1 - y0
    L = math.hypot(dx, dy) or 1.0
    ux, uy = dx / L, dy / L
    px, py = -uy, ux
    head_len = min(L * 0.5, 10 + 3.0 * w)
    head_w = 5 + 1.9 * w
    neck_w = max(1.2, 0.9 * w)
    return L, ux, uy, px, py, head_len, head_w, neck_w


def comet_polys(x0, y0, x1, y1, w, color, N=22):
    """彗星箭头（画布用）：多段四边形，尾尖头宽，颜色向尾部渐淡。"""
    L, ux, uy, px, py, hl, hw, nw = comet_params(x0, y0, x1, y1, w)
    nx, ny = x1 - ux * hl, y1 - uy * hl
    polys = []

    def pt(s):
        return (x0 + (nx - x0) * s, y0 + (ny - y0) * s)

    prev, prevw = pt(0.0), 0.0
    for i in range(1, N + 1):
        s = i / N
        c = pt(s)
        hwi = nw * (s ** 0.72)
        quad = [prev[0] + px * prevw, prev[1] + py * prevw,
                c[0] + px * hwi, c[1] + py * hwi,
                c[0] - px * hwi, c[1] - py * hwi,
                prev[0] - px * prevw, prev[1] - py * prevw]
        amt = max(0.0, 0.72 * (1 - (s - 0.5 / N)))     # 尾淡头浓（向白渐变）
        polys.append((quad, _blend_hex(color, (255, 255, 255), amt)))
        prev, prevw = c, hwi
    polys.append(([nx + px * hw, ny + py * hw, x1, y1, nx - px * hw,
                   ny - py * hw], color))
    return polys


def render_comet_ss(x0, y0, x1, y1, w, color, ss):
    """彗星箭头（导出/合成用）：局部坐标逐列 alpha 渐变 + 旋转，尾部淡出透明。"""
    L, ux, uy, px, py, hl, hw, nw = comet_params(x0, y0, x1, y1, w)
    ang = math.degrees(math.atan2(y1 - y0, x1 - x0))
    Ls, hws, nws, hls = L * ss, hw * ss, nw * ss, hl * ss
    pad = int(hws) + 6
    Wl, Hl = int(Ls) + 2 * pad, int(2 * hws) + 2 * pad
    img = Image.new("RGBA", (Wl, Hl), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cy = Hl / 2.0
    x_tail = pad
    x_tip = pad + Ls
    x_neck = x_tip - hls
    r, g, b = _hex(color)
    span = max(1.0, x_neck - x_tail)
    x = x_tail
    while x <= x_neck:
        s = (x - x_tail) / span
        hwi = nws * (s ** 0.72)
        alpha = int(255 * (0.10 + 0.90 * s))
        d.line([(x, cy - hwi), (x, cy + hwi)], fill=(r, g, b, alpha), width=1)
        x += 1
    d.polygon([(x_neck, cy - hws), (x_tip, cy), (x_neck, cy + hws)],
              fill=(r, g, b, 255))
    return img.rotate(-ang, resample=Image.BICUBIC, expand=True)


def render_comet_curve(p0, p1, p2, w, color, ss):
    """可弯折的彗星箭头：沿二次贝塞尔(p0→p2，控制点 p1)绘制，
    尾细透明→头浓，末端三角箭头。返回 (ss 分辨率 RGBA 图, 左上x, 左上y)。
    当 p1 = (p0+p2)/2 时即为直线箭头，外观与直箭头一致。"""
    N = 56
    pts = []
    for i in range(N + 1):
        t = i / N
        mt = 1 - t
        x = mt * mt * p0[0] + 2 * mt * t * p1[0] + t * t * p2[0]
        y = mt * mt * p0[1] + 2 * mt * t * p1[1] + t * t * p2[1]
        pts.append((x, y))
    neck_w = max(1.2, 0.9 * w)
    head_w = 5 + 1.9 * w
    seg = [math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1])
           for i in range(N)]
    total = sum(seg) or 1.0
    head_len = min(total * 0.5, 10 + 3.0 * w)
    cum = [0.0]
    for s_ in seg:
        cum.append(cum[-1] + s_)
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    maxr = max(head_w, neck_w)
    pad = int(maxr) + 4
    minx, miny = min(xs) - pad, min(ys) - pad
    W = int(max(xs) - min(xs)) + 2 * pad
    H = int(max(ys) - min(ys)) + 2 * pad
    Wss, Hss = max(1, int(W * ss)), max(1, int(H * ss))
    grad = Image.new("L", (Wss, Hss), 0)
    gd = ImageDraw.Draw(grad)

    def LP(p):
        return ((p[0] - minx) * ss, (p[1] - miny) * ss)

    # 找到颈部（彗尾结束处，约在凹口深度），其后交给箭头头部
    head_shaft = head_len * 0.62
    neck_i = N
    acc = 0.0
    for i in range(N - 1, -1, -1):
        acc += seg[i]
        if acc >= head_shaft:
            neck_i = i
            break

    def normal(i):
        a = pts[max(0, i - 1)]
        b = pts[min(N, i + 1)]
        tx, ty = b[0] - a[0], b[1] - a[1]
        L = math.hypot(tx, ty) or 1.0
        return (-ty / L, tx / L)

    # 平滑彗尾：沿曲线两侧偏移出连续变宽的带状，逐段四边形填充（无颗粒感）
    Lp, Rp = [], []
    for i in range(N + 1):
        s = cum[i] / total
        hw = neck_w * (s ** 0.72)
        nx_, ny_ = normal(i)
        Lp.append((pts[i][0] + nx_ * hw, pts[i][1] + ny_ * hw))
        Rp.append((pts[i][0] - nx_ * hw, pts[i][1] - ny_ * hw))
    for i in range(neck_i):
        s = cum[i + 1] / total
        alpha = int(255 * (0.10 + 0.90 * s))     # 升序绘制：头端 alpha 覆盖重叠处
        gd.polygon([LP(Lp[i]), LP(Lp[i + 1]), LP(Rp[i + 1]), LP(Rp[i])],
                   fill=alpha)
    # 带倒刺的箭头头部（似中国古箭）：尖端 + 两侧后掠倒刺 + 前凹的后端凹口。
    # 关键：后端凹口 = 彗尾的实际终点 pts[neck_i]（保证与尾巴严丝合缝，
    # 任何弯折都不脱节）；方向取「凹口→尖端」的弦向，稳定贴合曲线。
    tip = pts[N]
    Pn = pts[neck_i]                          # 彗尾终点，即箭头后端凹口
    dx, dy = tip[0] - Pn[0], tip[1] - Pn[1]
    Ln = math.hypot(dx, dy) or 1.0
    ux, uy = dx / Ln, dy / Ln
    pxp, pyp = -uy, ux
    barbBase = (tip[0] - ux * head_len, tip[1] - uy * head_len)   # 倒刺基准(尖端后 head_len)
    barbL = (barbBase[0] + pxp * head_w, barbBase[1] + pyp * head_w)
    barbR = (barbBase[0] - pxp * head_w, barbBase[1] - pyp * head_w)
    gd.polygon([LP(barbL), LP(tip), LP(barbR), LP(Pn)], fill=255)
    cr, cg, cb = _hex(color)
    img = Image.new("RGBA", (Wss, Hss), (cr, cg, cb, 255))
    img.putalpha(grad)
    return img, int(minx), int(miny)


def arrow_polygon(x0, y0, x1, y1, w):
    """实心渐细箭头：尾细→颈渐宽→大三角头。"""
    dx, dy = x1 - x0, y1 - y0
    L = math.hypot(dx, dy) or 1.0
    ux, uy = dx / L, dy / L
    px, py = -uy, ux
    head_len = min(L * 0.6, 12 + 3.2 * w)
    head_w = 5 + 1.9 * w
    neck_w = max(1.6, 0.95 * w)
    tail_w = max(1.0, 0.5 * w)
    bx, by = x1 - ux * head_len, y1 - uy * head_len
    return [(x0 + px * tail_w, y0 + py * tail_w),
            (bx + px * neck_w, by + py * neck_w),
            (bx + px * head_w, by + py * head_w),
            (x1, y1),
            (bx - px * head_w, by - py * head_w),
            (bx - px * neck_w, by - py * neck_w),
            (x0 - px * tail_w, y0 - py * tail_w)]


# ---------------------------------------------------------------------------
# Pillow 抗锯齿图标
# ---------------------------------------------------------------------------
_ICON_CACHE = {}


def _smooth_resize(im, size):
    """预乘 alpha 后再 LANCZOS 缩小，消除透明边缘的暗色毛边（真正的抗锯齿）。"""
    try:
        import numpy as np
        a = np.asarray(im.convert("RGBA")).astype(np.float32)
        al = a[..., 3:4] / 255.0
        a[..., :3] *= al                       # 预乘
        pm = Image.fromarray(a.astype("uint8"), "RGBA").resize(size, Image.LANCZOS)
        b = np.asarray(pm).astype(np.float32)
        al2 = np.clip(b[..., 3:4], 1.0, 255.0) / 255.0
        b[..., :3] = np.clip(b[..., :3] / al2, 0, 255)   # 反预乘
        return Image.fromarray(b.astype("uint8"), "RGBA")
    except Exception:
        return im.resize(size, Image.LANCZOS)


# 工具/动作 → icon 文件夹内的 PNG 文件名
_ICON_DIR = _os.path.join(_app_base(), "icon")
_ICON_FILE = {
    "rect": "直方框.png", "rrect": "圆角方框.png", "ellipse": "圆圈.png",
    "arrow": "长箭头-右上.png", "mosaic": "马赛克.png", "text": "文本块.png",
    "undo": "撤销.png", "save": "下载.png", "close": "取消.png",
    "confirm": "确定.png", "edit": "可编辑.png", "rotate": "顺时针方向.png",
}
_ICON_SRC_CACHE = {}


def _load_icon_src(name):
    """加载并裁剪到内容边界的原始 PNG 图标（缓存）。"""
    im = _ICON_SRC_CACHE.get(name)
    if im is None:
        im = Image.open(_os.path.join(_ICON_DIR, _ICON_FILE[name])).convert("RGBA")
        bbox = im.getbbox()
        if bbox:
            im = im.crop(bbox)
        _ICON_SRC_CACHE[name] = im
    return im


def render_icon(name, px=32, state="normal"):
    """从 icon 文件夹加载 PNG，统一裁剪、居中、按比例平滑缩放到按钮尺寸。"""
    key = (name, px, state)
    if key in _ICON_CACHE:
        return _ICON_CACHE[key]
    S = px * SS
    im = Image.new("RGBA", (S, S), (255, 255, 255, 0))
    d = ImageDraw.Draw(im)

    if state == "hover":
        d.rounded_rectangle([2 * SS, 2 * SS, (px - 2) * SS, (px - 2) * SS],
                            radius=8 * SS, fill=(238, 240, 242, 255))
    elif state == "active":
        d.rounded_rectangle([2 * SS, 2 * SS, (px - 2) * SS, (px - 2) * SS],
                            radius=8 * SS, fill=(225, 246, 234, 255))

    # 统一视觉尺寸：把裁剪后的图标按比例塞进居中的目标框
    target = 30 * SS                       # 图标在按钮内占比更大（再放大 2 号）
    src = _load_icon_src(name)
    sw, sh = src.size
    k = target / float(max(sw, sh))
    gw, gh = max(1, int(round(sw * k))), max(1, int(round(sh * k)))
    glyph = _smooth_resize(src, (gw, gh))
    im.alpha_composite(glyph, ((S - gw) // 2, (S - gh) // 2))

    out = ImageTk.PhotoImage(_smooth_resize(im, (px, px)))
    _ICON_CACHE[key] = out
    return out


def render_swatch(color, selected, px=24):
    """颜色色块：圆角方块（与粗细档位的圆点区分）。"""
    S = px * SS
    im = Image.new("RGBA", (S, S), (255, 255, 255, 0))
    d = ImageDraw.Draw(im)
    outline = (204, 204, 204) if color == "#FFFFFF" else _hex(color)
    d.rounded_rectangle([4 * SS, 4 * SS, (px - 4) * SS, (px - 4) * SS],
                        radius=int(3.2 * SS), fill=_hex(color),
                        outline=outline, width=SS)
    if selected:
        d.rounded_rectangle([1 * SS, 1 * SS, (px - 1) * SS, (px - 1) * SS],
                            radius=int(5 * SS), outline=_hex(ACCENT),
                            width=2 * SS)
    return ImageTk.PhotoImage(_smooth_resize(im, (px, px)))


def render_level(idx, active, px=28):
    """粗细档位圆点：三档半径差距加大，更易区分。"""
    S = px * SS
    im = Image.new("RGBA", (S, S), (255, 255, 255, 0))
    d = ImageDraw.Draw(im)
    rr = [2.5, 6, 10.5][idx]
    c = _hex(ACCENT) if active else (176, 182, 189)
    cx = px / 2
    d.ellipse([(cx - rr) * SS, (cx - rr) * SS, (cx + rr) * SS, (cx + rr) * SS],
              fill=c)
    return ImageTk.PhotoImage(_smooth_resize(im, (px, px)))


def render_plus(px=24):
    S = px * SS
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse([4 * SS, 4 * SS, (px - 4) * SS, (px - 4) * SS],
              outline=(153, 153, 153), width=SS)
    cx = px / 2
    d.line([(cx - 4) * SS, cx * SS, (cx + 4) * SS, cx * SS],
           fill=(120, 120, 120), width=SS)
    d.line([cx * SS, (cx - 4) * SS, cx * SS, (cx + 4) * SS],
           fill=(120, 120, 120), width=SS)
    return ImageTk.PhotoImage(_smooth_resize(im, (px, px)))


def render_badge(icon_name, px=22):
    """白底圆形徽标 + 居中图标（用于文字编辑铅笔、箭头旋转手柄等）。"""
    S = px * SS
    im = Image.new("RGBA", (S, S), (255, 255, 255, 0))
    d = ImageDraw.Draw(im)
    d.ellipse([SS, SS, (px - 1) * SS, (px - 1) * SS], fill=(255, 255, 255, 255),
              outline=_hex(ACCENT), width=int(1.4 * SS))
    src = _load_icon_src(icon_name)
    box = int((px - 8) * SS)
    sw, sh = src.size
    k = box / float(max(sw, sh))
    gw, gh = max(1, int(sw * k)), max(1, int(sh * k))
    glyph = _smooth_resize(src, (gw, gh))
    im.alpha_composite(glyph, ((S - gw) // 2, (S - gh) // 2))
    return ImageTk.PhotoImage(_smooth_resize(im, (px, px)))


def render_pencil(px=22):
    return render_badge("edit", px)


def render_pill(w, h, seps, radius=None):
    """圆角白色药丸 + 柔和阴影，返回 (PhotoImage, margin)。radius 默认半高(胶囊)。"""
    if radius is None:
        radius = h // 2
    M = 16
    W, Hh = w + 2 * M, h + 2 * M

    # 阴影：模糊后本身即平滑，1x 绘制即可
    shadow = Image.new("RGBA", (W, Hh), (0, 0, 0, 0))
    ds = ImageDraw.Draw(shadow)
    ds.rounded_rectangle([M, M + 3, M + w, M + h + 3], radius=radius,
                         fill=(0, 0, 0, 55))
    shadow = shadow.filter(ImageFilter.GaussianBlur(7))
    base = Image.alpha_composite(Image.new("RGBA", (W, Hh), (0, 0, 0, 0)),
                                 shadow)

    # 药丸主体 + 分隔线：超采样绘制 → LANCZOS 缩小，抗锯齿
    # 关键：透明底的 RGB 预置为白色，避免缩小时白色药丸与黑色透明像素
    # 插值产生灰色毛边（此前「外围椭圆圈锯齿」的真正成因）。
    ss = SS
    big = Image.new("RGBA", (W * ss, Hh * ss), (255, 255, 255, 0))
    db = ImageDraw.Draw(big)
    db.rounded_rectangle([M * ss, M * ss, (M + w) * ss, (M + h) * ss],
                         radius=radius * ss, fill=(255, 255, 255, 255),
                         outline=(228, 230, 234, 255), width=ss)
    for cx in seps:
        db.line([(M + cx) * ss, (M + 10) * ss, (M + cx) * ss, (M + h - 10) * ss],
                fill=(224, 227, 231, 255), width=ss)
    big = big.resize((W, Hh), Image.LANCZOS)
    base = Image.alpha_composite(base, big)
    return ImageTk.PhotoImage(base), M


class ScreenshotTool:
    def __init__(self, resident=True, hotkey="ctrl+alt+`", ipc_sock=None):
        self.resident = resident
        self.hotkey = hotkey
        self.ipc_sock = ipc_sock
        self.root = tk.Tk()
        self.root.withdraw()
        self.overlay = None
        self.active = False
        self.trigger = threading.Event()

        self.tool = None
        self.color = "#FF5B5B"
        self.level = 1
        self.width, self.font_size = LEVELS[self.level]
        self.annotations = []
        self.tool_buttons = {}
        self.level_buttons = []
        self.hover_anno_idx = None
        self._imgrefs = []

        if resident:
            threading.Thread(target=hotkey_listener,
                             args=(self.hotkey, self._hotkey_fired),
                             daemon=True).start()
            if self.ipc_sock is not None:
                threading.Thread(target=self._ipc_listen, daemon=True).start()
            self._poll_trigger()
            self.root.after(400, self._startup_toast)
            print("截图工具已启动。按快捷键开始截图，关闭此窗口即退出。")
        else:
            self.root.after(250, self.start_capture)

    def _ipc_listen(self):
        while True:
            try:
                conn, _ = self.ipc_sock.accept()
            except OSError:
                break
            try:
                conn.recv(16)
                conn.close()
            except OSError:
                pass
            self.trigger.set()

    def _startup_toast(self):
        hk = self.hotkey.replace("ctrl", "Ctrl").replace("alt", "Alt") \
            .replace("shift", "Shift").replace("+", "+")
        try:
            self.flash_toast(f"截图工具已就绪 · 按 {hk} 截图")
        except Exception:
            pass

    def _hotkey_fired(self):
        self.trigger.set()

    def _poll_trigger(self):
        if self.trigger.is_set():
            self.trigger.clear()
            self.start_capture()
        self.root.after(80, self._poll_trigger)

    def _poll_keys(self):
        """overrideredirect 覆盖窗常拿不到键盘焦点，直接轮询 Esc 键保证可退出。"""
        if not self.active or self.overlay is None:
            return
        try:
            if ctypes.windll.user32.GetAsyncKeyState(0x1B) & 0x8000:
                self.cancel()
                return
        except Exception:
            pass
        self.root.after(60, self._poll_keys)

    # ---------------- 截图 ----------------
    def start_capture(self):
        if self.active:
            return
        self.active = True
        u = ctypes.windll.user32
        self.vx, self.vy = u.GetSystemMetrics(76), u.GetSystemMetrics(77)
        self.vw, self.vh = u.GetSystemMetrics(78), u.GetSystemMetrics(79)
        try:
            self.full_img = ImageGrab.grab(
                bbox=(self.vx, self.vy, self.vx + self.vw, self.vy + self.vh),
                all_screens=True)
        except Exception:
            self.full_img = ImageGrab.grab()
            self.vx, self.vy = 0, 0
            self.vw, self.vh = self.full_img.size
        if self.full_img.mode != "RGB":
            self.full_img = self.full_img.convert("RGB")

        # 变暗背景（point 比 blend 略快，且不额外分配整幅黑图）
        self.dim_img = self.full_img.point(lambda p: int(p * 0.5))

        # 磁吸：在创建覆盖窗之前枚举当前可见窗口（此时覆盖窗尚未遮挡）
        self._enum_top_windows()
        self._child_cache = {}
        self._snap_rect = None

        self.overlay = tk.Toplevel(self.root)
        self.overlay.overrideredirect(True)
        self.overlay.geometry(f"{self.vw}x{self.vh}+{self.vx}+{self.vy}")
        self.overlay.attributes("-topmost", True)
        self.overlay.configure(cursor="crosshair")

        self.canvas = tk.Canvas(self.overlay, width=self.vw, height=self.vh,
                                highlightthickness=0, bd=0)
        self.canvas.pack(fill="both", expand=True)
        self.bg_photo = ImageTk.PhotoImage(self.dim_img)
        self.canvas.create_image(0, 0, anchor="nw", image=self.bg_photo)

        self.mode = "select"
        self.sel = None
        self.start_pt = None
        self.drag_mode = None
        self.active_handle = None
        self.temp_item = None
        self.pen_points = None
        self._mosaic_stamps = None
        self._ov_photo = None
        self._pencil_img = None
        self._rotate_img = None
        self.mosaic_shape = "rect"
        self.font_path = DEFAULT_FONT
        self._last_sel_hover = 0.0
        self._pending_hover = None
        self._hover_scheduled = False
        self._mag_photo = None
        self._mag_item = None
        self._crop_photo = None
        self.text_entry = None
        self.tool = None
        self.annotations = []
        self.hover_anno_idx = None
        self.selected_idx = None
        self.move_idx = None
        self._move_photo = None
        self._tb_widgets = []
        self._imgrefs = []

        self.canvas.bind("<ButtonPress-1>", self.on_press)
        self.canvas.bind("<B1-Motion>", self.on_drag)
        self.canvas.bind("<ButtonRelease-1>", self.on_release)
        self.canvas.bind("<Motion>", self.on_hover)
        self.canvas.bind("<Double-Button-1>", self.on_double)
        self.canvas.bind("<Button-3>", self.on_rclick)   # 右键取色
        # bind_all + 逐控件绑定，双保险保证 Esc/快捷键在任意焦点下都能触发
        for tgt in (self.overlay.bind_all, self.overlay.bind, self.canvas.bind):
            tgt("<Escape>", lambda e: self.cancel())
            tgt("<Control-z>", lambda e: self.undo())
            tgt("<Control-c>", lambda e: self.do_copy())
            tgt("<Control-s>", lambda e: self.do_save())
            tgt("<Delete>", lambda e: self.delete_selected())
        self.overlay.focus_force()
        self.canvas.focus_set()
        self.root.after(60, self._poll_keys)      # Esc 轮询兜底
        self.root.after(40, self._initial_hover)  # 启动即显示取色镜/吸附

    def _initial_hover(self):
        """无需等鼠标移动，进入即在当前光标处显示取色放大镜与磁吸高亮。"""
        if not self.active or self.overlay is None or self.mode != "select":
            return
        try:
            pt = wintypes.POINT()
            ctypes.windll.user32.GetCursorPos(ctypes.byref(pt))
            x, y = pt.x - self.vx, pt.y - self.vy
            if 0 <= x < self.vw and 0 <= y < self.vh:
                try:
                    self._update_snap(x, y)
                except Exception:
                    pass
                self._update_magnifier(x, y)
        except Exception:
            pass

        self.pick_hex = None
        self._mag_photo = None
        self.hint = self.canvas.create_text(
            self.vw // 2, 34,
            text="移动自动吸附窗口/模块，单击选取；拖动可自定义框选    ·    "
                 "放大镜取色，右键复制色值    ·    Esc 取消",
            fill="#EEEEEE", font=(CN_FONT_FAMILY, -15 * UI))

    # ---------------- 选区阶段 ----------------
    def on_press(self, e):
        if self.mode == "select":
            self.start_pt = (e.x, e.y)
            self.canvas.delete("mag")
            self.canvas.delete("snap")
        else:
            self.edit_press(e)

    def on_drag(self, e):
        if self.mode == "select":
            self.update_selection(e)
        else:
            self.edit_drag(e)

    def on_release(self, e):
        if self.mode == "select":
            self.finish_selection(e)
        else:
            self.edit_release(e)

    def update_selection(self, e):
        if not self.start_pt:
            return
        x0, y0 = self.start_pt
        l, t = min(x0, e.x), min(y0, e.y)
        r, b = max(x0, e.x), max(y0, e.y)
        self.canvas.delete("sel")
        crop = self.full_img.crop((l, t, r, b))
        if crop.width > 0 and crop.height > 0:
            self._sel_preview = ImageTk.PhotoImage(crop)
            self.canvas.create_image(l, t, anchor="nw",
                                     image=self._sel_preview, tags="sel")
        self.canvas.create_rectangle(l, t, r, b, outline=ACCENT, width=2,
                                     tags="sel")
        self._size_label(l, t, r, b, "sel")

    def finish_selection(self, e):
        if not self.start_pt:
            return
        x0, y0 = self.start_pt
        l, t = min(x0, e.x), min(y0, e.y)
        r, b = max(x0, e.x), max(y0, e.y)
        if r - l < 8 or b - t < 8:
            # 视为一次点击：若命中磁吸窗口/模块 → 直接选取该区域
            snap = getattr(self, "_snap_rect", None)
            if snap:
                sl, st = max(0, int(snap[0])), max(0, int(snap[1]))
                sr, sb = min(self.vw, int(snap[2])), min(self.vh, int(snap[3]))
                if sr - sl > 8 and sb - st > 8:
                    self.sel = [sl, st, sr, sb]
                    self.mode = "edit"
                    self.canvas.delete(self.hint)
                    self.canvas.delete("sel")
                    self.canvas.delete("mag")
                    self.canvas.delete("snap")
                    self.build_toolbar()
                    self.redraw_static()
                    self.start_pt = None
                    return
            self.start_pt = None
            return
        self.sel = [l, t, r, b]
        self.mode = "edit"
        self.canvas.delete(self.hint)
        self.canvas.delete("sel")
        self.canvas.delete("mag")
        self.canvas.delete("snap")
        self.build_toolbar()
        self.redraw_static()

    # ---------------- 选区手柄 ----------------
    def handle_points(self):
        l, t, r, b = self.sel
        mx, my = (l + r) // 2, (t + b) // 2
        return {"nw": (l, t), "n": (mx, t), "ne": (r, t), "e": (r, my),
                "se": (r, b), "s": (mx, b), "sw": (l, b), "w": (l, my)}

    def hit_test(self, x, y):
        if not self.sel:
            return ("outside",)
        for pos, (hx, hy) in self.handle_points().items():
            if abs(x - hx) <= 8 and abs(y - hy) <= 8:
                return ("handle", pos)
        l, t, r, b = self.sel
        near = ((abs(x - l) <= 5 or abs(x - r) <= 5) and t - 5 <= y <= b + 5) \
            or ((abs(y - t) <= 5 or abs(y - b) <= 5) and l - 5 <= x <= r + 5)
        if near:
            return ("border",)
        if l < x < r and t < y < b:
            return ("inside",)
        return ("outside",)

    # ---------------- 标注命中 ----------------
    def _text_size(self, text, fs):
        # 与最终 Pillow 混排渲染一致的测量（中文齐伋体 + 英文 California FB）
        w, h, _lh, _asc = measure_mixed(text or " ", fs)
        return w, h

    def _rrect_radius(self, x0, y0, x1, y1):
        """圆角方框的圆角半径：随尺寸自适应并封顶。"""
        m = min(abs(x1 - x0), abs(y1 - y0))
        return max(4.0, min(m * 0.22, 32.0, m / 2.0))

    def _anno_bbox(self, a):
        t = a["type"]
        if t == "mosaic":
            r = a.get("brush", 20) / 2.0
            xs = [s[0] for s in a["stamps"]]
            ys = [s[1] for s in a["stamps"]]
            return (min(xs) - r, min(ys) - r, max(xs) + r, max(ys) + r)
        if t in ("rect", "rrect", "ellipse", "arrow"):
            # 箭头方框只由两端点决定，弯折控制点不改变方框大小
            x0, y0, x1, y1 = a["coords"]
            return (min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1))
        if t == "pen":
            xs, ys = a["points"][0::2], a["points"][1::2]
            return (min(xs), min(ys), max(xs), max(ys))
        if t == "text":
            x, y = a["coords"]
            w, h, _lh, _a = measure_text(a["text"], a["font_size"],
                                         a.get("font", DEFAULT_FONT))
            return (x, y, x + w, y + h)
        return (0, 0, 0, 0)

    def _anno_at(self, x, y, pad=5):
        for i in range(len(self.annotations) - 1, -1, -1):
            a = self.annotations[i]
            if a["type"] == "mosaic":
                continue                      # 马赛克不参与悬停/移动
            bx0, by0, bx1, by1 = self._anno_bbox(a)
            if bx0 - pad <= x <= bx1 + pad and by0 - pad <= y <= by1 + pad:
                return i
            # 箭头的弯折控制点可能被拖到方框外，仍应可命中以便再次调整
            if a["type"] == "arrow":
                dx, dy = self._arrow_dot(a)
                if (x - dx) ** 2 + (y - dy) ** 2 <= 144:
                    return i
        return None

    def _in_pencil(self, x, y, idx):
        """是否点在文字右上角的铅笔按钮上。"""
        a = self.annotations[idx]
        if a["type"] != "text":
            return False
        bx0, by0, bx1, by1 = self._anno_bbox(a)
        return (x - bx1) ** 2 + (y - by0) ** 2 <= 13 ** 2

    def _draw_edit_handle(self, idx):
        """文字元素右上角绘制铅笔编辑按钮。"""
        self.canvas.delete("editbtn")
        if self.annotations[idx]["type"] != "text":
            return
        if not hasattr(self, "_pencil_img") or self._pencil_img is None:
            self._pencil_img = render_pencil(22 * UI)
        bx0, by0, bx1, by1 = self._anno_bbox(self.annotations[idx])
        self.canvas.create_image(bx1, by0, image=self._pencil_img,
                                 tags="editbtn")

    def _extend_hover(self, x, y, idx):
        """当鼠标落在已悬停要素的外部把手（铅笔/旋转/弯折点）上时，保持该要素。"""
        h = self.hover_anno_idx
        if idx is None and h is not None and h < len(self.annotations):
            if (self._in_pencil(x, y, h) or self._in_rotate_handle(x, y, h)
                    or self._in_arrow_dot(x, y, h)):
                return h
        return idx

    def _arrow_dot(self, a):
        """箭头结构中心（曲线中点 B(0.5)），即拖动控制点在曲线上的落点。"""
        x0, y0, x1, y1 = a["coords"]
        cx, cy = a.get("ctrl") or ((x0 + x1) / 2.0, (y0 + y1) / 2.0)
        return (0.25 * x0 + 0.5 * cx + 0.25 * x1,
                0.25 * y0 + 0.5 * cy + 0.25 * y1)

    def _in_arrow_dot(self, x, y, idx):
        a = self.annotations[idx]
        if a["type"] != "arrow":
            return False
        dx, dy = self._arrow_dot(a)
        return (x - dx) ** 2 + (y - dy) ** 2 <= 100      # 半径 10 命中

    def _rotate_handle_pos(self, idx):
        bx0, by0, bx1, by1 = self._anno_bbox(self.annotations[idx])
        return (bx1, by0)                                # 方框右上角

    def _in_rotate_handle(self, x, y, idx):
        if self.annotations[idx]["type"] != "arrow":
            return False
        hx, hy = self._rotate_handle_pos(idx)
        return (x - hx) ** 2 + (y - hy) ** 2 <= 144      # 半径 12 命中

    def _draw_move_box(self, idx):
        """可移动状态：四边虚线框 + 8 个节点方框，标示当前要移动的要素。
        非文字要素（只能移动不能编辑）用灰色；虚线贴在要素线条中心而非外围。"""
        self.canvas.delete("movebox")
        if idx is None or idx >= len(self.annotations):
            return
        a = self.annotations[idx]
        editable = (a["type"] == "text")
        col = ACCENT if editable else "#9AA0A6"       # 文字绿色，其余灰色
        bx0, by0, bx1, by1 = self._anno_bbox(a)       # 形状 bbox = 线条中心线
        # 细长的「长划 + 点」虚线（dash-dot），比例更精致
        self.canvas.create_rectangle(bx0, by0, bx1, by1, outline=col,
                                     width=2, dash=(16, 5, 2, 5), tags="movebox")
        nsz = 3.0 * UI                                        # 圆形节点半径
        mx, my = (bx0 + bx1) / 2.0, (by0 + by1) / 2.0
        for nx, ny in [(bx0, by0), (mx, by0), (bx1, by0), (bx1, my),
                       (bx1, by1), (mx, by1), (bx0, by1), (bx0, my)]:
            self.canvas.create_oval(nx - nsz, ny - nsz, nx + nsz, ny + nsz,
                                    fill="white", outline=col,
                                    width=1, tags="movebox")
        if a["type"] == "arrow":
            # 结构中心实心圆点：在方框内拖动它即可改变箭头弯折形状
            dx, dy = self._arrow_dot(a)
            rd = 5 * UI
            self.canvas.create_oval(dx - rd, dy - rd, dx + rd, dy + rd,
                                    fill=ACCENT, outline="white", width=1,
                                    tags="movebox")
            # 右上角旋转手柄（顺时针方向.png）：拖动可整体旋转箭头方位
            if not hasattr(self, "_rotate_img") or self._rotate_img is None:
                self._rotate_img = render_badge("rotate", 22 * UI)
            hx, hy = self._rotate_handle_pos(idx)
            self.canvas.create_image(hx, hy, image=self._rotate_img,
                                     tags="movebox")
        self.canvas.tag_raise("editbtn")

    # ---------------- 取色放大镜（选区阶段）----------------
    def _pick_color_at(self, x, y):
        x = min(max(int(x), 0), self.vw - 1)
        y = min(max(int(y), 0), self.vh - 1)
        return self.full_img.getpixel((x, y))[:3]

    # ---------------- 窗口/模块 磁吸 ----------------
    def _win_rect_dwm(self, hwnd):
        """优先取 DWM 真实边界（去掉 Win10/11 的隐形阴影边框）。"""
        rc = wintypes.RECT()
        try:
            if ctypes.windll.dwmapi.DwmGetWindowAttribute(
                    ctypes.c_void_p(hwnd), 9, ctypes.byref(rc),
                    ctypes.sizeof(rc)) == 0 and rc.right > rc.left:
                return rc
        except Exception:
            pass
        ctypes.windll.user32.GetWindowRect(hwnd, ctypes.byref(rc))
        return rc

    def _enum_top_windows(self):
        user32 = ctypes.windll.user32
        rects = []
        PROTO = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p,
                                   ctypes.c_void_p)

        def cb(hwnd, lparam):
            try:
                if not user32.IsWindowVisible(hwnd) or user32.IsIconic(hwnd):
                    return True
                rc = self._win_rect_dwm(hwnd)
                w, h = rc.right - rc.left, rc.bottom - rc.top
                if w < 80 or h < 60:
                    return True
                rects.append((rc.left, rc.top, rc.right, rc.bottom, hwnd))
            except Exception:
                pass
            return True

        try:
            user32.EnumWindows(PROTO(cb), 0)
        except Exception:
            pass
        self.snap_top = rects            # Z 序：最前的窗口在前

    def _children_of(self, hwnd):
        if hwnd in self._child_cache:
            return self._child_cache[hwnd]
        user32 = ctypes.windll.user32
        res = []
        PROTO = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p,
                                   ctypes.c_void_p)

        def cb(ch, lparam):
            try:
                if not user32.IsWindowVisible(ch):
                    return True
                rc = wintypes.RECT()
                user32.GetWindowRect(ch, ctypes.byref(rc))
                w, h = rc.right - rc.left, rc.bottom - rc.top
                if w < 48 or h < 24:
                    return True
                res.append((rc.left, rc.top, rc.right, rc.bottom))
            except Exception:
                pass
            return True

        try:
            user32.EnumChildWindows(hwnd, PROTO(cb), 0)
        except Exception:
            pass
        self._child_cache[hwnd] = res
        return res

    def _snap_candidate(self, x, y):
        """返回光标就近的窗口/模块矩形（画布坐标），无则 None。"""
        if not getattr(self, "snap_top", None):
            return None
        ax, ay = x + self.vx, y + self.vy
        for (l, t, r, b, hwnd) in self.snap_top:
            if l <= ax <= r and t <= ay <= b:
                best = (l, t, r, b)
                barea = (r - l) * (b - t)
                for (cl, ct, cr, cb) in self._children_of(hwnd):
                    if cl <= ax <= cr and ct <= ay <= cb:
                        a = (cr - cl) * (cb - ct)
                        if 600 < a < barea:      # 更小的模块（且非极小控件）
                            best, barea = (cl, ct, cr, cb), a
                return (best[0] - self.vx, best[1] - self.vy,
                        best[2] - self.vx, best[3] - self.vy)
        return None

    def _update_snap(self, x, y):
        self.canvas.delete("snap")
        rc = self._snap_candidate(x, y)
        self._snap_rect = rc
        if rc is None:
            return
        l, t, r, b = rc
        self.canvas.create_rectangle(l, t, r, b, outline=ACCENT, width=2 * UI,
                                     tags="snap")
        self.canvas.tag_raise("snap")

    def _update_magnifier(self, x, y):
        self.canvas.delete("mag")
        Z, half = 8 * UI, 8                  # 放大倍数 / 采样半径（整体放大）
        n = 2 * half + 1
        D = n * Z
        reg = Image.new("RGB", (n, n), (20, 20, 20))
        cl, ct = max(0, x - half), max(0, y - half)
        cr, cb = min(self.vw, x - half + n), min(self.vh, y - half + n)
        if cr > cl and cb > ct:
            reg.paste(self.full_img.crop((cl, ct, cr, cb)),
                      (cl - (x - half), ct - (y - half)))
        disp = reg.resize((D, D), Image.NEAREST)
        d = ImageDraw.Draw(disp)
        mid = half * Z
        GREEN = (19, 192, 96)
        # 绿色十字虚线准星（与大多数背景色可区分）+ 中心像素高亮
        cxl = mid + Z // 2
        for a0 in range(0, D, 6):                      # 虚线
            d.line([cxl, a0, cxl, min(a0 + 3, D)], fill=GREEN, width=UI)
            d.line([a0, cxl, min(a0 + 3, D), cxl], fill=GREEN, width=UI)
        d.rectangle([mid, mid, mid + Z - 1, mid + Z - 1], outline=GREEN, width=2)
        d.rectangle([0, 0, D - 1, D - 1], outline=(255, 255, 255), width=2*UI)
        d.rectangle([UI, UI, D-1-UI, D-1-UI], outline=(40, 40, 40), width=1)
        # 信息面板（浅色底）：两行黑字色号（HEX / RGB）+ 一行灰字提示
        rgb = self._pick_color_at(x, y)
        hexs = "#%02X%02X%02X" % rgb
        self.pick_hex = hexs
        pdx = 8 * UI
        lh = 19 * UI
        panelH = int(3.4 * lh)
        full = Image.new("RGB", (D, D + panelH), (245, 246, 248))
        full.paste(disp, (0, 0))
        pd = ImageDraw.Draw(full)
        pd.rectangle([pdx, D + pdx, pdx + 18*UI, D + pdx + 18*UI], fill=rgb,
                     outline=(160, 160, 160))
        f1 = ui_font(14 * UI)
        f2 = ui_font(12 * UI)
        tx0 = pdx + 22 * UI
        pd.text((tx0, D + 6*UI), hexs, fill=(20, 20, 20), font=f1)
        pd.text((tx0, D + 6*UI + lh), "RGB %d, %d, %d" % rgb, fill=(20, 20, 20),
                font=f1)
        pd.text((tx0, D + 6*UI + 2*lh), "右键复制色值", fill=(140, 140, 140),
                font=f2)
        # 复用同一个 Tk 图像对象（paste 覆盖像素），避免每帧重新分配（关键提速）
        mp = getattr(self, "_mag_photo", None)
        if mp is not None and mp.width() == full.width \
                and mp.height() == full.height:
            mp.paste(full)
        else:
            self._mag_photo = ImageTk.PhotoImage(full)
        ox, oy = x + 20, y + 20
        if ox + D > self.vw:
            ox = x - 20 - D
        if oy + D + panelH > self.vh:
            oy = y - 20 - (D + panelH)
        item = getattr(self, "_mag_item", None)
        if item and item in self.canvas.find_withtag("mag"):
            self.canvas.coords(item, ox, oy)
            self.canvas.itemconfigure(item, image=self._mag_photo)
        else:
            self._mag_item = self.canvas.create_image(
                ox, oy, anchor="nw", image=self._mag_photo, tags="mag")
        self.canvas.tag_raise("mag")

    # ---------------- 悬停 ----------------
    def _do_sel_hover(self, x, y):
        try:
            self._update_snap(x, y)
        except Exception:
            pass
        self._update_magnifier(x, y)           # 取色放大镜始终显示

    def _flush_hover(self):
        self._hover_scheduled = False
        if self.mode == "select" and not self.start_pt and self._pending_hover:
            self._last_sel_hover = time.perf_counter()
            self._do_sel_hover(*self._pending_hover)

    def on_hover(self, e):
        if self.mode == "select" and not self.start_pt:
            # 节流到约 30fps：避免每个原始 Motion 事件都重建放大镜/枚举窗口
            now = time.perf_counter()
            self._pending_hover = (e.x, e.y)
            if now - getattr(self, "_last_sel_hover", 0.0) < 0.03:
                if not getattr(self, "_hover_scheduled", False):
                    self._hover_scheduled = True
                    self.root.after(25, self._flush_hover)
                return
            self._last_sel_hover = now
            self._do_sel_hover(e.x, e.y)
            return
        if self.mode != "edit" or self.drag_mode:
            return
        idx = self._extend_hover(e.x, e.y, self._anno_at(e.x, e.y))
        self.canvas.delete("editbtn")
        self.canvas.delete("brushcur")
        if idx is not None:
            self.hover_anno_idx = idx
            self.selected_idx = idx                     # 记住当前选中要素（可换色）
            self._draw_move_box(idx)                    # 四边虚线 + 节点方框
            if self.annotations[idx]["type"] == "text":
                self._draw_edit_handle(idx)
                # 悬停在铅笔按钮上 → 手型（可点击编辑）；否则 → 十字（可拖动）
                if self._in_pencil(e.x, e.y, idx):
                    self.overlay.configure(cursor="hand2")
                else:
                    self.overlay.configure(cursor="fleur")
            elif self.annotations[idx]["type"] == "arrow" \
                    and self._in_rotate_handle(e.x, e.y, idx):
                self.overlay.configure(cursor="exchange")   # 旋转
            elif self.annotations[idx]["type"] == "arrow" \
                    and self._in_arrow_dot(e.x, e.y, idx):
                self.overlay.configure(cursor="hand2")      # 弯折控制点
            else:
                self.overlay.configure(cursor="fleur")   # 十字四向：整体平移
            return
        self.hover_anno_idx = None
        self.canvas.delete("movebox")

        hit = self.hit_test(e.x, e.y)
        cur = {"nw": "size_nw_se", "se": "size_nw_se", "ne": "size_ne_sw",
               "sw": "size_ne_sw", "n": "size_ns", "s": "size_ns",
               "e": "size_we", "w": "size_we"}
        if hit[0] == "handle":
            self.overlay.configure(cursor=cur.get(hit[1], "left_ptr"))
        elif hit[0] == "border":
            self.overlay.configure(cursor="fleur")
        elif hit[0] == "inside":
            if self.tool == "mosaic":
                # 马赛克：隐藏十字光标，改用与形状一致的半透明灰色笔刷指示
                self.overlay.configure(cursor="none")
                self._draw_brush_cursor(e.x, e.y)
            else:
                self.overlay.configure(
                    cursor="xterm" if self.tool == "text"
                    else ("crosshair" if self.tool else "fleur"))
        else:
            self.overlay.configure(cursor="left_ptr")

    def on_rclick(self, e):
        # 取色阶段：右键取色 → 复制色值 + 退出工具 + 在光标处自动粘贴
        if self.mode == "select":
            self.pick_hex = "#%02X%02X%02X" % self._pick_color_at(e.x, e.y)
            if self._set_clipboard_text(self.pick_hex):
                self._finish_and_paste()
                self.flash_toast(f"已复制并粘贴色值 {self.pick_hex}")

    def on_double(self, e):
        if self.mode != "edit":
            return
        for i in range(len(self.annotations) - 1, -1, -1):
            a = self.annotations[i]
            if a["type"] == "text":
                bx0, by0, bx1, by1 = self._anno_bbox(a)
                if bx0 - 4 <= e.x <= bx1 + 4 and by0 - 4 <= e.y <= by1 + 4:
                    self.edit_text(i)
                    return

    # ---------------- 编辑：按下 ----------------
    def edit_press(self, e):
        # 正在输入文字时，点击框外仅提交/退出，不立刻新建文本（需再次点击才新建）
        if self.text_entry is not None:
            self.commit_text_entry()
            return
        idx = self._extend_hover(e.x, e.y, self._anno_at(e.x, e.y))
        if idx is None:
            self.selected_idx = None            # 点空白处取消选中
        else:
            self.selected_idx = idx
        # 1) 点击文字右上角铅笔 → 重新编辑
        if idx is not None and self._in_pencil(e.x, e.y, idx):
            self.edit_text(idx)
            return
        # 1.4) 点击箭头右上角旋转手柄 → 旋转整体方位
        if idx is not None and self._in_rotate_handle(e.x, e.y, idx):
            self._start_rotate_arrow(idx, e)
            return
        # 1.5) 点击箭头结构中心圆点 → 弯折箭头
        if idx is not None and self._in_arrow_dot(e.x, e.y, idx):
            self._start_bend_arrow(idx, e)
            return
        # 2) 选区缩放手柄
        hit = self.hit_test(e.x, e.y)
        if hit[0] == "handle":
            self.drag_mode = "resize"
            self.active_handle = hit[1]
            self._destroy_toolbars()
            return
        # 3) 悬停在任意元素上 → 拖动移动（任何工具下都可）
        if idx is not None:
            self._start_move_anno(idx, e)
            return
        # 4) 无工具：移动 / 缩放选区
        if self.tool is None:
            if hit[0] in ("inside", "border"):
                self.drag_mode = "move"
                self.start_pt = (e.x, e.y)
                self._destroy_toolbars()
            return
        # 5) 有工具
        if hit[0] == "border":
            self.drag_mode = "move"
            self.start_pt = (e.x, e.y)
            self._destroy_toolbars()
            return
        if hit[0] == "inside":
            if self.tool == "text":
                self.place_text_entry(e.x, e.y)
            else:
                self.drag_mode = "draw"
                x, y = self._clamp(e.x, e.y)
                self.start_pt = (x, y)
                self.temp_item = None
                if self.tool == "pen":
                    self.pen_points = [x, y]
                elif self.tool == "mosaic":
                    self._mosaic_stamps = []
                    l, t, r, b = self.sel
                    self._mos_off = (l, t)
                    self._mos_pix = self._pixelate(
                        self.full_img.crop((l, t, r, b)).convert("RGB"))
                    self._mos_work = self._flatten().convert("RGB")
                    self._mos_last = None
                    self._paint_mosaic_stamp(x, y)

    def _start_move_anno(self, idx, e):
        self.drag_mode = "moveanno"
        self.move_idx = idx
        self.start_pt = (e.x, e.y)
        # 背景 = 除该元素外的合成（拖动过程用较低超采样，更流畅）
        bg = self._flatten(ss=2, exclude=idx)
        self._show_crop(bg)
        self._draw_chrome()
        self._draw_one_vector(self.annotations[idx], "movetemp")
        self._draw_move_box(idx)

    def _start_bend_arrow(self, idx, e):
        self.drag_mode = "bendarrow"
        self.move_idx = idx
        self.start_pt = (e.x, e.y)
        bg = self._flatten(ss=2, exclude=idx)
        self._show_crop(bg)
        self._draw_chrome()
        self._draw_one_vector(self.annotations[idx], "movetemp")
        self._draw_move_box(idx)

    def _bend_arrow_to(self, idx, mx, my):
        """把曲线中点拖到 (mx,my)：反解二次贝塞尔控制点，使 B(0.5)=中点。"""
        a = self.annotations[idx]
        x0, y0, x1, y1 = a["coords"]
        a["ctrl"] = (2 * mx - 0.5 * (x0 + x1), 2 * my - 0.5 * (y0 + y1))

    def _start_rotate_arrow(self, idx, e):
        a = self.annotations[idx]
        x0, y0, x1, y1 = a["coords"]
        cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
        ctrl = a.get("ctrl") or (cx, cy)
        self.drag_mode = "rotatearrow"
        self.move_idx = idx
        self._rot_center = (cx, cy)
        self._rot_base = [(x0, y0), (x1, y1), ctrl]
        self._rot_start = math.atan2(e.y - cy, e.x - cx)
        bg = self._flatten(ss=2, exclude=idx)
        self._show_crop(bg)
        self._draw_chrome()
        self._show_shape_overlay(a, "movetemp")
        self._draw_move_box(idx)

    def _rotate_arrow_to(self, e):
        cx, cy = self._rot_center
        ang = math.atan2(e.y - cy, e.x - cx) - self._rot_start
        ca, sa = math.cos(ang), math.sin(ang)

        def rot(p):
            dx, dy = p[0] - cx, p[1] - cy
            return (cx + dx * ca - dy * sa, cy + dx * sa + dy * ca)

        p0, p1, pc = self._rot_base
        r0, r1, rc = rot(p0), rot(p1), rot(pc)
        a = self.annotations[self.move_idx]
        # 端点与控制点夹在选区框内，旋转不越界
        a["coords"] = self._clamp(*r0) + self._clamp(*r1)
        a["ctrl"] = self._clamp(*rc)

    def edit_drag(self, e):
        shift = bool(e.state & 0x0001)
        if self.drag_mode == "resize":
            self._do_resize(e.x, e.y)
        elif self.drag_mode == "move":
            self._do_move(e.x, e.y)
        elif self.drag_mode == "moveanno":
            dx, dy = e.x - self.start_pt[0], e.y - self.start_pt[1]
            self._move_anno(self.move_idx, dx, dy)
            self.start_pt = (e.x, e.y)
            self.canvas.delete("movetemp")
            # 拖动过程用轻量矢量预览（松手后 redraw_static 用 Pillow 精修）
            self._draw_one_vector(self.annotations[self.move_idx], "movetemp")
            self._draw_move_box(self.move_idx)
            self._draw_edit_handle(self.move_idx)
        elif self.drag_mode == "bendarrow":
            mx, my = self._clamp(e.x, e.y)
            self._bend_arrow_to(self.move_idx, mx, my)
            self.canvas.delete("movetemp")
            self._draw_one_vector(self.annotations[self.move_idx], "movetemp")
            self._draw_move_box(self.move_idx)
        elif self.drag_mode == "rotatearrow":
            self._rotate_arrow_to(e)
            self.canvas.delete("movetemp")
            self._draw_one_vector(self.annotations[self.move_idx], "movetemp")
            self._draw_move_box(self.move_idx)
        elif self.drag_mode == "draw":
            self._do_draw(e.x, e.y, shift)

    def edit_release(self, e):
        if self.drag_mode in ("resize", "move"):
            l, t, r, b = self.sel
            self.sel = [min(l, r), min(t, b), max(l, r), max(t, b)]
            self.drag_mode = None
            self.build_toolbar()
            self.redraw_static()
        elif self.drag_mode in ("moveanno", "bendarrow", "rotatearrow"):
            self.canvas.delete("movetemp")
            self.canvas.delete("movebox")
            self.drag_mode = None
            self.redraw_static()
            self._draw_edit_handle(self.move_idx)
        elif self.drag_mode == "draw":
            self._commit_draw(e.x, e.y, bool(e.state & 0x0001))
            self.drag_mode = None
        self.start_pt = None

    def _do_resize(self, x, y):
        x = min(max(x, 0), self.vw)
        y = min(max(y, 0), self.vh)
        l, t, r, b = self.sel
        h = self.active_handle
        if "w" in h:
            l = x
        if "e" in h:
            r = x
        if "n" in h:
            t = y
        if "s" in h:
            b = y
        self.sel = [l, t, r, b]
        self.redraw_live()

    def _do_move(self, x, y):
        if not self.start_pt:
            return
        dx, dy = x - self.start_pt[0], y - self.start_pt[1]
        l, t, r, b = self.sel
        w, hgt = r - l, b - t
        l = min(max(l + dx, 0), self.vw - w)
        t = min(max(t + dy, 0), self.vh - hgt)
        self.sel = [l, t, l + w, t + hgt]
        self.start_pt = (x, y)
        self.redraw_live()

    def _move_anno(self, idx, dx, dy):
        a = self.annotations[idx]
        # 限制平移量，使整个要素始终留在选区框内
        bx0, by0, bx1, by1 = self._anno_bbox(a)
        l, t, r, b = self.sel
        dx = max(l - bx0, min(dx, r - bx1))
        dy = max(t - by0, min(dy, b - by1))
        if a["type"] == "mosaic":
            a["stamps"] = [(sx + dx, sy + dy) for sx, sy in a["stamps"]]
        elif a["type"] == "pen":
            a["points"] = [c + (dx if i % 2 == 0 else dy)
                           for i, c in enumerate(a["points"])]
        elif a["type"] == "text":
            x, y = a["coords"]
            a["coords"] = (x + dx, y + dy)
        else:
            x0, y0, x1, y1 = a["coords"]
            a["coords"] = (x0 + dx, y0 + dy, x1 + dx, y1 + dy)
            if a.get("ctrl"):                 # 箭头弯折控制点随整体平移
                cx, cy = a["ctrl"]
                a["ctrl"] = (cx + dx, cy + dy)

    def _clamp(self, x, y):
        l, t, r, b = self.sel
        return min(max(x, l), r), min(max(y, t), b)

    # ---------------- 绘制新元素（拖动中用矢量临时预览）----------------
    def _draw_comet(self, coords, w, color, tag):
        for quad, col in comet_polys(*coords, w, color):
            self.canvas.create_polygon(quad, fill=col, outline=col, tags=tag)

    def _draw_arrow_vector(self, a, tag):
        """弯折箭头的轻量矢量预览：贝塞尔平滑折线 + 三角箭头（拖动时用，
        松手后由 redraw_static 精修成彗星渐变）。"""
        x0, y0, x1, y1 = a["coords"]
        cx, cy = a.get("ctrl") or ((x0 + x1) / 2.0, (y0 + y1) / 2.0)
        w, color = a["width"], a["color"]
        N = 18
        pts = []
        for i in range(N + 1):
            t = i / N
            mt = 1 - t
            pts.append(mt * mt * x0 + 2 * mt * t * cx + t * t * x1)
            pts.append(mt * mt * y0 + 2 * mt * t * cy + t * t * y1)
        self.canvas.create_line(*pts, fill=color, width=w, smooth=True,
                                capstyle=tk.ROUND, joinstyle=tk.ROUND, tags=tag)
        ex, ey, px, py = pts[-2], pts[-1], pts[-4], pts[-3]
        dx, dy = ex - px, ey - py
        L = math.hypot(dx, dy) or 1.0
        ux, uy = dx / L, dy / L
        pxp, pyp = -uy, ux
        hl, hw = 10 + 3 * w, 5 + 1.9 * w
        bx, by = ex - ux * hl, ey - uy * hl
        self.canvas.create_polygon(bx + pxp * hw, by + pyp * hw, ex, ey,
                                   bx - pxp * hw, by - pyp * hw,
                                   fill=color, outline=color, tags=tag)

    def _shape_photo(self, a):
        """把 rect/rrect/ellipse/arrow 渲染成抗锯齿小图（与最终合成完全一致），
        返回 (PhotoImage, 左上x, 左上y)。用于插入/拖动时的所见即最终所得预览。"""
        typ = a["type"]
        col = _hex(a["color"])
        w = a["width"]
        ss = 4
        if typ == "arrow":
            x0, y0, x1, y1 = a["coords"]
            ctrl = a.get("ctrl") or ((x0 + x1) / 2.0, (y0 + y1) / 2.0)
            img_ss, ax, ay = render_comet_curve((x0, y0), ctrl, (x1, y1),
                                                w, a["color"], ss)
            img = _smooth_resize(img_ss, (max(1, img_ss.width // ss),
                                          max(1, img_ss.height // ss)))
            return ImageTk.PhotoImage(img), ax, ay
        x0, y0, x1, y1 = a["coords"]
        lx, ty = min(x0, x1), min(y0, y1)
        rx, by = max(x0, x1), max(y0, y1)
        pad = int(w / 2) + 3
        W, H = int(rx - lx) + 2 * pad, int(by - ty) + 2 * pad
        im = Image.new("RGBA", (W * ss, H * ss), (255, 255, 255, 0))
        d = ImageDraw.Draw(im)
        half = w * ss / 2.0

        def L(vx, vy):
            return ((vx - lx + pad) * ss, (vy - ty + pad) * ss)

        aa, bb = L(lx, ty), L(rx, by)
        ex = [aa[0] - half, aa[1] - half, bb[0] + half, bb[1] + half]
        if typ == "rect":
            d.rectangle(ex, outline=col, width=int(w * ss))
        elif typ == "rrect":
            rad = self._rrect_radius(lx, ty, rx, by) * ss
            d.rounded_rectangle(ex, radius=rad, outline=col, width=int(w * ss))
        elif typ == "ellipse":
            d.ellipse(ex, outline=col, width=int(w * ss))
        im = _smooth_resize(im, (W, H))
        return ImageTk.PhotoImage(im), int(lx - pad), int(ty - pad)

    def _show_shape_overlay(self, a, tag):
        """在 canvas 上以抗锯齿小图覆盖显示一个形状要素（alpha 叠加于底图）。
        用单一引用而非累积列表，避免拖动时内存与卡顿累积。"""
        self.canvas.delete(tag)
        photo, ox, oy = self._shape_photo(a)
        self._ov_photo = photo                       # 仅保留最新一张的引用
        self.canvas.create_image(ox, oy, anchor="nw", image=photo, tags=tag)

    def _canvas_rrect(self, x0, y0, x1, y1, color, w, tag, r=None):
        """在 canvas 上画圆角矩形轮廓（4 段圆角 + 4 条边），用于实时预览。"""
        l, t = min(x0, x1), min(y0, y1)
        rr, bb = max(x0, x1), max(y0, y1)
        if r is None:
            r = self._rrect_radius(l, t, rr, bb)
        d = r * 2
        opt = dict(outline=color, width=w, style=tk.ARC, tags=tag)
        self.canvas.create_arc(l, t, l + d, t + d, start=90, extent=90, **opt)
        self.canvas.create_arc(rr - d, t, rr, t + d, start=0, extent=90, **opt)
        self.canvas.create_arc(l, bb - d, l + d, bb, start=180, extent=90, **opt)
        self.canvas.create_arc(rr - d, bb - d, rr, bb, start=270, extent=90, **opt)
        for pts in [(l + r, t, rr - r, t), (l + r, bb, rr - r, bb),
                    (l, t + r, l, bb - r), (rr, t + r, rr, bb - r)]:
            self.canvas.create_line(*pts, fill=color, width=w, tags=tag)

    def _mosaic_brush(self):
        return MOSAIC_BRUSH[self.level]

    def _draw_brush_cursor(self, x, y):
        """马赛克笔刷指示：真 alpha 渲染，圆/方的透明度与颜色完全一致。"""
        self.canvas.delete("brushcur")
        d = max(2, int(self._mosaic_brush()))
        ss = 3
        S = d * ss
        im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        dr = ImageDraw.Draw(im)
        fill = (150, 156, 163, 90)          # 半透明灰（圆/方共用）
        outline = (90, 95, 100, 170)
        box = [ss, ss, S - ss, S - ss]
        if self.mosaic_shape == "circle":
            dr.ellipse(box, fill=fill, outline=outline, width=ss)
        else:
            dr.rectangle(box, fill=fill, outline=outline, width=ss)
        self._brush_photo = ImageTk.PhotoImage(_smooth_resize(im, (d, d)))
        self.canvas.create_image(int(x - d / 2), int(y - d / 2), anchor="nw",
                                 image=self._brush_photo, tags="brushcur")
        self.canvas.tag_raise("brushcur")

    def _stamp_mosaic_tile(self, x, y):
        """把一枚笔刷贴进工作图（不刷新显示）。"""
        l, t = self._mos_off
        cx, cy = x - l, y - t
        rad = self._mosaic_brush() / 2.0
        W, H = self._mos_work.size
        bx0, by0 = int(max(0, cx - rad)), int(max(0, cy - rad))
        bx1, by1 = int(min(W, cx + rad + 1)), int(min(H, cy + rad + 1))
        if bx1 <= bx0 or by1 <= by0:
            return
        tile_mask = Image.new("L", (bx1 - bx0, by1 - by0), 0)
        md = ImageDraw.Draw(tile_mask)
        bb = [cx - rad - bx0, cy - rad - by0, cx + rad - bx0, cy + rad - by0]
        (md.ellipse if self.mosaic_shape == "circle" else md.rectangle)(
            bb, fill=255)
        self._mos_work.paste(self._mos_pix.crop((bx0, by0, bx1, by1)),
                             (bx0, by0), tile_mask)

    def _paint_mosaic_stamp(self, x, y):
        """所见即所得：从上一点到当前点插值连续贴印（避免快速滑动出现断点），
        全部贴完后统一刷新显示。"""
        last = getattr(self, "_mos_last", None)
        if last is None:
            self._stamp_mosaic_tile(x, y)
        else:
            lx, ly = last
            dist = math.hypot(x - lx, y - ly)
            step = max(1.0, self._mosaic_brush() * 0.35)   # 间距≤笔刷，保证连续
            n = max(1, int(dist / step))
            for i in range(1, n + 1):
                px = lx + (x - lx) * i / n
                py = ly + (y - ly) * i / n
                self._stamp_mosaic_tile(px, py)
                self._mosaic_stamps.append((px, py))
        if last is None:
            self._mosaic_stamps.append((x, y))
        self._mos_last = (x, y)
        self._show_crop(self._mos_work)
        self._draw_chrome()
        self._draw_brush_cursor(x, y)                  # 笔刷指示随涂抹跟随

    def _draw_mosaic_preview(self, stamps, brush, shape, tag):
        """马赛克占位预览（仅用于选区缩放等瞬态；正式合成在 redraw_static）。"""
        self.canvas.delete(tag)
        r = brush / 2.0
        for sx, sy in stamps:
            if shape == "circle":
                self.canvas.create_oval(sx - r, sy - r, sx + r, sy + r,
                                        fill="#9098A0", outline="",
                                        stipple="gray50", tags=tag)
            else:
                self.canvas.create_rectangle(sx - r, sy - r, sx + r, sy + r,
                                             fill="#9098A0", outline="",
                                             stipple="gray50", tags=tag)

    def _square(self, x0, y0, x, y):
        """按住 Shift 时约束为正方形（正圆 / 正方 / 正圆角方框）。"""
        s = max(abs(x - x0), abs(y - y0))
        return (x0 + (s if x >= x0 else -s), y0 + (s if y >= y0 else -s))

    def _do_draw(self, x, y, shift=False):
        x, y = self._clamp(x, y)
        x0, y0 = self.start_pt
        if shift and self.tool in ("rect", "rrect", "ellipse"):
            x, y = self._square(x0, y0, x, y)
            x, y = self._clamp(x, y)
        if self.temp_item:
            self.canvas.delete(self.temp_item)
            self.temp_item = None
        self.canvas.delete("drawtemp")
        w = self.width
        # 拖动绘制用轻量 tkinter 矢量预览（松手 redraw_static 用 Pillow 精修成最终效果）
        if self.tool == "rect":
            self.temp_item = self.canvas.create_rectangle(
                x0, y0, x, y, outline=self.color, width=w)
        elif self.tool == "rrect":
            self._canvas_rrect(x0, y0, x, y, self.color, w, "drawtemp")
        elif self.tool == "ellipse":
            self.temp_item = self.canvas.create_oval(
                x0, y0, x, y, outline=self.color, width=w)
        elif self.tool == "arrow":
            self._draw_comet((x0, y0, x, y), w, self.color, "drawtemp")
        elif self.tool == "mosaic":
            self._paint_mosaic_stamp(x, y)
        elif self.tool == "pen":
            self.pen_points.extend([x, y])
            self.temp_item = self.canvas.create_line(
                *self.pen_points, fill=self.color, width=w,
                capstyle=tk.ROUND, joinstyle=tk.ROUND, smooth=True)

    def _commit_draw(self, x, y, shift=False):
        x, y = self._clamp(x, y)
        x0, y0 = self.start_pt
        if shift and self.tool in ("rect", "rrect", "ellipse"):
            x, y = self._square(x0, y0, x, y)
            x, y = self._clamp(x, y)
        a = None
        if self.tool == "rect" and abs(x - x0) > 2 and abs(y - y0) > 2:
            a = {"type": "rect", "coords": (x0, y0, x, y),
                 "color": self.color, "width": self.width}
        elif self.tool == "rrect" and abs(x - x0) > 2 and abs(y - y0) > 2:
            a = {"type": "rrect", "coords": (x0, y0, x, y),
                 "color": self.color, "width": self.width}
        elif self.tool == "ellipse" and abs(x - x0) > 2 and abs(y - y0) > 2:
            a = {"type": "ellipse", "coords": (x0, y0, x, y),
                 "color": self.color, "width": self.width}
        elif self.tool == "arrow" and (abs(x - x0) > 3 or abs(y - y0) > 3):
            a = {"type": "arrow", "coords": (x0, y0, x, y),
                 "color": self.color, "width": self.width}
        elif self.tool == "mosaic" and getattr(self, "_mosaic_stamps", None):
            a = {"type": "mosaic", "stamps": list(self._mosaic_stamps),
                 "shape": self.mosaic_shape, "brush": self._mosaic_brush()}
            self._mosaic_stamps = None
            self._mos_last = None
        elif self.tool == "pen" and self.pen_points and len(self.pen_points) >= 4:
            a = {"type": "pen", "points": list(self.pen_points),
                 "color": self.color, "width": self.width}
        if self.temp_item:
            self.canvas.delete(self.temp_item)
            self.temp_item = None
        self.canvas.delete("drawtemp")
        self.pen_points = None
        if a:
            self.annotations.append(a)
            self.selected_idx = len(self.annotations) - 1   # 新建后即为选中
            self.redraw_static()

    # ---------------- 文字 ----------------
    def place_text_entry(self, x, y, text="", color=None, fs=None, font=None):
        self.commit_text_entry()
        x, y = self._clamp(x, y)
        color = color or self.color
        fs = fs or self.font_size
        font = font or self.font_path
        txt = tk.Text(self.overlay, bd=0, bg="#FFFFFF", fg=color,
                      insertbackground=color, font=(family_of(font), -fs),
                      wrap="none", highlightthickness=0, padx=0, pady=0)
        if text:
            txt.insert("1.0", text)
        _w0, _h0, lineh, _asc = measure_text(text or " ", fs, font)
        # 初始宽度与光标高度一致（正方形起始框），随输入扩充
        ew = self.canvas.create_window(x, y, anchor="nw", window=txt,
                                       width=lineh + 2, height=lineh + 2)
        txt.focus_set()
        txt.mark_set("insert", "end")
        self.text_entry = (txt, x, y, ew, color, fs, font)
        # 回车提交；Shift+回车换行
        txt.bind("<Return>", self._text_commit_key)
        txt.bind("<Shift-Return>", self._text_newline_key)
        txt.bind("<Escape>", lambda e: self.cancel_text_entry())
        txt.bind("<KeyRelease>", lambda e: self._update_text_dashbox())
        self.overlay.after(10, self._update_text_dashbox)

    def _text_commit_key(self, e):
        self.commit_text_entry()
        return "break"

    def _text_newline_key(self, e):
        e.widget.insert("insert", "\n")
        self._update_text_dashbox()
        return "break"

    def _update_text_dashbox(self):
        """输入过程中始终显示虚线定位框；默认 2 个中文字宽，随输入扩充。"""
        if not self.text_entry:
            return
        txt, x, y, ew, color, fs, font = self.text_entry
        content = txt.get("1.0", "end-1c")
        w, h, lineh, _asc = measure_text(content if content else " ", fs, font)
        ww = max(lineh, w) + 4          # 初始≈光标高度的方框，随内容加宽
        hh = lineh * (content.count("\n") + 1) + 2
        self.canvas.itemconfigure(ew, width=ww, height=hh)
        self.canvas.delete("textbox")
        self.canvas.create_rectangle(x - 3, y - 2, x + ww + 3, y + hh + 2,
                                     outline=ACCENT, width=2, dash=(16, 5, 2, 5),
                                     tags="textbox")

    def edit_text(self, idx):
        a = self.annotations.pop(idx)
        self.redraw_static()
        x, y = a["coords"]
        self.place_text_entry(x, y, a["text"], a["color"], a["font_size"],
                              a.get("font", DEFAULT_FONT))

    def commit_text_entry(self):
        if not self.text_entry:
            return
        txt, x, y, ew, color, fs, font = self.text_entry
        text = txt.get("1.0", "end-1c")
        self.canvas.delete(ew)
        self.canvas.delete("textbox")
        txt.destroy()
        self.text_entry = None
        if text.strip():
            self.annotations.append({"type": "text", "coords": (x, y),
                                     "text": text, "color": color,
                                     "font_size": fs, "font": font})
            self.selected_idx = len(self.annotations) - 1
            self.redraw_static()

    def cancel_text_entry(self):
        if not self.text_entry:
            return
        txt, x, y, ew, color, fs, font = self.text_entry
        self.canvas.delete(ew)
        self.canvas.delete("textbox")
        txt.destroy()
        self.text_entry = None

    # ---------------- 抗锯齿合成（核心）----------------
    def _flatten(self, ss=None, exclude=None):
        l, t, r, b = self.sel
        base = self.full_img.crop((l, t, r, b)).convert("RGB")
        W, H = base.size
        if ss is None:
            area = W * H
            ss = 4 if area <= 900000 else (3 if area <= 2500000 else 2)
        ox, oy = l, t
        items = [a for i, a in enumerate(self.annotations) if i != exclude]

        # 1) 马赛克（先铺到底图）：按笔刷方块/圆形形状遮罩粘贴像素化结果
        mos = [a for a in items if a["type"] == "mosaic"]
        if mos:
            pix = self._pixelate(base)
            mask = Image.new("L", (W, H), 0)
            md = ImageDraw.Draw(mask)
            for a in mos:
                if "stamps" in a:                      # 笔刷形式
                    rad = a.get("brush", 20) / 2.0
                    circle = a.get("shape") == "circle"
                    for sx, sy in a["stamps"]:
                        cx, cy = sx - ox, sy - oy
                        bb = [cx - rad, cy - rad, cx + rad, cy + rad]
                        (md.ellipse if circle else md.rectangle)(bb, fill=255)
                else:                                   # 兼容旧矩形格式
                    x0, y0, x1, y1 = a["coords"]
                    md.rectangle([x0 - ox, y0 - oy, x1 - ox, y1 - oy], fill=255)
            base.paste(pix, (0, 0), mask)

        # 2) 矢量图形（超采样后缩小，抗锯齿）
        ov = Image.new("RGBA", (W * ss, H * ss), (0, 0, 0, 0))
        d = ImageDraw.Draw(ov)

        def P(x, y):
            return ((x - ox) * ss, (y - oy) * ss)

        has_vec = False
        for a in items:
            typ = a["type"]
            if typ == "text" or typ == "mosaic":
                continue
            has_vec = True
            col = _hex(a["color"])
            wpx = a["width"] * ss
            half = wpx / 2.0
            if typ == "rect":
                x0, y0, x1, y1 = a["coords"]
                aa = P(min(x0, x1), min(y0, y1))
                bb = P(max(x0, x1), max(y0, y1))
                d.rectangle([aa[0] - half, aa[1] - half,
                             bb[0] + half, bb[1] + half],
                            outline=col, width=int(wpx))
            elif typ == "rrect":
                x0, y0, x1, y1 = a["coords"]
                lx, ty = min(x0, x1), min(y0, y1)
                rx, by = max(x0, x1), max(y0, y1)
                rad = self._rrect_radius(lx, ty, rx, by) * ss
                aa, bb = P(lx, ty), P(rx, by)
                d.rounded_rectangle([aa[0] - half, aa[1] - half,
                                     bb[0] + half, bb[1] + half],
                                    radius=rad, outline=col, width=int(wpx))
            elif typ == "ellipse":
                x0, y0, x1, y1 = a["coords"]
                aa = P(min(x0, x1), min(y0, y1))
                bb = P(max(x0, x1), max(y0, y1))
                d.ellipse([aa[0] - half, aa[1] - half,
                           bb[0] + half, bb[1] + half],
                          outline=col, width=int(wpx))
            elif typ == "arrow":
                x0, y0, x1, y1 = a["coords"]
                ctrl = a.get("ctrl") or ((x0 + x1) / 2.0, (y0 + y1) / 2.0)
                img_ss, ax, ay = render_comet_curve((x0, y0), ctrl, (x1, y1),
                                                    a["width"], a["color"], ss)
                ov.alpha_composite(img_ss, (int((ax - ox) * ss),
                                            int((ay - oy) * ss)))
            elif typ == "pen":
                pts = [P(a["points"][i], a["points"][i + 1])
                       for i in range(0, len(a["points"]), 2)]
                if len(pts) >= 2:
                    d.line(pts, fill=col, width=int(wpx), joint="curve")
                    rr = wpx / 2
                    for x, y in pts:
                        d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=col)
        result = base.convert("RGBA")
        if has_vec:
            result = Image.alpha_composite(result, _smooth_resize(ov, (W, H)))
        # 3) 文字（1x 直接绘制，中英混排：齐伋体 + California FB，字体自带抗锯齿）
        dt = ImageDraw.Draw(result)
        for a in items:
            if a["type"] != "text":
                continue
            x, y = a["coords"]
            draw_text_multi(dt, x - ox, y - oy, a["text"], a["color"],
                            a["font_size"], a.get("font", DEFAULT_FONT))
        return result.convert("RGB")

    @staticmethod
    def _pixelate(region, block=15):
        w, h = region.size
        if w < 1 or h < 1:
            return region
        # 用 BOX 平均下采样（每个色块=区域平均色，色彩更多元、打码更明显），
        # 再用 NEAREST 放大成马赛克方格。
        small = region.resize((max(1, w // block), max(1, h // block)),
                              Image.BOX)
        return small.resize((w, h), Image.NEAREST)

    # ---------------- 显示 ----------------
    def _show_crop(self, pil_img):
        l, t, r, b = self.sel
        cp = getattr(self, "_crop_photo", None)
        if cp is not None and cp.width() == pil_img.width \
                and cp.height() == pil_img.height:
            cp.paste(pil_img)            # 复用同尺寸 Tk 图像，避免反复分配
        else:
            self._crop_photo = ImageTk.PhotoImage(pil_img)
        self.canvas.delete("crop")
        self.canvas.delete("anno")
        self.canvas.create_image(l, t, anchor="nw", image=self._crop_photo,
                                 tags="crop")

    def _draw_chrome(self):
        l, t, r, b = self.sel
        self.canvas.delete("sel")
        self.canvas.create_rectangle(l, t, r, b, outline=ACCENT, width=2,
                                     tags="sel")
        hs = 4 * UI
        for hx, hy in self.handle_points().values():
            self.canvas.create_rectangle(hx - hs, hy - hs, hx + hs, hy + hs,
                                         fill="white", outline=ACCENT,
                                         width=2, tags="sel")
        self._size_label(l, t, r, b, "sel")
        self.canvas.tag_raise("sel")
        self.canvas.tag_raise("editbtn")
        self.canvas.tag_raise("toolbar")

    def redraw_static(self):
        if not self.sel:
            return
        self._show_crop(self._flatten())
        self._draw_chrome()

    def redraw_live(self):
        """缩放/移动选区时的快速矢量预览。"""
        l, t, r, b = self.sel
        crop = self.full_img.crop((l, t, r, b))
        self._crop_photo = ImageTk.PhotoImage(crop)
        self.canvas.delete("crop")
        self.canvas.delete("anno")
        self.canvas.create_image(l, t, anchor="nw", image=self._crop_photo,
                                 tags="crop")
        for a in self.annotations:
            self._draw_one_vector(a, "anno")
        self._draw_chrome()

    def _draw_one_vector(self, a, tag):
        t = a["type"]
        if t == "rect":
            self.canvas.create_rectangle(*a["coords"], outline=a["color"],
                                         width=a["width"], tags=tag)
        elif t == "rrect":
            self._canvas_rrect(*a["coords"], a["color"], a["width"], tag)
        elif t == "ellipse":
            self.canvas.create_oval(*a["coords"], outline=a["color"],
                                    width=a["width"], tags=tag)
        elif t == "arrow":
            if a.get("ctrl"):                       # 弯折箭头：矢量曲线预览
                self._draw_arrow_vector(a, tag)
            else:
                self._draw_comet(a["coords"], a["width"], a["color"], tag)
        elif t == "pen":
            self.canvas.create_line(*a["points"], fill=a["color"],
                                    width=a["width"], capstyle=tk.ROUND,
                                    joinstyle=tk.ROUND, smooth=True, tags=tag)
        elif t == "mosaic":
            # 实时预览用灰色占位（真实马赛克在 redraw_static 中合成）
            if "stamps" in a:
                self._draw_mosaic_preview(a["stamps"], a.get("brush", 20),
                                          a.get("shape", "rect"), tag)
            else:
                x0, y0, x1, y1 = a["coords"]
                reg = self.full_img.crop((x0, y0, x1, y1))
                photo = ImageTk.PhotoImage(self._pixelate(reg))
                self._imgrefs.append(photo)
                self.canvas.create_image(x0, y0, anchor="nw", image=photo,
                                         tags=tag)
        elif t == "text":
            x, y = a["coords"]
            self.canvas.create_text(x, y, anchor="nw", text=a["text"],
                                    fill=a["color"],
                                    font=(family_of(a.get("font", DEFAULT_FONT)),
                                          -a["font_size"]),
                                    tags=tag)

    def _size_label(self, l, t, r, b, tag):
        text = f"{r - l} × {b - t}"
        ly = t - 14 * UI if t > 30 else t + 8
        self.canvas.create_text(l + 1, ly + 1, anchor="nw", text=text,
                                fill="#000000", font=(CN_FONT_FAMILY, -13 * UI),
                                tags=tag)
        self.canvas.create_text(l, ly, anchor="nw", text=text, fill="#FFFFFF",
                                font=(CN_FONT_FAMILY, -13 * UI), tags=tag)

    def delete_selected(self):
        """Delete 键：删除当前选中的要素。"""
        if self.mode != "edit" or self.text_entry:
            return
        si = self.selected_idx
        if si is not None and 0 <= si < len(self.annotations):
            del self.annotations[si]
            self.selected_idx = None
            self.hover_anno_idx = None
            self.canvas.delete("editbtn")
            self.canvas.delete("movebox")
            self.redraw_static()

    def undo(self):
        if self.text_entry:
            self.cancel_text_entry()
            return
        if self.annotations:
            self.annotations.pop()
            self.hover_anno_idx = None
            self.selected_idx = None
            self.canvas.delete("editbtn")
            self.canvas.delete("movebox")
            self.redraw_static()

    # ---------------- 工具栏 ----------------
    def _destroy_toolbars(self):
        for w in getattr(self, "_tb_widgets", []):
            try:
                w.destroy()
            except Exception:
                pass
        self._tb_widgets = []
        self.canvas.delete("toolbar")

    def build_toolbar(self):
        self._destroy_toolbars()
        self.tool_buttons = {}
        layout = [("tool", "rect"), ("tool", "rrect"), ("tool", "ellipse"),
                  ("tool", "arrow"), ("tool", "mosaic"), ("tool", "text"),
                  ("sep",),
                  ("act", "undo"), ("act", "save"),
                  ("sep",),
                  ("act", "close"), ("act", "confirm")]

        # 加大按钮间距使整体长度约再扩 0.5 倍
        btn, gap, sep_w, pad = 32*UI, 18*UI, 26*UI, 16*UI
        width = pad
        seps = []
        positions = []
        for it in layout:
            if it[0] == "sep":
                seps.append(width + sep_w // 2)
                width += sep_w
            else:
                positions.append((it[0], it[1], width))
                width += btn + gap
        width += pad - gap
        height = btn + pad * 2

        l, t, r, b = self.sel
        # 工具栏默认放在选区「右下方」（下方需容纳工具栏+子栏）
        tx = min(max(r - width, 8), self.vw - width - 8)
        ty = b + 14
        if ty + height + 56*UI > self.vh - 8:   # 下方放不下 → 放到上方
            ty = t - height - 14
            if ty < 8:                          # 上方也放不下 → 夹在可视范围
                ty = max(8, min(b + 14, self.vh - height - 8))
        self._tb_pos = (tx, ty, width, height)

        pill, M = render_pill(width, height, seps, radius=16 * UI)  # 圆角更小
        self._imgrefs.append(pill)
        self.canvas.create_image(tx - M, ty - M, anchor="nw", image=pill,
                                 tags="toolbar")

        cy = ty + height // 2
        for kind, name, rx in positions:
            self._make_button(kind, name, tx + rx, cy - btn // 2, btn)

        # 二级菜单（子栏）默认不出现，只有选中某个工具时才显示
        if self.tool:
            self._build_subbar(tx, ty, width, height)
        if self.tool:
            self._highlight_tool()

    def _make_button(self, kind, name, x, y, size):
        lbl = tk.Label(self.canvas, image=render_icon(name, size, "normal"),
                       bg=PILL_BG, bd=0, cursor="hand2")
        lbl._name = name
        lbl._active = False

        def show(state):
            lbl.configure(image=render_icon(name, size, state))

        def on_click(_e=None):
            if kind == "tool":
                self.set_tool(name)
            else:
                {"undo": self.undo, "save": self.do_save,
                 "close": self.cancel,
                 "confirm": lambda: self.do_copy(paste=True)}[name]()

        def on_enter(_e):
            show("active" if lbl._active else "hover")
            self._show_tooltip(name, x + size, y + size)   # 图标右下角

        def on_leave(_e):
            show("active" if lbl._active else "normal")
            self.canvas.delete("tooltip")

        lbl.bind("<Button-1>", on_click)
        lbl.bind("<Enter>", on_enter)
        lbl.bind("<Leave>", on_leave)
        lbl._show = show
        self.canvas.create_window(x, y, anchor="nw", window=lbl, tags="toolbar")
        self._tb_widgets.append(lbl)
        if kind == "tool":
            self.tool_buttons[name] = lbl

    def _show_tooltip(self, name, corner_x, corner_y):
        """一级菜单悬停提示：深灰色文字 + 透明背景，出现在图标右下角。"""
        self.canvas.delete("tooltip")
        label = TOOL_NAMES.get(name, name)
        fsz = -12 * UI
        f = tkfont.Font(family=CN_FONT_FAMILY, size=fsz)
        tw = f.measure(label)
        x0 = min(corner_x - 2 * UI, self.vw - tw - 6)
        y0 = min(corner_y, self.vh - (-fsz) - 6)
        self.canvas.create_text(x0, y0, anchor="nw", text=label,
                                fill="#3A3F44", font=(CN_FONT_FAMILY, fsz),
                                tags="tooltip")
        self.canvas.tag_raise("tooltip")

    def _highlight_tool(self):
        for name, lbl in self.tool_buttons.items():
            lbl._active = (name == self.tool)
            lbl._show("active" if lbl._active else "normal")

    def set_tool(self, name):
        self.tool = None if self.tool == name else name
        # 重建工具栏，使子栏在「颜色」与「马赛克形状」之间切换
        self.build_toolbar()

    def _build_subbar(self, tx, ty, width, height):
        step = 28 * UI            # 每个圆点/色块的水平步距
        half = step // 2
        pad = 12 * UI
        sep = 14 * UI
        shp_step = 32 * UI
        sh = 40 * UI
        sy = ty + height + 10
        if sy + sh > self.vh - 8:
            sy = ty - sh - 10

        n_level = len(LEVELS)
        mosaic = (self.tool == "mosaic")
        is_text = (self.tool == "text")
        font_w = 150 * UI if is_text else 0   # 文字工具额外的字体下拉宽度
        if mosaic:
            cw = pad + n_level*step + sep + 2*shp_step + pad
        else:
            n_color = len(PRESET_COLORS)
            cw = pad + n_level*step + sep + (n_color+1)*step + font_w + pad
        sx = min(max(tx, 8), self.vw - cw - 8)

        seps = [pad + n_level*step + sep//2]
        pill, M = render_pill(cw, sh, seps, radius=14 * UI)
        self._imgrefs.append(pill)
        self.canvas.create_image(sx - M, sy - M, anchor="nw", image=pill,
                                 tags="toolbar")

        cy = sy + sh // 2
        x = sx + pad
        self.level_buttons = []
        for i in range(n_level):
            lbl = tk.Label(self.canvas, bg=PILL_BG, bd=0, cursor="hand2")
            lbl.bind("<Button-1>", lambda e, k=i: self.set_level(k))
            self.canvas.create_window(x + half, cy, window=lbl, tags="toolbar")
            self.level_buttons.append(lbl)
            self._tb_widgets.append(lbl)
            x += step
        x += sep

        if mosaic:
            self.shape_buttons = {}
            for shp, icon in [("rect", "rect"), ("circle", "ellipse")]:
                lbl = tk.Label(self.canvas, bg=PILL_BG, bd=0, cursor="hand2")
                lbl.bind("<Button-1>", lambda e, s=shp: self.set_mosaic_shape(s))
                self.canvas.create_window(x + shp_step//2, cy, window=lbl,
                                          tags="toolbar")
                self.shape_buttons[shp] = (lbl, icon)
                self._tb_widgets.append(lbl)
                x += shp_step
            self.set_mosaic_shape(self.mosaic_shape)
        else:
            self.color_swatches = {}
            for col in PRESET_COLORS:
                lbl = tk.Label(self.canvas, bg=PILL_BG, bd=0, cursor="hand2")
                lbl.bind("<Button-1>", lambda e, c=col: self.set_color(c))
                self.canvas.create_window(x + half, cy, window=lbl, tags="toolbar")
                self.color_swatches[col] = lbl
                self._tb_widgets.append(lbl)
                x += step
            more = tk.Label(self.canvas, image=render_plus(24*UI), bg=PILL_BG,
                            bd=0, cursor="hand2")
            more._ref = more.cget("image")
            more.bind("<Button-1>", lambda e: self.pick_color())
            self.canvas.create_window(x + half, cy, window=more, tags="toolbar")
            self._tb_widgets.append(more)
            x += step
            self.set_color(self.color)
            if is_text:
                x += 6 * UI
                self._build_font_dropdown(x, cy)

        self.set_level(self.level)

    def _build_font_dropdown(self, x, cy):
        """文字工具的字体下拉：显示当前字体名，点击弹出内置字体列表选择。"""
        mb = tk.Menubutton(self.canvas, text=" " + name_of(self.font_path) + " ▾",
                           bg=PILL_BG, fg="#41464B", bd=0, relief="flat",
                           font=(CN_FONT_FAMILY, -14*UI), cursor="hand2",
                           activebackground="#EEF0F2", anchor="w",
                           justify="left", direction="below")
        menu = tk.Menu(mb, tearoff=0, font=(CN_FONT_FAMILY, -12*UI))
        for name, path, fam in FONTS:
            menu.add_command(label=name, columnbreak=0, hidemargin=1,
                             font=(fam, -16*UI), compound="left",
                             command=lambda p=path: self.set_font(p))
        mb.configure(menu=menu)
        self.font_menubtn = mb
        self.canvas.create_window(x, cy, anchor="w", window=mb, tags="toolbar")
        self._tb_widgets.append(mb)

    def set_font(self, path):
        self.font_path = path
        if hasattr(self, "font_menubtn"):
            try:
                self.font_menubtn.configure(text="  " + name_of(path) + " ▾")
            except Exception:
                pass
        # 若选中的是文字要素 → 直接换字体
        si = self.selected_idx
        if si is not None and 0 <= si < len(self.annotations) \
                and self.annotations[si].get("type") == "text":
            self.annotations[si]["font"] = path
            self.redraw_static()
            self._draw_move_box(si)
            self._draw_edit_handle(si)
        # 若正在输入 → 实时切换输入框字体
        if self.text_entry:
            txt = self.text_entry[0]
            fs = self.text_entry[5]
            try:
                txt.configure(font=(family_of(path), -fs))
            except Exception:
                pass
            self.text_entry = self.text_entry[:6] + (path,)
            self._update_text_dashbox()

    def set_mosaic_shape(self, shape):
        self.mosaic_shape = shape
        if not hasattr(self, "shape_buttons"):
            return
        for shp, (lbl, icon) in self.shape_buttons.items():
            img = render_icon(icon, 28*UI, "active" if shp == shape else "normal")
            lbl._ref = img
            lbl.configure(image=img)

    def set_level(self, k):
        self.level = k
        self.width, self.font_size = LEVELS[k]
        for i, lbl in enumerate(self.level_buttons):
            img = render_level(i, i == k, 28*UI)
            lbl._ref = img
            lbl.configure(image=img)

    def set_color(self, col):
        self.color = col
        # 若有选中的带颜色要素 → 直接替换其颜色（所见即所得）
        si = self.selected_idx
        if si is not None and 0 <= si < len(self.annotations) \
                and "color" in self.annotations[si]:
            self.annotations[si]["color"] = col
            self.redraw_static()
            self._draw_move_box(si)
            if self.annotations[si]["type"] == "text":
                self._draw_edit_handle(si)
        if not hasattr(self, "color_swatches"):
            return
        for c, lbl in self.color_swatches.items():
            img = render_swatch(c, c == col, 24*UI)
            lbl._ref = img
            lbl.configure(image=img)

    def pick_color(self):
        _, hexc = colorchooser.askcolor(color=self.color, parent=self.overlay)
        if hexc:
            self.set_color(hexc)

    # ---------------- 动作 ----------------
    def _set_clipboard_text(self, text):
        if text_to_clipboard(text):
            return True
        try:                                    # 兜底：tk 剪贴板
            self.root.clipboard_clear()
            self.root.clipboard_append(text)
            self.root.update()
            return True
        except Exception:
            return False

    def _finish_and_paste(self):
        """结束工具并在焦点处自动粘贴剪贴板内容（Ctrl+V）。"""
        self.finish()
        self.root.after(200, send_ctrl_v)

    def do_copy(self, paste=False):
        # 选区阶段：Ctrl+C 复制当前放大镜取到的色号
        if self.mode == "select":
            if self.pick_hex and self._set_clipboard_text(self.pick_hex):
                self.flash_toast(f"已复制色值 {self.pick_hex}")
            return
        if self.mode != "edit":
            return
        img = self._final()
        ok = image_to_clipboard(img)
        if paste and ok:
            self._finish_and_paste()
            self.flash_toast("已复制并粘贴到光标处 ✓")
        else:
            self.finish()
            if ok:
                self.flash_toast("已复制到剪贴板 ✓")

    def do_save(self):
        if self.mode != "edit":
            return
        img = self._final()
        default = "截图_" + datetime.now().strftime("%Y%m%d_%H%M%S") + ".png"
        self.overlay.attributes("-topmost", False)
        self.overlay.withdraw()
        path = filedialog.asksaveasfilename(
            parent=self.root, defaultextension=".png", initialfile=default,
            filetypes=[("PNG 图片", "*.png"), ("JPEG 图片", "*.jpg"),
                       ("所有文件", "*.*")])
        if path:
            try:
                if path.lower().endswith((".jpg", ".jpeg")):
                    img.convert("RGB").save(path, quality=95)
                else:
                    img.save(path)
                self.finish()
                self.flash_toast("已保存 ✓")
                return
            except Exception as ex:
                print("保存失败：", ex)
        self.finish()

    def _final(self):
        self.commit_text_entry()
        return self._flatten(ss=4)

    def cancel(self):
        self.finish()

    def finish(self):
        self._destroy_toolbars()
        for seq in ("<Escape>", "<Control-z>", "<Control-c>", "<Control-s>"):
            try:
                self.root.unbind_all(seq)
            except Exception:
                pass
        if self.overlay:
            try:
                self.overlay.destroy()
            except Exception:
                pass
        self.overlay = None
        self.annotations = []
        self.tool_buttons = {}
        self.text_entry = None
        self.tool = None
        self.hover_anno_idx = None
        self._imgrefs = []
        self.active = False
        if not self.resident:
            self.root.after(400, self.root.destroy)

    def flash_toast(self, text):
        toast = tk.Toplevel(self.root)
        toast.overrideredirect(True)
        toast.attributes("-topmost", True)
        try:
            toast.attributes("-alpha", 0.92)
        except Exception:
            pass
        tk.Label(toast, text=text, bg="#2B2B2B", fg="white",
                 font=(CN_FONT_FAMILY, -15), padx=20, pady=10).pack()
        toast.update_idletasks()
        sw, shh = toast.winfo_screenwidth(), toast.winfo_screenheight()
        w, h = toast.winfo_reqwidth(), toast.winfo_reqheight()
        toast.geometry(f"+{(sw - w) // 2}+{shh - h - 90}")
        toast.after(1300, toast.destroy)

    def run(self):
        self.root.mainloop()


def read_config_hotkey():
    """从可执行文件旁的 config.ini 读取自定义快捷键（免安装版用户可自行编辑）。
    支持形如：hotkey=ctrl+alt+a （行首 # 为注释）。"""
    path = _os.path.join(_exe_dir(), "config.ini")
    try:
        with open(path, "r", encoding="utf-8-sig") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or line.startswith("["):
                    continue
                if "=" in line:
                    k, v = line.split("=", 1)
                    if k.strip().lower() == "hotkey" and v.strip():
                        return v.strip()
    except Exception:
        pass
    return None


def main():
    resident, hotkey = True, "ctrl+alt+`"
    cfg = read_config_hotkey()            # 免安装版：config.ini 自定义快捷键
    if cfg:
        hotkey = cfg
    args = sys.argv[1:]
    if "--shot" in args:
        resident = False
    if "--hotkey" in args:                # 命令行优先级最高
        i = args.index("--hotkey")
        if i + 1 < len(args):
            hotkey = args[i + 1]

    # 单实例：已在运行 → 直接通知主实例截图后退出（双击快捷方式即截图）
    lock = acquire_lock()
    if lock is None:
        notify_primary()
        return

    if resident:
        ScreenshotTool(resident=True, hotkey=hotkey, ipc_sock=lock).run()
    else:
        # --shot 且当前就是主实例：截一次
        ScreenshotTool(resident=False, hotkey=hotkey, ipc_sock=lock).run()


if __name__ == "__main__":
    main()
