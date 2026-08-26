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

def paste_card(base,img,x,y,rad):
    mask=Image.new("L",img.size,0)
    ImageDraw.Draw(mask).rounded_rectangle([0,0,img.width-1,img.height-1],rad,fill=255)
    sh=Image.new("RGBA",base.size,(0,0,0,0))
    shm=Image.new("RGBA",img.size,(0,0,0,0))
    ImageDraw.Draw(shm).rounded_rectangle([0,0,img.width-1,img.height-1],rad,fill=(18,26,48,120))
    sh.alpha_composite(shm,(x,y+10*S))
    base.alpha_composite(sh.filter(ImageFilter.GaussianBlur(18*S)))
    base.paste(img,(x,y),mask)
    ImageDraw.Draw(base).rounded_rectangle([x,y,x+img.width-1,y+img.height-1],rad,outline=(255,255,255,90),width=1*S)

# ---------- 彗星路径（沿折线/贝塞尔，尾细头粗 + 箭头）----------
import math
def comet_path(canvas,pts,color,w=18*S):
    d=ImageDraw.Draw(canvas)
    # 稀疏折线（如仅首尾两点）先按弧长重采样为密集点，保证尾巴渐变平滑
    if len(pts)<24:
        seg=[0.0]
        for i in range(1,len(pts)): seg.append(seg[-1]+math.hypot(pts[i][0]-pts[i-1][0],pts[i][1]-pts[i-1][1]))
        tot=seg[-1] or 1; N=64; dens=[]
        for k in range(N+1):
            target=tot*k/N; j=1
            while j<len(seg) and seg[j]<target: j+=1
            j=min(j,len(pts)-1); a=seg[j-1]; b=seg[j] or a+1; t=(target-a)/(b-a)
            dens.append((pts[j-1][0]+(pts[j][0]-pts[j-1][0])*t, pts[j-1][1]+(pts[j][1]-pts[j-1][1])*t))
        pts=dens
    tip=pts[-1]
    ax,ay=tip[0]-pts[-2][0],tip[1]-pts[-2][1]; L=math.hypot(ax,ay) or 1
    ux,uy=ax/L,ay/L; nx,ny=-uy,ux; hL=w*2.6
    bx,by=tip[0]-ux*hL,tip[1]-uy*hL
    cum=[0]
    for i in range(1,len(pts)): cum.append(cum[-1]+math.hypot(pts[i][0]-pts[i-1][0],pts[i][1]-pts[i-1][1]))
    total=max(1,cum[-1]-hL)
    top=[];bot=[]
    for i,p in enumerate(pts):
        if cum[i]>total: break
        t=cum[i]/total; hw=(0.14+0.86*t)*w/2
        if i<len(pts)-1: dxx,dyy=pts[i+1][0]-p[0],pts[i+1][1]-p[1]
        else: dxx,dyy=p[0]-pts[i-1][0],p[1]-pts[i-1][1]
        dl=math.hypot(dxx,dyy) or 1; nnx,nny=-dyy/dl,dxx/dl
        top.append((p[0]+nnx*hw,p[1]+nny*hw)); bot.append((p[0]-nnx*hw,p[1]-nny*hw))
    if len(top)>1: d.polygon(top+bot[::-1],fill=color+(255,))
    hw2=w*1.15
    d.polygon([tip,(bx+nx*hw2,by+ny*hw2),(bx-nx*hw2,by-ny*hw2)],fill=color+(255,))

def bezier(p0,p1,p2,n=64):
    out=[]
    for i in range(n+1):
        t=i/n; mt=1-t
        out.append((mt*mt*p0[0]+2*mt*t*p1[0]+t*t*p2[0], mt*mt*p0[1]+2*mt*t*p1[1]+t*t*p2[1]))
    return out

def light_card(w,h):
    c=Image.new("RGBA",(w,h),(255,255,255,255))
    d=ImageDraw.Draw(c)
    for y in range(h):
        t=y/h; d.line([(0,y),(w,y)],fill=(int(250-6*t),int(251-5*t),int(253-3*t),255))
    return c

