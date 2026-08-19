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

CN_FONT_PATH = "C:/Windows/Fonts/msyh.ttc"
CN_FONT_FAMILY = "Microsoft YaHei"

ACCENT = "#13C060"
ICON = "#41464B"
RED = "#F5453A"
PILL_BG = "#FFFFFF"

PRESET_COLORS = ["#3B9EFF", "#5FCE3B", "#FFB020", "#3A3F44", "#FFFFFF", "#FF5B5B"]
LEVELS = [(3, 20), (6, 30), (11, 44)]   # (线宽, 字号)
SS = 4                                   # 图标超采样

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


def render_icon(name, px=32, state="normal"):
    key = (name, px, state)
    if key in _ICON_CACHE:
        return _ICON_CACHE[key]
    color = {"close": RED, "confirm": ACCENT}.get(name, ICON)
    col = _hex(color)
    S = px * SS
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)

    if state == "hover":
        d.rounded_rectangle([2 * SS, 2 * SS, (px - 2) * SS, (px - 2) * SS],
                            radius=8 * SS, fill=(238, 240, 242, 255))
    elif state == "active":
        d.rounded_rectangle([2 * SS, 2 * SS, (px - 2) * SS, (px - 2) * SS],
                            radius=8 * SS, fill=(225, 246, 234, 255))

    def s(v):
        return v * SS

    w = 2.3 * SS

    def poly(pts, width=w, c=col):
        p = [(s(x), s(y)) for x, y in pts]
        d.line(p, fill=c, width=int(round(width)), joint="curve")
        r = width / 2
        for x, y in p:
            d.ellipse([x - r, y - r, x + r, y + r], fill=c)

    if name == "rect":
        d.rounded_rectangle([s(8), s(9), s(24), s(23)], radius=s(3),
                            outline=col, width=int(w))
    elif name == "ellipse":
        d.ellipse([s(8), s(8), s(24), s(24)], outline=col, width=int(w))
    elif name == "arrow":
        d.polygon([(s(x), s(y)) for x, y in arrow_polygon(9, 23, 23, 9, 2.4)],
                  fill=col)
    elif name == "pen":
        tail, tip = (8.5, 23.5), (24, 8)
        dx, dy = tip[0] - tail[0], tip[1] - tail[1]
        L = math.hypot(dx, dy)
        ux, uy = dx / L, dy / L
        px_, py_ = -uy, ux
        hw, nib = 2.7, 5.0
        neck = (tip[0] - ux * nib, tip[1] - uy * nib)
        shaft = [(tail[0] + px_ * hw, tail[1] + py_ * hw),
                 (neck[0] + px_ * hw, neck[1] + py_ * hw),
                 (neck[0] - px_ * hw, neck[1] - py_ * hw),
                 (tail[0] - px_ * hw, tail[1] - py_ * hw)]
        d.polygon([(s(x), s(y)) for x, y in shaft], fill=col)
        d.polygon([(s(neck[0] + px_ * hw), s(neck[1] + py_ * hw)),
                   (s(tip[0]), s(tip[1])),
                   (s(neck[0] - px_ * hw), s(neck[1] - py_ * hw))], fill=col)
    elif name == "mosaic":
        cells = [(0, 0), (2, 0), (1, 1), (3, 1), (0, 2), (2, 2), (1, 3), (3, 3)]
        c, base = 3.6, 9.2
        for i, j in cells:
            x, y = base + i * c, base + j * c
            d.rectangle([s(x), s(y), s(x + c), s(y + c)], fill=col)
    elif name == "text":
        poly([(9, 10), (23, 10)])
        poly([(16, 10), (16, 23)])
    elif name == "undo":
        d.arc([s(9), s(10), s(24), s(25)], start=110, end=430, fill=col,
              width=int(w))
        cx, cy, rr = 16.5, 17.5, 7.5
        a = math.radians(110)
        ex, ey = cx + rr * math.cos(a), cy + rr * math.sin(a)
        poly([(ex - 3, ey - 1), (ex, ey), (ex + 1, ey - 3.2)], width=2.1 * SS)
    elif name == "save":
        poly([(16, 7), (16, 17.5)])
        d.polygon([(s(16), s(19)), (s(12.5), s(14.5)), (s(19.5), s(14.5))],
                  fill=col)
        poly([(8, 17), (8, 24), (24, 24), (24, 17)])
    elif name == "close":
        poly([(10, 10), (22, 22)], width=2.6 * SS)
        poly([(22, 10), (10, 22)], width=2.6 * SS)
    elif name == "confirm":
        poly([(9, 17), (14.5, 22.5), (24, 9)], width=2.8 * SS)

    out = ImageTk.PhotoImage(im.resize((px, px), Image.LANCZOS))
    _ICON_CACHE[key] = out
    return out


