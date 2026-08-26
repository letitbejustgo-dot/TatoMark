# -*- coding: utf-8 -*-
"""生成 TatoMark 宣传图：忠实复用工具真实的图标 PNG、字体与配色，用 Pillow 合成。
输出到 assets/：hero.png（带标题的横幅）、interface.png（纯界面演示）、toolbar.png（工具栏特写）。
"""
import os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICON = os.path.join(ROOT, "icon")
FONTS = os.path.join(ROOT, "fonts")
OUT = os.path.join(ROOT, "assets")
os.makedirs(OUT, exist_ok=True)
S = 2  # 2x 超采样

# ---- 工具真实配色 ----
PALETTE = [(59,158,255),(95,206,59),(255,176,32),(58,63,68),(255,255,255),(255,91,91)]
GREEN = (19,192,96)
NODE_OUT = (154,160,166)
PILL_BORDER = (228,230,234)
ACTIVE_BG = (225,246,234)

serif = lambda px: ImageFont.truetype(os.path.join(FONTS,"SourceHanSerifCN-Regular.ttf"), px)
happy = lambda px: ImageFont.truetype(os.path.join(FONTS,"ZhanKuKuaiLeTi.ttf"), px)

ICONS = {"rect":"直方框.png","rrect":"圆角方框.png","ellipse":"圆圈.png","arrow":"长箭头-右上.png",
         "mosaic":"马赛克.png","text":"文本块.png","undo":"撤销.png","save":"下载.png",
         "close":"取消.png","confirm":"确定.png"}

def load_icon(key):
    im = Image.open(os.path.join(ICON, ICONS[key])).convert("RGBA")
    bbox = im.getbbox()  # 内容边界，统一视觉大小
    return im.crop(bbox) if bbox else im

def paste_icon(canvas, key, cx, cy, target):
    im = load_icon(key)
    w,h = im.size
    k = target/max(w,h)
    im = im.resize((max(1,int(w*k)),max(1,int(h*k))), Image.LANCZOS)
    canvas.alpha_composite(im, (int(cx-im.width/2), int(cy-im.height/2)))

def rounded(draw, box, r, fill=None, outline=None, width=1):
    draw.rounded_rectangle(box, radius=r, fill=fill, outline=outline, width=width)

# ---------- 工具栏 ----------
def draw_toolbar(canvas, x, y, active="arrow"):
    """返回 (宽,高)。忠实还原：白色圆角药丸 + 10 图标，选中项绿色底。"""
    btn=28*S; gap=7*S; pad=6*S
    order=["rect","rrect","ellipse","arrow","mosaic","text","undo","save","close","confirm"]
    w = pad*2 + len(order)*btn + (len(order)-1)*gap
    h = btn + pad*2
    d = ImageDraw.Draw(canvas)
    # 阴影
    sh = Image.new("RGBA", canvas.size, (0,0,0,0))
    ImageDraw.Draw(sh).rounded_rectangle([x,y+3*S,x+w,y+h+3*S], radius=12*S, fill=(20,26,40,70))
    canvas.alpha_composite(sh.filter(ImageFilter.GaussianBlur(7*S)))
    rounded(d,[x,y,x+w,y+h],12*S,fill=(255,255,255,255),outline=PILL_BORDER+(255,),width=1*S)
    cx = x+pad+btn/2
    for k in order:
        if k==active:
            rounded(d,[cx-btn/2+3*S,y+pad+3*S,cx+btn/2-3*S,y+h-pad-3*S],8*S,fill=ACTIVE_BG+(255,))
        ratio = 0.56 if k=="close" else 0.66
        paste_icon(canvas,k,cx,y+h/2,btn*ratio)
        cx += btn+gap
    return w,h

# ---------- 子栏（粗细 + 颜色）----------
def draw_subbar(canvas, x, y, color_idx=5, level=1):
    step=28*S; sp=12*S
    cw = sp+3*step+14*S+6*step+sp
    h=40*S
    d=ImageDraw.Draw(canvas)
    sh=Image.new("RGBA",canvas.size,(0,0,0,0))
    ImageDraw.Draw(sh).rounded_rectangle([x,y+3*S,x+cw,y+h+3*S],radius=12*S,fill=(20,26,40,60))
    canvas.alpha_composite(sh.filter(ImageFilter.GaussianBlur(6*S)))
    rounded(d,[x,y,x+cw,y+h],12*S,fill=(255,255,255,255),outline=PILL_BORDER+(255,),width=1*S)
    cy=y+h/2; px=x+sp
    dots=[4*S,7*S,10*S]
    for i,r in enumerate(dots):
        if i==level:
            rounded(d,[px+2*S,cy-step/2+2*S,px+step-2*S,cy+step/2-2*S],7*S,fill=ACTIVE_BG+(255,))
        d.ellipse([px+step/2-r,cy-r,px+step/2+r,cy+r],fill=(58,63,68,255))
        px+=step
    px+=14*S
    for i,c in enumerate(PALETTE):
        box=[px+6*S,cy-step/2+6*S,px+step-6*S,cy+step/2-6*S]
        rounded(d,box,4*S,fill=c+(255,),outline=(210,213,217,255) if c==(255,255,255) else None,width=1*S)
        if i==color_idx:
            rounded(d,[px+3*S,cy-step/2+3*S,px+step-3*S,cy+step/2-3*S],6*S,outline=GREEN+(255,),width=2*S)
        px+=step
    return cw,h