def node(d,x,y,r=5*S):
    d.ellipse([x-r,y-r,x+r,y+r],fill=(255,255,255,255),outline=NODE_OUT+(255,),width=1*S)

def dashed_rect(d,box,color,dash=10*S,gap=7*S,w=2*S):
    x0,y0,x1,y1=box
    def seg(a,b,horiz,fix):
        p=a
        while p<b:
            q=min(p+dash,b)
            if horiz: d.line([(p,fix),(q,fix)],fill=color,width=w)
            else: d.line([(fix,p),(fix,q)],fill=color,width=w)
            p=q+gap
    seg(x0,x1,True,y0); seg(x0,x1,True,y1); seg(y0,y1,False,x0); seg(y0,y1,False,x1)

# ================= HERO（清爽两栏，思源宋体标题）=================
def build_hero():
    W,H=1440*S,600*S
    hero=Image.new("RGBA",(W,H),(255,255,255,255))
    d=ImageDraw.Draw(hero)
    for y in range(H):
        t=y/H; d.line([(0,y),(W,y)],fill=(int(246-8*t),int(249-6*t),int(253-2*t),255))
    # 右侧界面卡片
    card=build_interface(1180*S,720*S,False)
    cw=760*S; ch=int(cw*card.height/card.width)
    card=card.resize((cw,ch),Image.LANCZOS)
    paste_card(hero,card,W-cw-70*S,(H-ch)//2,16*S)
    # 左侧文案（全部思源宋体——规范字体）
    d.text((92*S,120*S),"WINDOWS 截图标注",font=serif(20*S),fill=(120,150,190,255))
    d.text((88*S,150*S),"TatoMark",font=serif(96*S),fill=(28,34,52,255))
    d.text((92*S,272*S),"原生 Direct2D · GPU 合成",font=serif(30*S),fill=(70,80,100,255))
    d.text((92*S,314*S),"变暗 / 提亮 / 放大全交给显卡，丝滑不吃 CPU",font=serif(21*S),fill=(120,130,150,255))
    bullets=[(PALETTE[0],"窗口磁吸选取 · 取色放大镜"),
             (PALETTE[5],"彗星箭头 · 可弯折可旋转"),
             (PALETTE[2],"马赛克 · 文字 · 要素自由缩放"),
             (GREEN,"双击即截 · 一键复制 / 保存")]
    by=380*S
    for col,txt in bullets:
        d.ellipse([94*S,by+6*S,94*S+13*S,by+19*S],fill=col+(255,))
        d.text((120*S,by),txt,font=serif(22*S),fill=(60,68,84,255))
        by+=44*S
    save(hero,"hero.png")

# ================= 彗星箭头图 =================
def build_comet():
    c=light_card(920*S,380*S); d=ImageDraw.Draw(c)
    d.text((40*S,28*S),"彗星箭头：尾细头粗，可拖中点弯折",font=serif(24*S),fill=(40,48,64,255))
    # 直箭头（红）
    comet_path(c,[(80*S,300*S),(360*S,130*S)],PALETTE[5])
    d.text((90*S,318*S),"拖拽即画",font=serif(18*S),fill=(120,130,150,255))
    # 弯折箭头（蓝）+ 绿色中点控制
    p0=(500*S,310*S); ctrl=(640*S,120*S); p2=(860*S,200*S)
    comet_path(c,bezier(p0,ctrl,p2),PALETTE[0])
    node_c=(int((p0[0]+p2[0])/2),int((p0[1]+p2[1])/2))
    d.ellipse([ctrl[0]-8*S,ctrl[1]-8*S,ctrl[0]+8*S,ctrl[1]+8*S],fill=GREEN+(255,),outline=(255,255,255,255),width=2*S)
    d.text((560*S,320*S),"拖绿色中点 → 弯折",font=serif(18*S),fill=(120,130,150,255))
    save(c,"comet.png")

# ================= 缩放 / 旋转图 =================
def build_resize():
    c=light_card(920*S,380*S); d=ImageDraw.Draw(c)
    d.text((40*S,26*S),"选中要素：四角拖拽缩放 / 旋转，文本等比缩放",font=serif(24*S),fill=(40,48,64,255))
    # 一个蓝色圆角矩形要素 + 虚线包围盒 + 四角节点 + 旋转手柄
    bx0,by0,bx1,by1=150*S,140*S,420*S,300*S
    rounded(d,[bx0,by0,bx1,by1],16*S,outline=PALETTE[0]+(255,),width=4*S)
    dashed_rect(d,[bx0-10*S,by0-10*S,bx1+10*S,by1+10*S],NODE_OUT+(255,))
    corners=[(bx0-10*S,by0-10*S),(bx1+10*S,by0-10*S),(bx1+10*S,by1+10*S),(bx0-10*S,by1+10*S)]
    for cx,cy in corners: node(d,cx,cy,6*S)
    # 旋转手柄
    hx=(bx0+bx1)//2; hy=by0-10*S
    d.line([(hx,hy),(hx,hy-46*S)],fill=NODE_OUT+(255,),width=2*S)
    rh=(hx,hy-60*S)
    d.ellipse([rh[0]-15*S,rh[1]-15*S,rh[0]+15*S,rh[1]+15*S],fill=(255,255,255,255),outline=NODE_OUT+(255,),width=2*S)
    rot=Image.open(os.path.join(ICON,"顺时针旋转.png")).convert("RGBA")
    bb=rot.getbbox(); rot=rot.crop(bb) if bb else rot
    k=20*S/max(rot.size); rot=rot.resize((int(rot.width*k),int(rot.height*k)),Image.LANCZOS)
    c.alpha_composite(rot,(int(rh[0]-rot.width/2),int(rh[1]-rot.height/2)))
    # 缩放双向箭头（右下角）
    ah=(bx1+10*S,by1+10*S)
    d.line([(ah[0]-26*S,ah[1]-26*S),(ah[0]+26*S,ah[1]+26*S)],fill=PALETTE[5]+(255,),width=3*S)
    for sgn in (-1,1):
        ex,ey=ah[0]+sgn*26*S,ah[1]+sgn*26*S
        d.polygon([(ex,ey),(ex-sgn*14*S,ey-sgn*4*S),(ex-sgn*4*S,ey-sgn*14*S)],fill=PALETTE[5]+(255,))
    d.text((150*S,320*S),"拖四角：缩放",font=serif(18*S),fill=(120,130,150,255))
    d.text((300*S,320*S),"拖顶部圆点：旋转",font=serif(18*S),fill=(120,130,150,255))
    # 文本要素（等比）
    d.text((560*S,150*S),"文字",font=happy(60*S),fill=PALETTE[2]+(255,))
    tb=[540*S,140*S,700*S,220*S]
    dashed_rect(d,tb,NODE_OUT+(255,))
    for cx,cy in [(tb[0],tb[1]),(tb[2],tb[1]),(tb[2],tb[3]),(tb[0],tb[3])]: node(d,cx,cy,6*S)
    d.text((560*S,250*S),"文本要素：等比缩放",font=serif(18*S),fill=(120,130,150,255))
    save(c,"resize.png")

# ================= 纯界面 + 工具栏特写 =================
def build_pure():
    save(build_interface(1440*S,780*S,False),"interface.png")
    tb=Image.new("RGBA",(560*S,150*S),(244,246,250,255))
    gd=ImageDraw.Draw(tb)
    for y in range(tb.height):
        t=y/tb.height; gd.line([(0,y),(tb.width,y)],fill=(int(236+10*t),int(240+8*t),int(248),255))
    tw,th=draw_toolbar(tb,30*S,30*S,active="arrow")
    draw_subbar(tb,30*S,30*S+th+14*S,color_idx=5,level=1)
    save(tb,"toolbar.png")

build_hero()
build_pure()
build_comet()
build_resize()
print("done")