def render_swatch(color, selected, px=24):
    S = px * SS
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    outline = (204, 204, 204) if color == "#FFFFFF" else _hex(color)
    d.ellipse([4 * SS, 4 * SS, (px - 4) * SS, (px - 4) * SS], fill=_hex(color),
              outline=outline, width=SS)
    if selected:
        d.ellipse([1 * SS, 1 * SS, (px - 1) * SS, (px - 1) * SS],
                  outline=_hex(ACCENT), width=2 * SS)
    return ImageTk.PhotoImage(im.resize((px, px), Image.LANCZOS))


def render_level(idx, active, px=26):
    S = px * SS
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    rr = [3, 5, 7][idx]
    c = _hex(ACCENT) if active else (183, 188, 194)
    cx = px / 2
    d.ellipse([(cx - rr) * SS, (cx - rr) * SS, (cx + rr) * SS, (cx + rr) * SS],
              fill=c)
    return ImageTk.PhotoImage(im.resize((px, px), Image.LANCZOS))


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
    return ImageTk.PhotoImage(im.resize((px, px), Image.LANCZOS))


def render_pencil(px=22):
    """文字右上角的铅笔按钮：白底圆 + 绿色铅笔。"""
    S = px * SS
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse([SS, SS, (px - 1) * SS, (px - 1) * SS], fill=(255, 255, 255, 255),
              outline=_hex(ACCENT), width=int(1.6 * SS))
    col = _hex(ACCENT)

    def s(v):
        return v * SS

    tail, tip = (6.8, 14.5), (14.5, 6.8)
    dx, dy = tip[0] - tail[0], tip[1] - tail[1]
    L = math.hypot(dx, dy)
    ux, uy = dx / L, dy / L
    px_, py_ = -uy, ux
    hw, nib = 1.9, 3.4
    neck = (tip[0] - ux * nib, tip[1] - uy * nib)
    shaft = [(tail[0] + px_ * hw, tail[1] + py_ * hw),
             (neck[0] + px_ * hw, neck[1] + py_ * hw),
             (neck[0] - px_ * hw, neck[1] - py_ * hw),
             (tail[0] - px_ * hw, tail[1] - py_ * hw)]
    d.polygon([(s(x), s(y)) for x, y in shaft], fill=col)
    d.polygon([(s(neck[0] + px_ * hw), s(neck[1] + py_ * hw)),
               (s(tip[0]), s(tip[1])),
               (s(neck[0] - px_ * hw), s(neck[1] - py_ * hw))], fill=col)
    return ImageTk.PhotoImage(im.resize((px, px), Image.LANCZOS))