# ---------- 彗星箭头 ----------
def comet_arrow(canvas, p0, p1, color, w=18*S):
    import math
    d=ImageDraw.Draw(canvas)
    x0,y0=p0; x1,y1=p1
    dx,dy=x1-x0,y1-y0; L=math.hypot(dx,dy); ux,uy=dx/L,dy/L; nx,ny=-uy,ux
    hL=w*2.6; bx,by=x1-ux*hL, y1-uy*hL  # 箭身末端（箭头底）
    n=40; top=[]; bot=[]
    for i in range(n+1):
        t=i/n; x=x0+(bx-x0)*t; y=y0+(by-y0)*t
        hw=(0.15+0.85*t)*w/2
        top.append((x+nx*hw,y+ny*hw)); bot.append((x-nx*hw,y-ny*hw))
    d.polygon(top+bot[::-1], fill=color+(255,))
    hw2=w*1.15
    d.polygon([(x1,y1),(bx+nx*hw2,by+ny*hw2),(bx-nx*hw2,by-ny*hw2)], fill=color+(255,))

# ---------- 放大镜（取色）----------
def magnifier(canvas, cx, cy, sample_rgb):
    d=ImageDraw.Draw(canvas)
    D=118*S; grid=9; cell=D/grid
    lx,ly=cx-D/2, cy-D/2
    sh=Image.new("RGBA",canvas.size,(0,0,0,0))
    ImageDraw.Draw(sh).rectangle([lx,ly,lx+D,ly+D+62*S],fill=(20,26,40,80))
    canvas.alpha_composite(sh.filter(ImageFilter.GaussianBlur(8*S)))
    # 像素网格（围绕采样色的渐变，模拟放大后的像素）
    base=sample_rgb
    for gy in range(grid):
        for gx in range(grid):
            f=1+((gx-grid//2)+(gy-grid//2))*0.03
            c=tuple(max(0,min(255,int(v*f))) for v in base)
            d.rectangle([lx+gx*cell,ly+gy*cell,lx+(gx+1)*cell,ly+(gy+1)*cell],fill=c+(255,),outline=(0,0,0,25))
    # 绿色十字 + 中心格
    d.rectangle([lx,ly+D/2-cell/2,lx+D,ly+D/2+cell/2],outline=None)
    d.line([lx,cy,lx+D,cy],fill=GREEN+(230,),width=2*S)
    d.line([cx,ly,cx,ly+D],fill=GREEN+(230,),width=2*S)
    d.rectangle([cx-cell/2,cy-cell/2,cx+cell/2,cy+cell/2],outline=(255,255,255,255),width=2*S)
    d.rectangle([lx,ly,lx+D,ly+D],outline=(255,255,255,255),width=2*S)
    # 信息面板
    py=ly+D
    d.rectangle([lx,py,lx+D,py+62*S],fill=(245,246,248,255),outline=(255,255,255,255),width=2*S)
    hexs="#%02X%02X%02X"%base
    fS=serif(15*S)
    sw=16*S
    d.rectangle([lx+10*S,py+9*S,lx+10*S+sw,py+9*S+sw],fill=base+(255,),outline=(120,120,120,255))
    d.text((lx+10*S+sw+9*S,py+8*S),hexs,font=fS,fill=(20,20,20,255))
    d.text((lx+10*S,py+30*S),"RGB %d,%d,%d"%base,font=serif(13*S),fill=(60,60,60,255))
    d.text((lx+10*S,py+46*S),"右键复制色值",font=serif(12*S),fill=(140,140,140,255))

# ---------- 桌面背景（模拟真实屏幕）----------
def desktop(w,h):
    bg=Image.new("RGBA",(w,h),(0,0,0,255))
    d=ImageDraw.Draw(bg)
    for y in range(h):
        t=y/h
        c=(int(38+70*t),int(52+95*t*0.7),int(92+120*(1-t)*0.5))
        d.line([(0,y),(w,y)],fill=c+(255,))
    # 假窗口
    def win(x,y,ww,hh,tone):
        rounded(d,[x,y,x+ww,y+hh],10*S,fill=tone+(235,),outline=(255,255,255,40),width=1*S)
        d.rectangle([x,y,x+ww,y+30*S],fill=(tone[0]-8,tone[1]-8,tone[2]-8,235))
        for i,cc in enumerate([(255,95,86),(255,189,46),(39,201,63)]):
            d.ellipse([x+14*S+i*18*S,y+11*S,x+22*S+i*18*S,y+19*S],fill=cc+(255,))
        for i in range(4):
            d.line([(x+16*S,y+52*S+i*20*S),(x+ww-40*S,y+52*S+i*20*S)],fill=(255,255,255,30),width=2*S)
    win(60*S,70*S,520*S,360*S,(247,248,250))
    win(360*S,230*S,560*S,380*S,(30,36,52))
    return bg

# ---------- 组合界面演示 ----------
def build_interface(w,h,with_title):
    canvas=desktop(w,h)
    # 变暗层
    dim=Image.new("RGBA",(w,h),(0,0,0,128))
    canvas=Image.alpha_composite(canvas,dim)
    d=ImageDraw.Draw(canvas)
    # 选区（提亮：把原桌面亮区贴回来）
    sx,sy,sw_,sh_=int(w*0.30),int(h*0.20),int(w*0.46),int(h*0.50)
    bright=desktop(w,h).crop((sx,sy,sx+sw_,sy+sh_))
    canvas.alpha_composite(bright,(sx,sy))
    # 选区边框 + 8 手柄
    d.rectangle([sx,sy,sx+sw_,sy+sh_],outline=GREEN+(255,),width=2*S)
    pts=[(sx,sy),(sx+sw_/2,sy),(sx+sw_,sy),(sx+sw_,sy+sh_/2),(sx+sw_,sy+sh_),(sx+sw_/2,sy+sh_),(sx,sy+sh_),(sx,sy+sh_/2)]
    for (hx,hy) in pts:
        r=5*S; d.ellipse([hx-r,hy-r,hx+r,hy+r],fill=(255,255,255,255),outline=NODE_OUT+(255,),width=1*S)
    # 尺寸标签（左上，左对齐上移）
    d.text((sx+6*S,sy-24*S),"%d × %d"%(int(sw_/S),int(sh_/S)),font=serif(15*S),fill=(255,255,255,255))
    # 标注要素
    rounded(d,[sx+40*S,sy+70*S,sx+230*S,sy+180*S],14*S,outline=PALETTE[0]+(255,),width=4*S)  # 蓝圆角矩形
    comet_arrow(canvas,(sx+sw_-70*S,sy+sh_-60*S),(sx+270*S,sy+150*S),PALETTE[5])            # 红彗星箭头
    d.text((sx+60*S,sy+215*S),"重点看这里",font=happy(34*S),fill=PALETTE[2]+(255,))          # 橙色手写体文字
    # 马赛克小块
    mx,my=sx+300*S,sy+70*S
    for gy in range(5):
        for gx in range(9):
            import random; random.seed(gx*13+gy*7)
            c=(random.randint(90,180),)*3
            d.rectangle([mx+gx*12*S,my+gy*12*S,mx+(gx+1)*12*S,my+(gy+1)*12*S],fill=c+(255,))
    # 工具栏（选区下方）
    tw,th=draw_toolbar(canvas,sx,sy+sh_+14*S,active="arrow")
    draw_subbar(canvas,sx,sy+sh_+14*S+th+10*S,color_idx=5,level=1)
    # 放大镜（右上角外侧）
    magnifier(canvas,sx+sw_-90*S,sy+90*S,PALETTE[0])
    if with_title:
        # 顶部标题遮层
        grad=Image.new("RGBA",(w,int(h*0.34)),(0,0,0,0))
        gd=ImageDraw.Draw(grad)
        for y in range(grad.height):
            a=int(150*(1-y/grad.height))
            gd.line([(0,y),(w,y)],fill=(8,12,22,a))
        canvas.alpha_composite(grad,(0,0))
        d=ImageDraw.Draw(canvas)
        d.text((56*S,40*S),"TatoMark",font=happy(76*S),fill=(255,255,255,255))
        d.text((60*S,132*S),"原生 Direct2D · GPU 合成的极速截图标注",font=serif(26*S),fill=(220,230,245,255))
        feats="窗口磁吸 · 取色放大镜 · 彗星箭头 · 马赛克 · 文字标注 · 一键复制/保存"
        d.text((60*S,176*S),feats,font=serif(18*S),fill=(150,205,255,255))
    return canvas

def save(canvas,name):
    out=canvas.convert("RGB").resize((canvas.width//S,canvas.height//S),Image.LANCZOS)
    p=os.path.join(OUT,name); out.save(p,"PNG"); print("wrote",p,out.size)

# hero（带标题）
save(build_interface(1440*S,780*S,True),"hero.png")
# interface（纯界面）
save(build_interface(1440*S,780*S,False),"interface.png")
# toolbar 特写
tb=Image.new("RGBA",(560*S,150*S),(244,246,250,255))
gd=ImageDraw.Draw(tb)
for y in range(tb.height):
    t=y/tb.height; gd.line([(0,y),(tb.width,y)],fill=(int(236+10*t),int(240+8*t),int(248),255))
tw,th=draw_toolbar(tb,30*S,30*S,active="arrow")
draw_subbar(tb,30*S,30*S+th+14*S,color_idx=5,level=1)
save(tb,"toolbar.png")
print("done")