def render_pill(w, h, seps):
    """圆角白色药丸 + 柔和阴影，返回 (PhotoImage, margin)。"""
    M = 16
    W, Hh = w + 2 * M, h + 2 * M
    shadow = Image.new("RGBA", (W, Hh), (0, 0, 0, 0))
    ds = ImageDraw.Draw(shadow)
    ds.rounded_rectangle([M, M + 3, M + w, M + h + 3], radius=h // 2,
                         fill=(0, 0, 0, 55))
    shadow = shadow.filter(ImageFilter.GaussianBlur(7))
    base = Image.new("RGBA", (W, Hh), (0, 0, 0, 0))
    base = Image.alpha_composite(base, shadow)
    d = ImageDraw.Draw(base)
    d.rounded_rectangle([M, M, M + w, M + h], radius=h // 2,
                        fill=(255, 255, 255, 255), outline=(228, 230, 234, 255),
                        width=1)
    for cx in seps:
        d.line([M + cx, M + 10, M + cx, M + h - 10], fill=(224, 227, 231, 255),
               width=1)
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
        self.text_entry = None
        self.tool = None
        self.annotations = []
        self.hover_anno_idx = None
        self.move_idx = None
        self._move_photo = None
        self._tb_widgets = []
        self._imgrefs = []

        self.canvas.bind("<ButtonPress-1>", self.on_press)
        self.canvas.bind("<B1-Motion>", self.on_drag)
        self.canvas.bind("<ButtonRelease-1>", self.on_release)
        self.canvas.bind("<Motion>", self.on_hover)
        self.canvas.bind("<Double-Button-1>", self.on_double)
        self.overlay.bind("<Escape>", lambda e: self.cancel())
        self.overlay.bind("<Control-z>", lambda e: self.undo())
        self.overlay.bind("<Control-c>", lambda e: self.do_copy())
        self.overlay.bind("<Control-s>", lambda e: self.do_save())
        self.overlay.focus_force()
        self.canvas.focus_set()

        self.hint = self.canvas.create_text(
            self.vw // 2, 34, text="拖动鼠标框选区域        Esc 取消",
            fill="#EEEEEE", font=(CN_FONT_FAMILY, -17))

    # ---------------- 选区阶段 ----------------
    def on_press(self, e):
        if self.mode == "select":
            self.start_pt = (e.x, e.y)
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
            return
        self.sel = [l, t, r, b]
        self.mode = "edit"
        self.canvas.delete(self.hint)
        self.canvas.delete("sel")
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
        f = tkfont.Font(family=CN_FONT_FAMILY, size=-fs)
        return f.measure(text or " "), f.metrics("linespace")

    def _anno_bbox(self, a):
        t = a["type"]
        if t in ("rect", "ellipse", "mosaic", "arrow"):
            x0, y0, x1, y1 = a["coords"]
            return (min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1))
        if t == "pen":
            xs, ys = a["points"][0::2], a["points"][1::2]
            return (min(xs), min(ys), max(xs), max(ys))
        if t == "text":
            x, y = a["coords"]
            w, h = self._text_size(a["text"], a["font_size"])
            return (x, y, x + w, y + h)
        return (0, 0, 0, 0)

    def _anno_at(self, x, y, pad=5):
        for i in range(len(self.annotations) - 1, -1, -1):
            bx0, by0, bx1, by1 = self._anno_bbox(self.annotations[i])
            if bx0 - pad <= x <= bx1 + pad and by0 - pad <= y <= by1 + pad:
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
            self._pencil_img = render_pencil(22)
        bx0, by0, bx1, by1 = self._anno_bbox(self.annotations[idx])
        self.canvas.create_image(bx1, by0, image=self._pencil_img,
                                 tags="editbtn")

    # ---------------- 悬停 ----------------
    def on_hover(self, e):
        if self.mode != "edit" or self.drag_mode:
            return
        idx = self._anno_at(e.x, e.y)
        # 悬停到铅笔按钮上时保持当前文字元素
        if idx is None and self.hover_anno_idx is not None \
                and self.hover_anno_idx < len(self.annotations) \
                and self._in_pencil(e.x, e.y, self.hover_anno_idx):
            idx = self.hover_anno_idx
        self.canvas.delete("editbtn")
        if idx is not None:
            self.hover_anno_idx = idx
            if self.annotations[idx]["type"] == "text":
                self._draw_edit_handle(idx)
            self.overlay.configure(cursor="hand2")   # 手型：可拖动
            return
        self.hover_anno_idx = None

        hit = self.hit_test(e.x, e.y)
        cur = {"nw": "size_nw_se", "se": "size_nw_se", "ne": "size_ne_sw",
               "sw": "size_ne_sw", "n": "size_ns", "s": "size_ns",
               "e": "size_we", "w": "size_we"}
        if hit[0] == "handle":
            self.overlay.configure(cursor=cur.get(hit[1], "left_ptr"))
        elif hit[0] == "border":
            self.overlay.configure(cursor="fleur")
        elif hit[0] == "inside":
            self.overlay.configure(
                cursor="xterm" if self.tool == "text"
                else ("crosshair" if self.tool else "fleur"))
        else:
            self.overlay.configure(cursor="left_ptr")

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
        idx = self._anno_at(e.x, e.y)
        if idx is None and self.hover_anno_idx is not None \
                and self.hover_anno_idx < len(self.annotations) \
                and self._in_pencil(e.x, e.y, self.hover_anno_idx):
            idx = self.hover_anno_idx
        # 1) 点击文字右上角铅笔 → 重新编辑
        if idx is not None and self._in_pencil(e.x, e.y, idx):
            self.edit_text(idx)
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

    def _start_move_anno(self, idx, e):
        self.drag_mode = "moveanno"
        self.move_idx = idx
        self.start_pt = (e.x, e.y)
        # 背景 = 除该元素外的抗锯齿合成，之后仅移动该元素
        bg = self._flatten(exclude=idx)
        self._show_crop(bg)
        self._draw_chrome()

    def edit_drag(self, e):
        if self.drag_mode == "resize":
            self._do_resize(e.x, e.y)
        elif self.drag_mode == "move":
            self._do_move(e.x, e.y)
        elif self.drag_mode == "moveanno":
            dx, dy = e.x - self.start_pt[0], e.y - self.start_pt[1]
            self._move_anno(self.move_idx, dx, dy)
            self.start_pt = (e.x, e.y)
            self.canvas.delete("movetemp")
            self._draw_one_vector(self.annotations[self.move_idx], "movetemp")
            self._draw_edit_handle(self.move_idx)
        elif self.drag_mode == "draw":
            self._do_draw(e.x, e.y)

    def edit_release(self, e):
        if self.drag_mode in ("resize", "move"):
            l, t, r, b = self.sel
            self.sel = [min(l, r), min(t, b), max(l, r), max(t, b)]
            self.drag_mode = None
            self.build_toolbar()
            self.redraw_static()
        elif self.drag_mode == "moveanno":
            self.canvas.delete("movetemp")
            self.drag_mode = None
            self.redraw_static()
            self._draw_edit_handle(self.move_idx)
        elif self.drag_mode == "draw":
            self._commit_draw(e.x, e.y)
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
        if a["type"] == "pen":
            a["points"] = [c + (dx if i % 2 == 0 else dy)
                           for i, c in enumerate(a["points"])]
        elif a["type"] == "text":
            x, y = a["coords"]
            a["coords"] = (x + dx, y + dy)
        else:
            x0, y0, x1, y1 = a["coords"]
            a["coords"] = (x0 + dx, y0 + dy, x1 + dx, y1 + dy)

    def _clamp(self, x, y):
        l, t, r, b = self.sel
        return min(max(x, l), r), min(max(y, t), b)

    # ---------------- 绘制新元素（拖动中用矢量临时预览）----------------
    def _draw_comet(self, coords, w, color, tag):
        for quad, col in comet_polys(*coords, w, color):
            self.canvas.create_polygon(quad, fill=col, outline=col, tags=tag)

    def _do_draw(self, x, y):
        x, y = self._clamp(x, y)
        x0, y0 = self.start_pt
        if self.temp_item:
            self.canvas.delete(self.temp_item)
            self.temp_item = None
        self.canvas.delete("drawtemp")
        w = self.width
        if self.tool == "rect":
            self.temp_item = self.canvas.create_rectangle(
                x0, y0, x, y, outline=self.color, width=w)
        elif self.tool == "ellipse":
            self.temp_item = self.canvas.create_oval(
                x0, y0, x, y, outline=self.color, width=w)
        elif self.tool == "arrow":
            self._draw_comet((x0, y0, x, y), w, self.color, "drawtemp")
        elif self.tool == "mosaic":
            self.temp_item = self.canvas.create_rectangle(
                x0, y0, x, y, outline="#FFFFFF", width=1, dash=(4, 3))
        elif self.tool == "pen":
            self.pen_points.extend([x, y])
            self.temp_item = self.canvas.create_line(
                *self.pen_points, fill=self.color, width=w,
                capstyle=tk.ROUND, joinstyle=tk.ROUND, smooth=True)

    def _commit_draw(self, x, y):
        x, y = self._clamp(x, y)
        x0, y0 = self.start_pt
        a = None
        if self.tool == "rect" and abs(x - x0) > 2 and abs(y - y0) > 2:
            a = {"type": "rect", "coords": (x0, y0, x, y),
                 "color": self.color, "width": self.width}
        elif self.tool == "ellipse" and abs(x - x0) > 2 and abs(y - y0) > 2:
            a = {"type": "ellipse", "coords": (x0, y0, x, y),
                 "color": self.color, "width": self.width}
        elif self.tool == "arrow" and (abs(x - x0) > 3 or abs(y - y0) > 3):
            a = {"type": "arrow", "coords": (x0, y0, x, y),
                 "color": self.color, "width": self.width}
        elif self.tool == "mosaic" and abs(x - x0) > 4 and abs(y - y0) > 4:
            a = {"type": "mosaic",
                 "coords": (min(x0, x), min(y0, y), max(x0, x), max(y0, y))}
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
            self.redraw_static()

    # ---------------- 文字 ----------------
    def place_text_entry(self, x, y, text="", color=None, fs=None):
        self.commit_text_entry()
        x, y = self._clamp(x, y)
        color = color or self.color
        fs = fs or self.font_size
        var = tk.StringVar(value=text)
        entry = tk.Entry(self.overlay, textvariable=var, bd=0, bg="#FFFFFF",
                         fg=color, insertbackground=color,
                         font=(CN_FONT_FAMILY, -fs))
        ew = self.canvas.create_window(x, y, anchor="nw", window=entry)
        entry.focus_set()
        entry.icursor("end")
        self.text_entry = (entry, var, x, y, ew, color, fs)
        entry.bind("<Return>", lambda e: self.commit_text_entry())
        entry.bind("<Escape>", lambda e: self.cancel_text_entry())

    def edit_text(self, idx):
        a = self.annotations.pop(idx)
        self.redraw_static()
        x, y = a["coords"]
        self.place_text_entry(x, y, a["text"], a["color"], a["font_size"])

    def commit_text_entry(self):
        if not self.text_entry:
            return
        entry, var, x, y, ew, color, fs = self.text_entry
        text = var.get()
        self.canvas.delete(ew)
        entry.destroy()
        self.text_entry = None
        if text.strip():
            self.annotations.append({"type": "text", "coords": (x, y),
                                     "text": text, "color": color,
                                     "font_size": fs})
            self.redraw_static()

    def cancel_text_entry(self):
        if not self.text_entry:
            return
        entry, var, x, y, ew, color, fs = self.text_entry
        self.canvas.delete(ew)
        entry.destroy()
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

        # 1) 马赛克（先铺到底图）
        for a in items:
            if a["type"] == "mosaic":
                x0, y0, x1, y1 = a["coords"]
                reg = base.crop((x0 - ox, y0 - oy, x1 - ox, y1 - oy))
                base.paste(self._pixelate(reg), (x0 - ox, y0 - oy))

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
            elif typ == "ellipse":
                x0, y0, x1, y1 = a["coords"]
                aa = P(min(x0, x1), min(y0, y1))
                bb = P(max(x0, x1), max(y0, y1))
                d.ellipse([aa[0] - half, aa[1] - half,
                           bb[0] + half, bb[1] + half],
                          outline=col, width=int(wpx))
            elif typ == "arrow":
                x0, y0, x1, y1 = a["coords"]
                rot = render_comet_ss(x0, y0, x1, y1, a["width"], a["color"], ss)
                mx, my = P((x0 + x1) / 2, (y0 + y1) / 2)
                ov.alpha_composite(rot, (int(mx - rot.width / 2),
                                         int(my - rot.height / 2)))
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
            result = Image.alpha_composite(result, ov.resize((W, H),
                                                             Image.LANCZOS))
        # 3) 文字（1x 直接绘制，字体自带抗锯齿，最清晰）
        dt = ImageDraw.Draw(result)
        for a in items:
            if a["type"] != "text":
                continue
            x, y = a["coords"]
            try:
                font = ImageFont.truetype(CN_FONT_PATH, a["font_size"])
            except Exception:
                font = ImageFont.load_default()
            dt.text((x - ox, y - oy), a["text"], fill=a["color"], font=font)
        return result.convert("RGB")

    @staticmethod
    def _pixelate(region, block=12):
        w, h = region.size
        if w < 1 or h < 1:
            return region
        small = region.resize((max(1, w // block), max(1, h // block)),
                              Image.NEAREST)
        return small.resize((w, h), Image.NEAREST)

    # ---------------- 显示 ----------------
    def _show_crop(self, pil_img):
        l, t, r, b = self.sel
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
        for hx, hy in self.handle_points().values():
            self.canvas.create_rectangle(hx - 5, hy - 5, hx + 5, hy + 5,
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
        elif t == "ellipse":
            self.canvas.create_oval(*a["coords"], outline=a["color"],
                                    width=a["width"], tags=tag)
        elif t == "arrow":
            self._draw_comet(a["coords"], a["width"], a["color"], tag)
        elif t == "pen":
            self.canvas.create_line(*a["points"], fill=a["color"],
                                    width=a["width"], capstyle=tk.ROUND,
                                    joinstyle=tk.ROUND, smooth=True, tags=tag)
        elif t == "mosaic":
            x0, y0, x1, y1 = a["coords"]
            reg = self.full_img.crop((x0, y0, x1, y1))
            photo = ImageTk.PhotoImage(self._pixelate(reg))
            self._imgrefs.append(photo)
            self.canvas.create_image(x0, y0, anchor="nw", image=photo, tags=tag)
        elif t == "text":
            x, y = a["coords"]
            self.canvas.create_text(x, y, anchor="nw", text=a["text"],
                                    fill=a["color"],
                                    font=(CN_FONT_FAMILY, -a["font_size"]),
                                    tags=tag)

    def _size_label(self, l, t, r, b, tag):
        text = f"{r - l} × {b - t}"
        ly = t - 24 if t > 30 else t + 8
        self.canvas.create_text(l + 1, ly + 1, anchor="nw", text=text,
                                fill="#000000", font=(CN_FONT_FAMILY, -14),
                                tags=tag)
        self.canvas.create_text(l, ly, anchor="nw", text=text, fill="#FFFFFF",
                                font=(CN_FONT_FAMILY, -14), tags=tag)

    def undo(self):
        if self.text_entry:
            self.cancel_text_entry()
            return
        if self.annotations:
            self.annotations.pop()
            self.hover_anno_idx = None
            self.canvas.delete("editbtn")
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
        layout = [("tool", "rect"), ("tool", "ellipse"), ("tool", "arrow"),
                  ("tool", "pen"), ("tool", "mosaic"), ("tool", "text"),
                  ("sep",),
                  ("act", "undo"), ("act", "save"),
                  ("sep",),
                  ("act", "close"), ("act", "confirm")]

        btn, gap, sep_w, pad = 32, 4, 15, 10
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
        tx = min(max(r - width, 8), self.vw - width - 8)
        ty = t - height - 14
        if ty < 8:
            ty = b + 14
        if ty + height > self.vh - 8:
            ty = max(8, t - height - 14)
        self._tb_pos = (tx, ty, width, height)

        pill, M = render_pill(width, height, seps)
        self._imgrefs.append(pill)
        self.canvas.create_image(tx - M, ty - M, anchor="nw", image=pill,
                                 tags="toolbar")

        cy = ty + height // 2
        for kind, name, rx in positions:
            self._make_button(kind, name, tx + rx, cy - btn // 2, btn)

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
                 "close": self.cancel, "confirm": self.do_copy}[name]()

        lbl.bind("<Button-1>", on_click)
        lbl.bind("<Enter>", lambda e: show("active" if lbl._active else "hover"))
        lbl.bind("<Leave>", lambda e: show("active" if lbl._active else "normal"))
        lbl._show = show
        self.canvas.create_window(x, y, anchor="nw", window=lbl, tags="toolbar")
        self._tb_widgets.append(lbl)
        if kind == "tool":
            self.tool_buttons[name] = lbl

    def _highlight_tool(self):
        for name, lbl in self.tool_buttons.items():
            lbl._active = (name == self.tool)
            lbl._show("active" if lbl._active else "normal")

    def set_tool(self, name):
        self.tool = None if self.tool == name else name
        self._highlight_tool()

    def _build_subbar(self, tx, ty, width, height):
        sh = 40
        sy = ty + height + 10
        if sy + sh > self.vh - 8:
            sy = ty - sh - 10

        # 计算内容宽度
        n_level, n_color = len(LEVELS), len(PRESET_COLORS)
        pad = 12
        cw = pad + n_level * 28 + 14 + (n_color + 1) * 28 + pad
        sx = min(max(tx, 8), self.vw - cw - 8)

        seps = [pad + n_level * 28 + 7]
        pill, M = render_pill(cw, sh, seps)
        self._imgrefs.append(pill)
        self.canvas.create_image(sx - M, sy - M, anchor="nw", image=pill,
                                 tags="toolbar")

        cy = sy + sh // 2
        x = sx + pad
        self.level_buttons = []
        for i in range(n_level):
            lbl = tk.Label(self.canvas, bg=PILL_BG, bd=0, cursor="hand2")
            lbl.bind("<Button-1>", lambda e, k=i: self.set_level(k))
            self.canvas.create_window(x + 13, cy, window=lbl, tags="toolbar")
            self.level_buttons.append(lbl)
            self._tb_widgets.append(lbl)
            x += 28
        x += 14
        self.color_swatches = {}
        for col in PRESET_COLORS:
            lbl = tk.Label(self.canvas, bg=PILL_BG, bd=0, cursor="hand2")
            lbl.bind("<Button-1>", lambda e, c=col: self.set_color(c))
            self.canvas.create_window(x + 13, cy, window=lbl, tags="toolbar")
            self.color_swatches[col] = lbl
            self._tb_widgets.append(lbl)
            x += 28
        more = tk.Label(self.canvas, image=render_plus(), bg=PILL_BG, bd=0,
                        cursor="hand2")
        more._ref = more.cget("image")
        more.bind("<Button-1>", lambda e: self.pick_color())
        self.canvas.create_window(x + 13, cy, window=more, tags="toolbar")
        self._tb_widgets.append(more)

        self.set_level(self.level)
        self.set_color(self.color)

    def set_level(self, k):
        self.level = k
        self.width, self.font_size = LEVELS[k]
        for i, lbl in enumerate(self.level_buttons):
            img = render_level(i, i == k)
            lbl._ref = img
            lbl.configure(image=img)

    def set_color(self, col):
        self.color = col
        if not hasattr(self, "color_swatches"):
            return
        for c, lbl in self.color_swatches.items():
            img = render_swatch(c, c == col)
            lbl._ref = img
            lbl.configure(image=img)

    def pick_color(self):
        _, hexc = colorchooser.askcolor(color=self.color, parent=self.overlay)
        if hexc:
            self.set_color(hexc)

    # ---------------- 动作 ----------------
    def do_copy(self):
        if self.mode != "edit":
            return
        img = self._final()
        ok = image_to_clipboard(img)
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


def main():
    resident, hotkey = True, "ctrl+alt+`"
    args = sys.argv[1:]
    if "--shot" in args:
        resident = False
    if "--hotkey" in args:
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
