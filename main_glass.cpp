// TatoMark (C++ / Direct2D 原生 GPU 合成版) —— M1 + M2
// M1: 覆盖窗 + 桌面捕获 + 变暗 + 选区 + 取色放大镜 + 窗口磁吸
// M2: 工具栏 + 矩形/圆角矩形/椭圆 + 颜色/粗细 + 复制到剪贴板 + 保存 PNG
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#ifdef DrawText
#undef DrawText
#endif
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <commdlg.h>
#include <vector>
#include <string>
#include <map>
#include <cstdio>
#include <cwchar>
#include <cmath>
#include <algorithm>

#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif
using std::min; using std::max;

template <class T> void SafeRelease(T** pp){ if(*pp){ (*pp)->Release(); *pp=nullptr; } }

// ---- COM 全局 ----
static ID2D1Factory*          g_factory = nullptr;
static IDWriteFactory*        g_dwrite  = nullptr;
static IWICImagingFactory*    g_wic     = nullptr;
static ID2D1HwndRenderTarget* g_rt      = nullptr;
static ID2D1Bitmap*           g_shot    = nullptr;
static IDWriteTextFormat*     g_tf      = nullptr;  // 面板
static IDWriteTextFormat*     g_tfSmall = nullptr;  // 放大镜面板(小一号)
static IDWriteTextFormat*     g_tfHint  = nullptr;  // 顶部提示
static IDWriteTextFormat*     g_tfIcon  = nullptr;  // 动作图标字形
static IDWriteTextFormat*     g_tfMenu  = nullptr;  // 字体名/下拉(小号,垂直居中)

static int g_vx,g_vy,g_vw,g_vh;
static std::vector<BYTE> g_pixels; static int g_stride=0;

enum Mode { SEL, EDIT };
static Mode g_mode = SEL;
static bool g_selecting=false;
static POINT g_start={0,0}, g_cursor={0,0};
static RECT  g_sel={0,0,0,0};

struct WinRect{ LONG l,t,r,b; };
static std::vector<WinRect> g_windows;
static bool g_haveSnap=false; static RECT g_snap={0,0,0,0};

static const float UI = 2.0f;

// ---- 标注 ----  type: 0=rect 1=rrect 2=ellipse 3=arrow 4=mosaic 5=text
struct Anno {
    int type; float x0,y0,x1,y1; int color; float width;
    bool hasCtrl=false; float cx=0,cy=0;          // 箭头弯折控制点
    int mshape=0; float brush=27;                  // 马赛克：0 方 1 圆 / 笔刷直径
    std::vector<D2D1_POINT_2F> stamps;             // 马赛克笔迹
    std::wstring text; int fontIdx=0; float fontSize=40; // 文字
    float rot=0;                                   // 文字旋转(弧度)
    int id=0;                                      // 稳定唯一 id
    int cid0=0,cnode0=-1,cid1=0,cnode1=-1;         // 箭头端点连接：连到的要素 id + 节点序号
};
static std::vector<Anno> g_annos;
static int   g_tool=-1;          // -1 无, 0 rect,1 rrect,2 ellipse,3 arrow,4 mosaic
static int   g_colorIdx=5;
static int   g_level=1;
static int   g_mshape=0;         // 马赛克形状 0 方 1 圆
static bool  g_drawing=false;
static Anno  g_cur;
static int   g_selArrow=-1;      // 当前显示弯折控制点的箭头
static int   g_dragMode=0;       // 0 无 1 弯折 2 旋转
static float g_rotCx=0,g_rotCy=0,g_rotStart=0; static D2D1_POINT_2F g_rotBase[3];
static int   g_rotIdx=-1; static float g_rotBaseAngle=0;   // 通用旋转(文字)
static bool  g_snapOn=false; static float g_snapX=0,g_snapY=0; // 箭头端点吸附提示
static int   g_arrowEnd=-1;   // 拖动中的箭头端点：0 尾 1 头
static int   g_nextId=1;      // 要素 id 分配器
static int   g_selRz=-1;      // 正在拖动的选区手柄 0..7(-1 无)
static bool  g_selMv=false;   // 正在移动整个选区
static bool  g_fontMenu=false;// 字体下拉是否展开
static HCURSOR g_curCross=nullptr; // 自定义初始光标(取色十字)
// 角点缩放 (dragMode 3)
static int   g_rzCorner=-1; static float g_rzAx=0,g_rzAy=0,g_rzGx=0,g_rzGy=0; static Anno g_rzOrig;
static ID2D1StrokeStyle* g_round=nullptr;
static const float MBRUSH[3]={27,51,84};
static const float TSIZE[3]={24,40,64};

// 文字/字体（动态：扫描 fonts/ 下所有 ttf/otf，可安装/卸载）
static std::vector<std::wstring> g_fontFams;   // 每个文件的字体族名（渲染匹配用）
static std::vector<std::wstring> g_fontNames;  // 下拉显示名
static int  g_fontIdx=0;
// 文字输入：直接用 D2D 绘制(无子窗口，避免闪烁；框随文字自适应)
static bool  g_typing=false;
static std::wstring g_typingText;
static POINT g_typePos={0,0};
static int   g_typeFont=0; static float g_typeSize=40; static int g_typeColor=5;

// 选中/移动/自动粘贴
static int  g_selIdx=-1;         // 当前选中要素（可移动/换色/删除）
static int  g_hoverIdx=-1;       // 悬停的要素（显示可拖动虚线框）
static int  g_hoverBtn=-1;        // 悬停的工具栏按钮（显示名称气泡）
static bool g_moving=false; static POINT g_moveLast={0,0};
static ID2D1StrokeStyle* g_dash=nullptr;
static bool g_pasteAfter=false;  // 退出后自动 Ctrl+V

// 前置声明
static void AnnoBBox(const Anno&a,float&l,float&t,float&r,float&b);
static int  NodesOf(const Anno&a,float ox[8],float oy[8]);
static void CommitText();
// 旋转手柄中心：箭头=右上角；文字=随旋转的右上角并外移一点。返回 false 表示该要素不可旋转
static bool RotHandleCenter(int idx,float&hx,float&hy){
    if(idx<0||idx>=(int)g_annos.size()) return false;
    const Anno&a=g_annos[idx]; if(a.type!=3&&a.type!=5) return false;
    float l,t,r,b; AnnoBBox(a,l,t,r,b);
    if(a.type==3){ // 箭头：跟随箭尖，沿末端切线外移
        float ex=a.hasCtrl?a.cx:a.x0, ey=a.hasCtrl?a.cy:a.y0;
        float dx=a.x1-ex,dy=a.y1-ey,len=hypotf(dx,dy); if(len<1){dx=1;dy=0;len=1;}
        hx=a.x1+dx/len*14*UI; hy=a.y1+dy/len*14*UI; return true; }
    float cx=(l+r)/2,cy=(t+b)/2, ca=cosf(a.rot),sa=sinf(a.rot);
    float dx=r-cx,dy=t-cy, rx=cx+dx*ca-dy*sa, ry=cy+dx*sa+dy*ca;
    float ox2=rx-cx,oy2=ry-cy,len=hypotf(ox2,oy2); if(len<1)len=1;
    hx=rx+ox2/len*9*UI; hy=ry+oy2/len*9*UI; return true;
}

// 马赛克位图(整屏像素化)+ 位图画刷
static std::vector<BYTE> g_mosaicPixels;
static ID2D1Bitmap*      g_mosaicBmp=nullptr;
static ID2D1BitmapBrush* g_mosaicBrush=nullptr;

// —— 液态玻璃：整屏模糊背板(1/4 分辨率) + 位图画刷 + 顶部高光渐变 ——
static std::vector<BYTE>       g_blurPixels; static int g_bw=0,g_bh=0;
static ID2D1Bitmap*            g_blurBmp=nullptr;
static ID2D1BitmapBrush*       g_blurBrush=nullptr;
static ID2D1LinearGradientBrush* g_sheen=nullptr;

// —— 方案 A：工具高亮融流动画 ——
static HWND  g_hwnd=nullptr;
static float g_hlLead=0, g_hlLag=0; static bool g_hlOn=false;

// PNG 图标（与 Python 版一致）：WIC 源(设备无关) + D2D 位图(设备相关)
static std::map<std::string,IWICFormatConverter*> g_iconWic;
static std::map<std::string,ID2D1Bitmap*>         g_iconBmp;
static std::map<std::string,D2D1_RECT_F>          g_iconBox;  // 内容边界(裁掉透明边)

static const D2D1_COLOR_F PALETTE[6] = {
    {0.231f,0.62f,1,1},{0.373f,0.808f,0.231f,1},{1,0.69f,0.125f,1},
    {0.227f,0.247f,0.267f,1},{1,1,1,1},{1,0.357f,0.357f,1}
};
static const float WIDTHS[3] = {3.0f,9.0f,21.0f};

// 工具栏布局(编辑态实时算)
struct Btn { RECT rc; int kind; int id; }; // kind 0=tool 1=undo 2=close 3=confirm ; id: tool 索引
static std::vector<Btn> g_btns;
static RECT g_barRc={0,0,0,0}, g_subRc={0,0,0,0};
struct Sw { RECT rc; int idx; };            // 颜色/粗细 子栏按钮
static std::vector<Sw> g_colSw; static std::vector<Sw> g_lvlSw; static std::vector<Sw> g_shpSw;
static RECT g_fontBtn={0,0,0,0};
static RECT g_fontAddBtn={0,0,0,0};              // 文字：安装字体 ＋
static RECT g_fontDelBtn={0,0,0,0};              // 文字：卸载当前字体 －
static RECT g_sizeSlider={0,0,0,0};              // 文字：字号拖动条轨道
static bool g_sliding=false;                      // 正在拖字号条
static const float SMIN=12.0f, SMAX=96.0f;        // 字号范围

// ---------- 桌面捕获 ----------
static bool CaptureDesktop(){
    g_vx=GetSystemMetrics(SM_XVIRTUALSCREEN); g_vy=GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_vw=GetSystemMetrics(SM_CXVIRTUALSCREEN); g_vh=GetSystemMetrics(SM_CYVIRTUALSCREEN);
    HDC hs=CreateDCW(L"DISPLAY",nullptr,nullptr,nullptr); HDC hm=CreateCompatibleDC(hs);
    HBITMAP hb=CreateCompatibleBitmap(hs,g_vw,g_vh); HGDIOBJ old=SelectObject(hm,hb);
    BitBlt(hm,0,0,g_vw,g_vh,hs,g_vx,g_vy,SRCCOPY|CAPTUREBLT);
    BITMAPINFO bi; ZeroMemory(&bi,sizeof(bi));
    bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth=g_vw;
    bi.bmiHeader.biHeight=-g_vh; bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32;
    bi.bmiHeader.biCompression=BI_RGB; g_stride=g_vw*4;
    g_pixels.resize((size_t)g_stride*g_vh);
    GetDIBits(hm,hb,0,g_vh,g_pixels.data(),&bi,DIB_RGB_COLORS);
    SelectObject(hm,old); DeleteObject(hb); DeleteDC(hm); DeleteDC(hs);
    return !g_pixels.empty();
}

// ---------- 马赛克像素(整屏一次性像素化)----------
static void ComputeMosaic(){
    const int B=15; g_mosaicPixels.assign(g_pixels.size(),0);
    for(int by=0; by<g_vh; by+=B){
        int bh=min(B,g_vh-by);
        for(int bx=0; bx<g_vw; bx+=B){
            int bw=min(B,g_vw-bx);
            long sr=0,sg=0,sb=0,cnt=bw*bh;
            for(int y=0;y<bh;y++){ const BYTE* p=&g_pixels[(size_t)(by+y)*g_stride+(size_t)bx*4];
                for(int x=0;x<bw;x++){ sb+=p[0]; sg+=p[1]; sr+=p[2]; p+=4; } }
            BYTE ab=(BYTE)(sb/cnt),ag=(BYTE)(sg/cnt),ar=(BYTE)(sr/cnt);
            for(int y=0;y<bh;y++){ BYTE* p=&g_mosaicPixels[(size_t)(by+y)*g_stride+(size_t)bx*4];
                for(int x=0;x<bw;x++){ p[0]=ab; p[1]=ag; p[2]=ar; p[3]=255; p+=4; } }
        }
    }
}

// 生成 1/4 分辨率的整屏模糊背板（液态玻璃背景）。降采样 + 多趟盒式模糊 ≈ 高斯。
static void ComputeBlur(){
    g_bw=(g_vw+3)/4; g_bh=(g_vh+3)/4;
    std::vector<BYTE> small_((size_t)g_bw*g_bh*4);
    // 4x4 均值降采样
    for(int y=0;y<g_bh;y++){ for(int x=0;x<g_bw;x++){
        int sx=x*4, sy=y*4; long sr=0,sg=0,sb=0,c=0;
        for(int j=0;j<4;j++){ int yy=sy+j; if(yy>=g_vh)break; const BYTE* p=&g_pixels[(size_t)yy*g_stride+(size_t)sx*4];
            for(int i=0;i<4;i++){ int xx=sx+i; if(xx>=g_vw)break; sb+=p[0];sg+=p[1];sr+=p[2]; p+=4; c++; } }
        BYTE* d=&small_[((size_t)y*g_bw+x)*4]; if(c<1)c=1; d[0]=(BYTE)(sb/c);d[1]=(BYTE)(sg/c);d[2]=(BYTE)(sr/c);d[3]=255;
    }}
    // 可分离盒式模糊（水平+垂直），跑 3 趟
    std::vector<BYTE> tmp(small_.size()); const int R=3;
    auto boxH=[&](std::vector<BYTE>&src,std::vector<BYTE>&dst){
        for(int y=0;y<g_bh;y++){ for(int x=0;x<g_bw;x++){ long sr=0,sg=0,sb=0,c=0;
            for(int k=-R;k<=R;k++){ int xx=x+k; if(xx<0)xx=0; if(xx>=g_bw)xx=g_bw-1; const BYTE* p=&src[((size_t)y*g_bw+xx)*4]; sb+=p[0];sg+=p[1];sr+=p[2];c++; }
            BYTE* d=&dst[((size_t)y*g_bw+x)*4]; d[0]=(BYTE)(sb/c);d[1]=(BYTE)(sg/c);d[2]=(BYTE)(sr/c);d[3]=255; } } };
    auto boxV=[&](std::vector<BYTE>&src,std::vector<BYTE>&dst){
        for(int y=0;y<g_bh;y++){ for(int x=0;x<g_bw;x++){ long sr=0,sg=0,sb=0,c=0;
            for(int k=-R;k<=R;k++){ int yy=y+k; if(yy<0)yy=0; if(yy>=g_bh)yy=g_bh-1; const BYTE* p=&src[((size_t)yy*g_bw+x)*4]; sb+=p[0];sg+=p[1];sr+=p[2];c++; }
            BYTE* d=&dst[((size_t)y*g_bw+x)*4]; d[0]=(BYTE)(sb/c);d[1]=(BYTE)(sg/c);d[2]=(BYTE)(sr/c);d[3]=255; } } };
    for(int pass=0;pass<3;pass++){ boxH(small_,tmp); boxV(tmp,small_); }
    g_blurPixels.swap(small_);
}

// ---------- 磁吸 ----------
static RECT DwmRect(HWND h){ RECT rc; if(FAILED(DwmGetWindowAttribute(h,DWMWA_EXTENDED_FRAME_BOUNDS,&rc,sizeof(rc)))) GetWindowRect(h,&rc); return rc; }
static BOOL CALLBACK EnumProc(HWND h,LPARAM){ if(!IsWindowVisible(h)||IsIconic(h)) return TRUE; RECT rc=DwmRect(h); if(rc.right-rc.left<80||rc.bottom-rc.top<60) return TRUE; g_windows.push_back({rc.left,rc.top,rc.right,rc.bottom}); return TRUE; }
static void EnumWindowsForSnap(){ g_windows.clear(); EnumWindows(EnumProc,0); }
static bool SnapAt(int cx,int cy,RECT& out){ int ax=cx+g_vx, ay=cy+g_vy; for(auto&w:g_windows){ if(ax>=w.l&&ax<=w.r&&ay>=w.t&&ay<=w.b){ out={w.l-g_vx,w.t-g_vy,w.r-g_vx,w.b-g_vy}; return true; } } return false; }

// ---------- D2D ----------
static D2D1_COLOR_F Col(int r,int g,int b,float a=1){ return D2D1::ColorF(r/255.0f,g/255.0f,b/255.0f,a); }
static void GetPixel(int x,int y,int&r,int&g,int&b){ x=x<0?0:(x>=g_vw?g_vw-1:x); y=y<0?0:(y>=g_vh?g_vh-1:y); const BYTE*p=&g_pixels[(size_t)y*g_stride+(size_t)x*4]; b=p[0]; g=p[1]; r=p[2]; }

static void CreateDeviceResources(HWND hwnd){
    if(g_rt) return;
    RECT rc; GetClientRect(hwnd,&rc);
    // 关键：强制渲染目标 96 DPI，使 D2D 坐标(DIP)与物理像素 1:1，
    // 否则高分屏(如 150%)会把按物理像素绘制的内容整体放大，只显示左上一角。
    g_factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN,D2D1_ALPHA_MODE_UNKNOWN), 96.0f, 96.0f),
        D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right-rc.left,rc.bottom-rc.top),
            D2D1_PRESENT_OPTIONS_IMMEDIATELY), &g_rt);
    if(!g_rt) return;
    D2D1_BITMAP_PROPERTIES bp=D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE));
    g_rt->CreateBitmap(D2D1::SizeU(g_vw,g_vh), g_pixels.data(), g_stride, bp, &g_shot);
    // 马赛克位图 + 位图画刷（用它填充笔迹形状即得像素化贴片）
    g_rt->CreateBitmap(D2D1::SizeU(g_vw,g_vh), g_mosaicPixels.data(), g_stride, bp, &g_mosaicBmp);
    if(g_mosaicBmp){
        g_rt->CreateBitmapBrush(g_mosaicBmp,
            D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP,D2D1_EXTEND_MODE_CLAMP,
                D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR),
            D2D1::BrushProperties(), &g_mosaicBrush);
    }
    for(auto&kv:g_iconWic){ ID2D1Bitmap* bm=nullptr; if(SUCCEEDED(g_rt->CreateBitmapFromWicBitmap(kv.second,nullptr,&bm))&&bm) g_iconBmp[kv.first]=bm; }
    // 液态玻璃背板：模糊位图 + 位图画刷(缩放回全屏) + 顶部高光渐变
    if(!g_blurPixels.empty()){
        g_rt->CreateBitmap(D2D1::SizeU(g_bw,g_bh), g_blurPixels.data(), g_bw*4, bp, &g_blurBmp);
        if(g_blurBmp){
            g_rt->CreateBitmapBrush(g_blurBmp,
                D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP,D2D1_EXTEND_MODE_CLAMP,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
                D2D1::BrushProperties(), &g_blurBrush);
            if(g_blurBrush) g_blurBrush->SetTransform(D2D1::Matrix3x2F::Scale((float)g_vw/g_bw,(float)g_vh/g_bh));
        }
    }
    ID2D1GradientStopCollection* gsc=nullptr; D2D1_GRADIENT_STOP gs[2]={
        {0.0f, D2D1::ColorF(1,1,1,0.55f)}, {1.0f, D2D1::ColorF(1,1,1,0.0f)} };
    if(SUCCEEDED(g_rt->CreateGradientStopCollection(gs,2,&gsc))&&gsc){
        g_rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,0),D2D1::Point2F(0,1)),gsc,&g_sheen);
        gsc->Release();
    }
}
static void DiscardDeviceResources(){ for(auto&kv:g_iconBmp) if(kv.second) kv.second->Release(); g_iconBmp.clear(); SafeRelease(&g_sheen); SafeRelease(&g_blurBrush); SafeRelease(&g_blurBmp); SafeRelease(&g_mosaicBrush); SafeRelease(&g_mosaicBmp); SafeRelease(&g_shot); SafeRelease(&g_rt); }

// 二次贝塞尔取点
static D2D1_POINT_2F Bez(float x0,float y0,float cx,float cy,float x1,float y1,float t){
    float mt=1-t; return D2D1::Point2F(mt*mt*x0+2*mt*t*cx+t*t*x1, mt*mt*y0+2*mt*t*cy+t*t*y1);
}
// 彗星箭头：沿贝塞尔构造连续渐宽的带状(平滑无颗粒) + 线性渐变淡出 + 倒刺箭头
static void BuildComet(ID2D1RenderTarget* rt, const Anno& a, ID2D1SolidColorBrush* br, float ox, float oy){
    float x0=a.x0+ox,y0=a.y0+oy,x1=a.x1+ox,y1=a.y1+oy;
    float cx = a.hasCtrl? a.cx+ox : (x0+x1)/2, cy = a.hasCtrl? a.cy+oy : (y0+y1)/2;
    float w=a.width; D2D1_COLOR_F c=PALETTE[a.color];
    const int N=64;
    std::vector<D2D1_POINT_2F> pts(N+1); std::vector<float> cum(N+1,0);
    pts[0]=Bez(x0,y0,cx,cy,x1,y1,0); float total=0;
    for(int i=1;i<=N;i++){ pts[i]=Bez(x0,y0,cx,cy,x1,y1,(float)i/N); total+=hypotf(pts[i].x-pts[i-1].x,pts[i].y-pts[i-1].y); cum[i]=total; }
    if(total<1) total=1;
    float neck_w=max(1.2f,0.9f*w), head_w=5+1.9f*w, head_len=min(total*0.5f,10+3.0f*w);
    int neck=N; { float acc=0; for(int i=N-1;i>=0;i--){ acc+=hypotf(pts[i+1].x-pts[i].x,pts[i+1].y-pts[i].y); if(acc>=head_len*0.62f){neck=i;break;} } }
    auto normal=[&](int i)->D2D1_POINT_2F{ int a1=max(0,i-1),b1=min(N,i+1); float tx=pts[b1].x-pts[a1].x,ty=pts[b1].y-pts[a1].y; float L=hypotf(tx,ty); if(L<1e-3f)L=1; return D2D1::Point2F(-ty/L,tx/L); };
    std::vector<D2D1_POINT_2F> Lp(neck+1),Rp(neck+1);
    for(int i=0;i<=neck;i++){ float s=cum[i]/total; float hw=neck_w*powf(s,0.72f); D2D1_POINT_2F nn=normal(i); Lp[i]=D2D1::Point2F(pts[i].x+nn.x*hw,pts[i].y+nn.y*hw); Rp[i]=D2D1::Point2F(pts[i].x-nn.x*hw,pts[i].y-nn.y*hw); }
    // 尾透明→头浓 的线性渐变画刷
    ID2D1GradientStopCollection* gsc=nullptr; ID2D1LinearGradientBrush* lg=nullptr;
    D2D1_GRADIENT_STOP stops[2]={{0.0f,D2D1::ColorF(c.r,c.g,c.b,0.12f)},{1.0f,D2D1::ColorF(c.r,c.g,c.b,1.0f)}};
    rt->CreateGradientStopCollection(stops,2,&gsc);
    if(gsc) rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(pts[0],pts[N]),gsc,&lg);
    ID2D1Brush* fill = lg? (ID2D1Brush*)lg : (ID2D1Brush*)br; if(!lg) br->SetColor(c);
    // 连续带状彗尾
    ID2D1PathGeometry* pg=nullptr; g_factory->CreatePathGeometry(&pg); ID2D1GeometrySink* sk=nullptr; pg->Open(&sk);
    sk->BeginFigure(Lp[0],D2D1_FIGURE_BEGIN_FILLED);
    for(int i=1;i<=neck;i++) sk->AddLine(Lp[i]);
    for(int i=neck;i>=0;i--) sk->AddLine(Rp[i]);
    sk->EndFigure(D2D1_FIGURE_END_CLOSED); sk->Close(); sk->Release();
    rt->FillGeometry(pg,fill); pg->Release();
    // 倒刺箭头（底部凹口落在颈部曲线点，浑然一体）
    D2D1_POINT_2F tip=pts[N], Pn=pts[neck];
    float dx=tip.x-Pn.x,dy=tip.y-Pn.y; float L=hypotf(dx,dy); if(L<1)L=1; float ux=dx/L,uy=dy/L,pxp=-uy,pyp=ux;
    D2D1_POINT_2F baseC=D2D1::Point2F(tip.x-ux*head_len,tip.y-uy*head_len);
    ID2D1PathGeometry* hg=nullptr; g_factory->CreatePathGeometry(&hg); ID2D1GeometrySink* hs=nullptr; hg->Open(&hs);
    hs->BeginFigure(D2D1::Point2F(baseC.x+pxp*head_w,baseC.y+pyp*head_w),D2D1_FIGURE_BEGIN_FILLED);
    hs->AddLine(tip); hs->AddLine(D2D1::Point2F(baseC.x-pxp*head_w,baseC.y-pyp*head_w)); hs->AddLine(Pn);
    hs->EndFigure(D2D1_FIGURE_END_CLOSED); hs->Close(); hs->Release();
    br->SetColor(c); rt->FillGeometry(hg,br); hg->Release();
    SafeRelease(&lg); SafeRelease(&gsc);
}

// ---- 文字/字体（自定义字体集：直接从 ttf 文件加载，确保生效）----
static std::vector<std::wstring> g_fontFiles;
static IDWriteFontCollection* g_fontColl=nullptr;
struct FontFileEnum : public IDWriteFontFileEnumerator {
    LONG ref=1; IDWriteFactory* f; size_t i=(size_t)-1; IDWriteFontFile* cur=nullptr;
    FontFileEnum(IDWriteFactory* fac):f(fac){}
    HRESULT __stdcall QueryInterface(REFIID riid,void**pp){ if(riid==__uuidof(IUnknown)||riid==__uuidof(IDWriteFontFileEnumerator)){*pp=this;AddRef();return S_OK;} *pp=nullptr;return E_NOINTERFACE; }
    ULONG __stdcall AddRef(){ return InterlockedIncrement(&ref); }
    ULONG __stdcall Release(){ ULONG r=InterlockedDecrement(&ref); if(!r){ if(cur)cur->Release(); delete this; } return r; }
    HRESULT __stdcall MoveNext(BOOL* has){ i++; if(cur){cur->Release();cur=nullptr;} if(i<g_fontFiles.size()) f->CreateFontFileReference(g_fontFiles[i].c_str(),nullptr,&cur); *has=(i<g_fontFiles.size()); return S_OK; }
    HRESULT __stdcall GetCurrentFontFile(IDWriteFontFile** ff){ if(cur) cur->AddRef(); *ff=cur; return S_OK; }
};
struct FontCollLoader : public IDWriteFontCollectionLoader {
    LONG ref=1;
    HRESULT __stdcall QueryInterface(REFIID riid,void**pp){ if(riid==__uuidof(IUnknown)||riid==__uuidof(IDWriteFontCollectionLoader)){*pp=this;AddRef();return S_OK;} *pp=nullptr;return E_NOINTERFACE; }
    ULONG __stdcall AddRef(){ return InterlockedIncrement(&ref); }
    ULONG __stdcall Release(){ ULONG r=InterlockedDecrement(&ref); if(!r) delete this; return r; }
    HRESULT __stdcall CreateEnumeratorFromKey(IDWriteFactory* factory,const void*,UINT32,IDWriteFontFileEnumerator** en){ *en=new FontFileEnum(factory); return S_OK; }
};
static FontCollLoader* g_fontLoader=nullptr;

// ---- 文字/字体 ----
static std::map<long long,IDWriteTextFormat*> g_tfCache;
static IDWriteTextFormat* GetTextFmt(int fi,int sz){
    if(fi<0||fi>=(int)g_fontFams.size()) return nullptr;
    long long key=(long long)fi*100000+sz; auto it=g_tfCache.find(key); if(it!=g_tfCache.end()) return it->second;
    IDWriteTextFormat* f=nullptr;
    g_dwrite->CreateTextFormat(g_fontFams[fi].c_str(),g_fontColl,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,(float)sz,L"zh-cn",&f);
    if(f) g_tfCache[key]=f; return f;
}
static void TextSize(const Anno&a,float&w,float&h){
    IDWriteTextFormat* f=GetTextFmt(a.fontIdx,(int)a.fontSize); if(!f){w=h=0;return;}
    IDWriteTextLayout* lay=nullptr; g_dwrite->CreateTextLayout(a.text.c_str(),(UINT32)a.text.size(),f,4000,4000,&lay);
    if(!lay){w=h=0;return;} DWRITE_TEXT_METRICS m; lay->GetMetrics(&m); w=m.width; h=m.height; lay->Release();
}
static std::wstring ExeDir(){ wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr,p,MAX_PATH); std::wstring s(p); size_t k=s.find_last_of(L"\\/"); return k==std::wstring::npos?L"":s.substr(0,k+1); }
static std::wstring FontsDir(){ return ExeDir()+L"fonts\\"; }
static std::wstring FamilyOfFile(const std::wstring& path){
    std::wstring fam; IDWriteFontFile* file=nullptr;
    if(FAILED(g_dwrite->CreateFontFileReference(path.c_str(),nullptr,&file))||!file) return fam;
    BOOL sup=FALSE; DWRITE_FONT_FILE_TYPE ft; DWRITE_FONT_FACE_TYPE fat; UINT32 num=0;
    if(SUCCEEDED(file->Analyze(&sup,&ft,&fat,&num)) && sup && num>=1){
        IDWriteFontFace* face=nullptr;
        if(SUCCEEDED(g_dwrite->CreateFontFace(fat,1,&file,0,DWRITE_FONT_SIMULATIONS_NONE,&face)) && face){
            IDWriteFontFace3* f3=nullptr;
            if(SUCCEEDED(face->QueryInterface(__uuidof(IDWriteFontFace3),(void**)&f3)) && f3){
                IDWriteLocalizedStrings* names=nullptr;
                if(SUCCEEDED(f3->GetFamilyNames(&names)) && names){
                    UINT32 idx=0; BOOL ex=FALSE; names->FindLocaleName(L"zh-cn",&idx,&ex); if(!ex){ names->FindLocaleName(L"en-us",&idx,&ex); if(!ex) idx=0; }
                    UINT32 len=0; if(SUCCEEDED(names->GetStringLength(idx,&len))){ std::vector<wchar_t> buf(len+1,0); if(SUCCEEDED(names->GetString(idx,buf.data(),len+1))) fam=buf.data(); }
                    names->Release();
                }
                f3->Release();
            }
            face->Release();
        }
    }
    file->Release(); return fam;
}
static std::wstring StemOf(const std::wstring& p){ std::wstring s=p; size_t k=s.find_last_of(L"\\/"); if(k!=std::wstring::npos) s=s.substr(k+1); size_t d=s.find_last_of(L'.'); if(d!=std::wstring::npos) s=s.substr(0,d); return s; }
static std::wstring CleanName(const std::wstring& fam){
    for(size_t i=0;i<fam.size();++i){ if(fam[i]>=L'0'&&fam[i]<=L'9'){ bool cjk=false; for(size_t j=0;j<i;j++) if((unsigned short)fam[j]>127){cjk=true;break;}
        if(cjk){ std::wstring s=fam.substr(0,i); while(!s.empty()&&s.back()==L' ') s.pop_back(); if(!s.empty()) return s; } break; } }
    return fam;
}
static void ScanFontFiles(){
    g_fontFiles.clear(); std::wstring d=FontsDir();
    const wchar_t* pats[]={L"*.ttf",L"*.otf",L"*.ttc"};
    for(auto pat:pats){ WIN32_FIND_DATAW fd; HANDLE h=FindFirstFileW((d+pat).c_str(),&fd);
        if(h!=INVALID_HANDLE_VALUE){ do{ if(!(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) g_fontFiles.push_back(d+fd.cFileName); }while(FindNextFileW(h,&fd)); FindClose(h); } }
    std::sort(g_fontFiles.begin(),g_fontFiles.end());
}
static int g_collKey=0;
static bool g_loaderReg=false;
static void BuildFontCollection(){
    for(auto&kv:g_tfCache) if(kv.second) kv.second->Release(); g_tfCache.clear();
    SafeRelease(&g_fontColl);
    g_fontFams.clear(); g_fontNames.clear();
    for(auto&p:g_fontFiles){ AddFontResourceExW(p.c_str(),0,0);
        std::wstring fam=FamilyOfFile(p); if(fam.empty()) fam=StemOf(p);
        g_fontFams.push_back(fam); g_fontNames.push_back(CleanName(fam)); }
    if(!g_fontLoader) g_fontLoader=new FontCollLoader();
    if(!g_loaderReg){ if(SUCCEEDED(g_dwrite->RegisterFontCollectionLoader(g_fontLoader))) g_loaderReg=true; }
    if(g_loaderReg && !g_fontFiles.empty()){ wchar_t key[32]; swprintf(key,32,L"tato%d",++g_collKey);
        g_dwrite->CreateCustomFontCollection(g_fontLoader,key,(UINT32)((wcslen(key)+1)*sizeof(wchar_t)),&g_fontColl); }
    if(g_fontIdx>=(int)g_fontFams.size()) g_fontIdx=g_fontFams.empty()?0:(int)g_fontFams.size()-1;
    if(g_fontIdx<0) g_fontIdx=0;
}
static void RegisterFonts(){ ScanFontFiles(); BuildFontCollection(); }
static void LayoutToolbar();
static void InstallFont(HWND hwnd){
    wchar_t path[MAX_PATH]=L""; OPENFILENAMEW ofn; ZeroMemory(&ofn,sizeof(ofn)); ofn.lStructSize=sizeof(ofn);
    ofn.hwndOwner=hwnd; ofn.lpstrFilter=L"TTF 字体\0*.ttf\0所有字体\0*.ttf;*.otf;*.ttc\0所有文件\0*.*\0"; ofn.lpstrFile=path; ofn.nMaxFile=MAX_PATH; ofn.lpstrDefExt=L"ttf"; ofn.nFilterIndex=1; ofn.Flags=OFN_FILEMUSTEXIST|OFN_EXPLORER;
    SetWindowPos(hwnd,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
    BOOL ok=GetOpenFileNameW(&ofn);
    SetWindowPos(hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE); SetForegroundWindow(hwnd);
    if(ok){ std::wstring src=path; std::wstring name=src.substr(src.find_last_of(L"\\/")+1); std::wstring dst=FontsDir()+name;
        CreateDirectoryW(FontsDir().c_str(),nullptr);
        if(CopyFileW(src.c_str(),dst.c_str(),FALSE)){ RegisterFonts();
            for(size_t i=0;i<g_fontFiles.size();++i) if(_wcsicmp(g_fontFiles[i].c_str(),dst.c_str())==0){ g_fontIdx=(int)i; break; }
            if(g_selIdx>=0&&g_selIdx<(int)g_annos.size()&&g_annos[g_selIdx].type==5) g_annos[g_selIdx].fontIdx=g_fontIdx;
        } else MessageBoxW(hwnd,L"复制字体失败",L"TatoMark",MB_OK|MB_ICONERROR); }
    LayoutToolbar();
}
static void UninstallFont(HWND hwnd){
    if(g_fontIdx<0||g_fontIdx>=(int)g_fontFiles.size()) return;
    std::wstring path=g_fontFiles[g_fontIdx];
    std::wstring msg=L"确定卸载字体：\n"+g_fontNames[g_fontIdx]+L"\n将从 fonts 文件夹删除该文件。";
    SetWindowPos(hwnd,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
    int r=MessageBoxW(hwnd,msg.c_str(),L"TatoMark",MB_OKCANCEL|MB_ICONWARNING);
    SetWindowPos(hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE); SetForegroundWindow(hwnd);
    if(r!=IDOK) return;
    for(auto&kv:g_tfCache) if(kv.second) kv.second->Release(); g_tfCache.clear(); SafeRelease(&g_fontColl);
    RemoveFontResourceExW(path.c_str(),0,0);
    if(!DeleteFileW(path.c_str())) MessageBoxW(hwnd,L"删除文件失败(可能被占用)，可稍后手动删除",L"TatoMark",MB_OK|MB_ICONWARNING);
    RegisterFonts();
    if(g_selIdx>=0&&g_selIdx<(int)g_annos.size()&&g_annos[g_selIdx].type==5 && g_fontIdx<(int)g_fontFams.size()) g_annos[g_selIdx].fontIdx=g_fontIdx;
    LayoutToolbar();
}
static IWICFormatConverter* LoadIconWic(const std::wstring& path){
    IWICBitmapDecoder* dec=nullptr;
    if(FAILED(g_wic->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&dec))||!dec) return nullptr;
    IWICBitmapFrameDecode* fr=nullptr; dec->GetFrame(0,&fr);
    IWICFormatConverter* conv=nullptr; g_wic->CreateFormatConverter(&conv);
    if(conv&&fr) conv->Initialize(fr,GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
    if(fr) fr->Release(); dec->Release(); return conv;
}
static void LoadIcons(){
    std::wstring d=ExeDir()+L"icon\\";
    struct{const char*k; const wchar_t*f;} defs[]={
        {"rect",L"直方框.png"},{"rrect",L"圆角方框.png"},{"ellipse",L"圆圈.png"},
        {"arrow",L"长箭头-右上.png"},{"mosaic",L"马赛克.png"},{"text",L"文本块.png"},
        {"undo",L"撤销.png"},{"save",L"下载.png"},{"close",L"取消.png"},{"confirm",L"确定.png"}};
    // 旋转图标：优先用新命名的“顺时针旋转.png”，否则回退“顺时针方向.png”
    { IWICFormatConverter* rc=LoadIconWic(d+L"顺时针旋转.png"); if(!rc) rc=LoadIconWic(d+L"顺时针方向.png");
      if(rc){ g_iconWic["rotate"]=rc; UINT w=0,h=0; rc->GetSize(&w,&h); if(w&&h){ std::vector<BYTE> buf((size_t)w*4*h); rc->CopyPixels(nullptr,w*4,(UINT)buf.size(),buf.data());
        int l=w,t=h,r=-1,b=-1; for(UINT y=0;y<h;y++){ const BYTE* p=&buf[(size_t)y*w*4+3]; for(UINT x=0;x<w;x++){ if(*p>40){ if((int)x<l)l=x; if((int)x>r)r=x; if((int)y<t)t=y; if((int)y>b)b=y; } p+=4; } }
        if(r<l){l=0;t=0;r=(int)w-1;b=(int)h-1;} g_iconBox["rotate"]=D2D1::RectF((float)l,(float)t,(float)(r+1),(float)(b+1)); } } }
    for(auto&e:defs){ IWICFormatConverter* c=LoadIconWic(d+e.f); if(!c) continue; g_iconWic[e.k]=c;
        UINT w=0,h=0; c->GetSize(&w,&h); if(!w||!h){ continue; }
        std::vector<BYTE> buf((size_t)w*4*h); c->CopyPixels(nullptr,w*4,(UINT)buf.size(),buf.data());
        int l=w,t=h,r=-1,b=-1;
        for(UINT y=0;y<h;y++){ const BYTE* p=&buf[(size_t)y*w*4+3]; for(UINT x=0;x<w;x++){ if(*p>40){ if((int)x<l)l=x; if((int)x>r)r=x; if((int)y<t)t=y; if((int)y>b)b=y; } p+=4; } }
        if(r<l){ l=0;t=0;r=(int)w-1;b=(int)h-1; }
        g_iconBox[e.k]=D2D1::RectF((float)l,(float)t,(float)(r+1),(float)(b+1));
    }
}

// 通用形状绘制（屏幕 & 导出共用），offset 用于导出时平移到选区局部坐标
static void DrawShape(ID2D1RenderTarget* rt, const Anno& a, ID2D1SolidColorBrush* br,
                      float ox, float oy, ID2D1BitmapBrush* mbrush){
    if(a.type==5){ IDWriteTextFormat* f=GetTextFmt(a.fontIdx,(int)a.fontSize); if(f){ br->SetColor(PALETTE[a.color]);
            D2D1_MATRIX_3X2_F oldm; bool rotd=(fabsf(a.rot)>0.0001f);
            if(rotd){ float tw,th; TextSize(a,tw,th); float ccx=a.x0+ox+tw/2, ccy=a.y0+oy+th/2;
                rt->GetTransform(&oldm); rt->SetTransform(D2D1::Matrix3x2F::Rotation(a.rot*57.29578f,D2D1::Point2F(ccx,ccy))*oldm); }
            rt->DrawText(a.text.c_str(),(UINT32)a.text.size(),f,D2D1::RectF(a.x0+ox,a.y0+oy,a.x0+ox+4000,a.y0+oy+4000),br);
            if(rotd) rt->SetTransform(oldm);
        } return; }
    if(a.type==4){                                   // 马赛克：用位图画刷填充笔迹
        if(!mbrush) return;
        // 位图画刷按屏幕坐标采样；导出时整体平移(ox,oy)使其对齐选区
        mbrush->SetTransform(D2D1::Matrix3x2F::Translation(ox,oy));
        float r=a.brush/2;
        for(auto& s:a.stamps){ float px=s.x+ox, py=s.y+oy;
            if(a.mshape==1) rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px,py),r,r),mbrush);
            else rt->FillRectangle(D2D1::RectF(px-r,py-r,px+r,py+r),mbrush);
        }
        return;
    }
    if(a.type==3){ BuildComet(rt,a,br,ox,oy); return; }
    br->SetColor(PALETTE[a.color]);
    float x0=min(a.x0,a.x1)+ox, y0=min(a.y0,a.y1)+oy, x1=max(a.x0,a.x1)+ox, y1=max(a.y0,a.y1)+oy;
    if(a.type==0){
        rt->DrawRectangle(D2D1::RectF(x0,y0,x1,y1), br, a.width);
    } else if(a.type==1){
        float r=min(24.0f, min(x1-x0,y1-y0)*0.25f);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0,y0,x1,y1), r, r), br, a.width);
    } else if(a.type==2){
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((x0+x1)/2,(y0+y1)/2),(x1-x0)/2,(y1-y0)/2), br, a.width);
    }
}

// ---------- 工具栏布局 ----------
static void LayoutToolbar(){
    g_btns.clear(); g_colSw.clear(); g_lvlSw.clear(); g_shpSw.clear();
    float btn=28*UI, gap=7*UI, sep=14*UI, pad=6*UI;  // 更紧凑精致
    // 顺序：rect rrect ellipse | undo | close confirm
    float w=pad;
    struct Item{int kind;int id;}; std::vector<Item> items={
        {0,0},{0,1},{0,2},{0,3},{0,4},{0,5},{1,0},{4,0},{2,0},{3,0}};  // 统一间距，去掉分组分隔
    std::vector<float> xs;
    for(size_t i=0;i<items.size();++i){ if(items[i].kind==-1){ w+=sep; } else { xs.push_back(w); w+=btn+gap; } }
    w+=pad-gap;
    float h=btn+pad*2;
    float tx=min((float)max((long)(g_sel.right-w),8L),(float)(g_vw-w-8));
    float ty=g_sel.bottom+14;
    if(ty+h+56*UI>g_vh-8){ ty=g_sel.top-h-14; if(ty<8) ty=8; }
    g_barRc={(LONG)tx,(LONG)ty,(LONG)(tx+w),(LONG)(ty+h)};
    // 按钮矩形
    size_t xi=0;
    for(auto&it:items){ if(it.kind==-1) continue; float bx=tx+xs[xi++]; RECT r={(LONG)bx,(LONG)(ty+pad),(LONG)(bx+btn),(LONG)(ty+pad+btn)}; g_btns.push_back({r,it.kind,it.id}); }
    // 子栏（选中工具时）：粗细 + 颜色；马赛克则粗细 + 形状(方/圆)
    if(g_tool>=0){
        bool mo=(g_tool==4), tx5=(g_tool==5);
        float sh=40*UI, step=28*UI, sp=12*UI, fw=tx5?128*UI:0, slW=120*UI;
        float cw = mo ? (sp+3*step+14*UI+2*step+sp)
                 : tx5 ? (sp+slW+14*UI+6*step+fw+sp)
                       : (sp+3*step+14*UI+6*step+sp);
        float sx=min((float)max((long)tx,8L),(float)(g_vw-cw-8));
        float sy=ty+h+10;
        if(sy+sh>g_vh-8) sy=ty-sh-10;
        g_subRc={(LONG)sx,(LONG)sy,(LONG)(sx+cw),(LONG)(sy+sh)};
        float x=sx+sp, cy=sy+sh/2; g_fontBtn={0,0,0,0}; g_fontAddBtn={0,0,0,0}; g_fontDelBtn={0,0,0,0}; g_sizeSlider={0,0,0,0};
        if(tx5){ // 文字：字号拖动条（替代三档圆点）
            g_sizeSlider={(LONG)x,(LONG)(cy-step/2),(LONG)(x+slW),(LONG)(cy+step/2)}; x+=slW;
        } else {
            for(int i=0;i<3;i++){ RECT r={(LONG)x,(LONG)(cy-step/2),(LONG)(x+step),(LONG)(cy+step/2)}; g_lvlSw.push_back({r,i}); x+=step; }
        }
        x+=14*UI;
        if(mo){ for(int i=0;i<2;i++){ RECT r={(LONG)x,(LONG)(cy-step/2),(LONG)(x+step),(LONG)(cy+step/2)}; g_shpSw.push_back({r,i}); x+=step; } }
        else  { for(int i=0;i<6;i++){ RECT r={(LONG)x,(LONG)(cy-step/2),(LONG)(x+step),(LONG)(cy+step/2)}; g_colSw.push_back({r,i}); x+=step; }
                if(tx5){ x+=8*UI; g_fontBtn={(LONG)x,(LONG)(cy-14*UI),(LONG)(x+fw-8*UI),(LONG)(cy+14*UI)}; } }
    } else g_subRc={0,0,0,0};
}
static bool PtIn(const RECT&r,int x,int y){ return x>=r.left&&x<=r.right&&y>=r.top&&y<=r.bottom; }
// 选区 8 手柄命中(0TL 1TM 2TR 3RM 4BR 5BM 6BL 7LM)
static int SelHandleAt(int x,int y){ RECT s=g_sel; float mx=(s.left+s.right)/2.0f,my=(s.top+s.bottom)/2.0f;
    float px[8]={(float)s.left,mx,(float)s.right,(float)s.right,(float)s.right,mx,(float)s.left,(float)s.left};
    float py[8]={(float)s.top,(float)s.top,(float)s.top,my,(float)s.bottom,(float)s.bottom,(float)s.bottom,my};
    float r=9*UI; for(int i=0;i<8;i++){ float dx=x-px[i],dy=y-py[i]; if(dx*dx+dy*dy<=r*r) return i; } return -1; }
static bool SelBorderHit(int x,int y){ RECT s=g_sel; float b=6*UI;
    bool nearX=(fabsf((float)x-s.left)<=b||fabsf((float)x-s.right)<=b), nearY=(fabsf((float)y-s.top)<=b||fabsf((float)y-s.bottom)<=b);
    bool inX=(x>=s.left-b&&x<=s.right+b), inY=(y>=s.top-b&&y<=s.bottom+b); return (nearX&&inY)||(nearY&&inX); }
static int FontMenuLayout(RECT& panel, std::vector<RECT>& rows){
    rows.clear(); if(g_fontBtn.right<=g_fontBtn.left) return 0;
    int nf=(int)g_fontNames.size(), total=nf+2;
    float rowH=26*UI, pad=4*UI, w=max((float)(g_fontBtn.right-g_fontBtn.left),150.0f*UI);
    float x=(float)g_fontBtn.left, H=total*rowH+pad*2, top=(float)g_fontBtn.bottom+6*UI;
    if(top+H>g_vh-6){ top=(float)g_fontBtn.top-H-6*UI; if(top<6) top=6; }
    panel={(LONG)x,(LONG)top,(LONG)(x+w),(LONG)(top+H)};
    float ry=top+pad; for(int i=0;i<total;i++){ rows.push_back({(LONG)(x+3*UI),(LONG)ry,(LONG)(x+w-3*UI),(LONG)(ry+rowH)}); ry+=rowH; }
    return total;
}

// ---------- 绘制工具栏（苹果液态玻璃）----------
static void PaintPill(const RECT& r, ID2D1SolidColorBrush* br){
    float rad=14*UI;
    D2D1_RECT_F rf=D2D1::RectF(r.left,r.top,r.right,r.bottom);
    // 0) 柔和投影：多层递减 alpha 的圆角矩形，让玻璃在纯白背景上也能“浮起来”
    for(int i=5;i>=1;i--){ float e=i*2.4f; br->SetColor(D2D1::ColorF(0.05f,0.07f,0.10f,0.045f));
        g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rf.left-e,rf.top-e+2.5f,rf.right+e,rf.bottom+e+3.5f),rad+e,rad+e),br); }
    D2D1_ROUNDED_RECT rr=D2D1::RoundedRect(rf,rad,rad);
    if(g_blurBrush){
        // 1) 磨砂背板：把背后屏幕的模糊图裁进圆角
        g_rt->FillRoundedRectangle(rr,g_blurBrush);
        // 2) 轻微“压暗+去饱和”的中性色霜面（比纯白更能在白底上显出玻璃块，避免全白融背景）
        br->SetColor(D2D1::ColorF(0.62f,0.66f,0.72f,0.30f)); g_rt->FillRoundedRectangle(rr,br);
        br->SetColor(D2D1::ColorF(1,1,1,0.14f)); g_rt->FillRoundedRectangle(rr,br);
    } else {
        br->SetColor(Col(255,255,255)); g_rt->FillRoundedRectangle(rr,br);
    }
    // 3) 顶部高光渐变（镜面反光）
    if(g_sheen){ g_sheen->SetStartPoint(D2D1::Point2F(rf.left,rf.top));
        g_sheen->SetEndPoint(D2D1::Point2F(rf.left,rf.top+(rf.bottom-rf.top)*0.62f));
        g_rt->FillRoundedRectangle(rr,g_sheen); }
    // 4) 外深内亮双描边：白底也有清晰玻璃轮廓
    br->SetColor(D2D1::ColorF(0.32f,0.36f,0.42f,0.45f)); g_rt->DrawRoundedRectangle(rr,br,1.1f);
    br->SetColor(D2D1::ColorF(1,1,1,0.75f));
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rf.left+1.4f,rf.top+1.4f,rf.right-1.4f,rf.bottom-1.4f),rad-1,rad-1),br,1.0f);
}
// 方案 A：工具高亮融流（随激活工具在按钮间像液体一样拉伸/移动）
static void PaintFlowHighlight(ID2D1SolidColorBrush* br){
    if(g_tool<0){ g_hlOn=false; return; }
    int idx=-1; for(size_t i=0;i<g_btns.size();++i) if(g_btns[i].kind==0 && g_btns[i].id==g_tool){ idx=(int)i; break; }
    if(idx<0){ g_hlOn=false; return; }
    RECT r=g_btns[idx].rc; float inset=3.0f;
    float tx=r.left+inset, hh=(r.bottom-r.top)-inset*2, w=(r.right-r.left)-inset*2, y=r.top+inset;
    if(!g_hlOn){ g_hlLead=g_hlLag=tx; g_hlOn=true; }
    g_hlLead += (tx-g_hlLead)*0.34f;
    g_hlLag  += (tx-g_hlLag )*0.19f;
    float x0=min(g_hlLead,g_hlLag), x1=max(g_hlLead,g_hlLag)+w;
    float rad=hh/2;
    D2D1_ROUNDED_RECT rr=D2D1::RoundedRect(D2D1::RectF(x0,y,x1,y+hh),rad,rad);
    br->SetColor(Col(19,192,96,0.95f)); g_rt->FillRoundedRectangle(rr,br);
    br->SetColor(D2D1::ColorF(1,1,1,0.28f)); // 顶部一点高光让绿块也有玻璃感
    g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0,y,x1,y+hh*0.5f),rad,rad),br);
    if((fabs(tx-g_hlLead)>0.4f||fabs(tx-g_hlLag)>0.4f) && g_hwnd) InvalidateRect(g_hwnd,nullptr,FALSE);
}
static void DrawIconBmp(const char* key, const RECT& rc, float ratio){
    auto it=g_iconBmp.find(key); if(it==g_iconBmp.end()||!it->second) return;
    ID2D1Bitmap* bm=it->second;
    // 用内容边界作为源矩形，统一按最大边缩放到目标尺寸 → 各图标视觉大小一致
    D2D1_RECT_F src; auto bi=g_iconBox.find(key);
    if(bi!=g_iconBox.end()) src=bi->second; else { D2D1_SIZE_F s=bm->GetSize(); src=D2D1::RectF(0,0,s.width,s.height); }
    float sw=src.right-src.left, sh=src.bottom-src.top; if(sw<1)sw=1; if(sh<1)sh=1;
    float target=(rc.right-rc.left)*ratio;
    float k=target/max(sw,sh); float w=sw*k, h=sh*k;
    float cx=(rc.left+rc.right)/2.0f, cy=(rc.top+rc.bottom)/2.0f;
    g_rt->DrawBitmap(bm,D2D1::RectF(cx-w/2,cy-h/2,cx+w/2,cy+h/2),1.0f,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,src);
}
static void PaintIcon(const Btn& b, ID2D1SolidColorBrush* br){
    // 激活态高亮已由 PaintFlowHighlight 统一绘制（融流），这里只画图标
    const char* key=nullptr;
    if(b.kind==0){ static const char* ks[]={"rect","rrect","ellipse","arrow","mosaic","text"}; key=ks[b.id]; }
    else if(b.kind==1) key="undo"; else if(b.kind==4) key="save";
    else if(b.kind==2) key="close"; else key="confirm";
    // 取消(✕)图标视觉偏大，单独缩小一点，与确认(✓)协调
    float ratio=(b.kind==2)?0.56f:0.66f;
    DrawIconBmp(key,b.rc,ratio);
}

// ---------- 放大镜 ----------
static void DrawMagnifier(int cx,int cy, ID2D1SolidColorBrush* br){
    const int Z=(int)(8*UI), half=8, n=2*half+1, D=n*Z;
    float lh=18*UI, phH=8+3*lh;   // 三行信息，行距按字体行高（小一号字体）
    float off=28;                                    // 让放大镜避开光标，不与取色框重叠
    float ox=cx+off, oy=cy+off;
    if(ox+D>g_vw) ox=cx-off-D; if(oy+D+phH>g_vh) oy=cy-off-(D+phH);
    g_rt->DrawBitmap(g_shot, D2D1::RectF(ox,oy,ox+D,oy+D),1.0f,
        D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
        D2D1::RectF((float)(cx-half),(float)(cy-half),(float)(cx+half+1),(float)(cy+half+1)));
    float mid=ox+half*Z+Z/2.0f, midY=oy+half*Z+Z/2.0f;
    // 十字准星：白色描边 + 品红芯，任意背景(浅/深/灰/彩)都清晰
    br->SetColor(D2D1::ColorF(1,1,1,0.95f));
    g_rt->DrawLine(D2D1::Point2F(mid,(float)oy),D2D1::Point2F(mid,(float)(oy+D)),br,3.0f*UI);
    g_rt->DrawLine(D2D1::Point2F((float)ox,midY),D2D1::Point2F((float)(ox+D),midY),br,3.0f*UI);
    br->SetColor(Col(255,45,60));
    g_rt->DrawLine(D2D1::Point2F(mid,(float)oy),D2D1::Point2F(mid,(float)(oy+D)),br,1.3f*UI);
    g_rt->DrawLine(D2D1::Point2F((float)ox,midY),D2D1::Point2F((float)(ox+D),midY),br,1.3f*UI);
    D2D1_RECT_F cxr=D2D1::RectF(ox+half*Z,oy+half*Z,ox+half*Z+Z,oy+half*Z+Z);
    br->SetColor(D2D1::ColorF(1,1,1,0.95f)); g_rt->DrawRectangle(cxr,br,3.0f);
    br->SetColor(Col(255,45,60)); g_rt->DrawRectangle(cxr,br,1.5f);
    br->SetColor(Col(255,255,255)); g_rt->DrawRectangle(D2D1::RectF(ox,oy,ox+D,oy+D),br,2.0f*UI);
    int r,g,b; GetPixel(cx,cy,r,g,b);
    br->SetColor(Col(245,246,248)); g_rt->FillRectangle(D2D1::RectF(ox,oy+D,ox+D,oy+D+phH),br);
    float py=oy+D+4, sw=18*UI, tx=ox+10+sw+10;
    br->SetColor(Col(r,g,b)); g_rt->FillRectangle(D2D1::RectF(ox+10,py+(lh-sw)/2,ox+10+sw,py+(lh-sw)/2+sw),br);
    wchar_t l1[32],l2[48]; swprintf(l1,32,L"#%02X%02X%02X",r,g,b); swprintf(l2,48,L"RGB %d, %d, %d",r,g,b);
    br->SetColor(Col(20,20,20));
    g_rt->DrawText(l1,(UINT32)wcslen(l1),g_tfSmall,D2D1::RectF(tx,py,ox+D,py+lh),br);
    g_rt->DrawText(l2,(UINT32)wcslen(l2),g_tfSmall,D2D1::RectF(tx,py+lh,ox+D,py+2*lh),br);
    br->SetColor(Col(140,140,140));
    const wchar_t* h3=L"右键复制色值"; g_rt->DrawText(h3,(UINT32)wcslen(h3),g_tfSmall,D2D1::RectF(tx,py+2*lh,ox+D,py+3*lh),br);
    // 整体外框：让下方信息框与取色框左右/底边完全对齐
    br->SetColor(Col(255,255,255)); g_rt->DrawRectangle(D2D1::RectF(ox,oy,ox+D,oy+D+phH),br,2.0f*UI);
    br->SetColor(Col(210,213,217)); g_rt->DrawLine(D2D1::Point2F(ox,oy+D),D2D1::Point2F(ox+D,oy+D),br,1.0f);
}

// ---------- 渲染 ----------
static void Render(HWND hwnd){
    g_hwnd=hwnd;
    CreateDeviceResources(hwnd); if(!g_rt) return;
    g_rt->BeginDraw(); g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    D2D1_RECT_F full=D2D1::RectF(0,0,(float)g_vw,(float)g_vh);
    g_rt->DrawBitmap(g_shot,full,1.0f,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,full);
    ID2D1SolidColorBrush* br=nullptr; g_rt->CreateSolidColorBrush(Col(0,0,0),&br);
    br->SetColor(Col(0,0,0,0.5f)); g_rt->FillRectangle(full,br);

    if(g_mode==EDIT || g_selecting){
        RECT s=g_sel; D2D1_RECT_F selr=D2D1::RectF(s.left,s.top,s.right,s.bottom);
        g_rt->DrawBitmap(g_shot,selr,1.0f,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,selr);
        if(g_mode==EDIT){
            g_rt->PushAxisAlignedClip(selr,D2D1_ANTIALIAS_MODE_ALIASED);
            for(auto&a:g_annos) if(a.type==4) DrawShape(g_rt,a,br,0,0,g_mosaicBrush);   // 马赛克在最底层(只盖原图)
            if(g_drawing && g_cur.type==4) DrawShape(g_rt,g_cur,br,0,0,g_mosaicBrush);
            for(auto&a:g_annos) if(a.type!=4) DrawShape(g_rt,a,br,0,0,g_mosaicBrush);
            if(g_drawing && g_cur.type!=4) DrawShape(g_rt,g_cur,br,0,0,g_mosaicBrush);
            g_rt->PopAxisAlignedClip();
            // 箭头端点吸附提示：绿色圆环
            if(((g_drawing && g_cur.type==3)||g_dragMode==4) && g_snapOn){ br->SetColor(Col(19,192,96)); g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(g_snapX,g_snapY),7*UI,7*UI),br,2.0f); }
            // 可拖动要素：灰色长虚线框(压在线条上) + 四周圆点
            { int tgt = (g_moving||g_dragMode==3)? g_selIdx : g_hoverIdx;
              if(tgt>=0 && tgt<(int)g_annos.size() && g_annos[tgt].type!=3){  // 箭头不显示四方外框
                const Anno& A=g_annos[tgt]; float nr=3.5f*UI;
                float ox[8],oy[8]; NodesOf(A,ox,oy);          // 文字随旋转，框与节点一起转
                br->SetColor(Col(154,160,166));
                int corn[4]={0,2,4,6};                        // TL TR BR BL
                for(int i=0;i<4;i++){ D2D1_POINT_2F p0=D2D1::Point2F(ox[corn[i]],oy[corn[i]]), p1=D2D1::Point2F(ox[corn[(i+1)%4]],oy[corn[(i+1)%4]]); g_rt->DrawLine(p0,p1,br,2.0f,g_dash); }
                for(int i=0;i<8;i++){ br->SetColor(Col(255,255,255)); g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ox[i],oy[i]),nr,nr),br); br->SetColor(Col(154,160,166)); g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(ox[i],oy[i]),nr,nr),br,1.0f); }
              }
            }
            // 选中箭头的弯折控制点（曲线中点）
            if(g_selArrow>=0 && g_selArrow<(int)g_annos.size() && g_annos[g_selArrow].type==3){
                const Anno& a=g_annos[g_selArrow]; float cx=a.hasCtrl?a.cx:(a.x0+a.x1)/2, cy=a.hasCtrl?a.cy:(a.y0+a.y1)/2;
                float mx=0.25f*a.x0+0.5f*cx+0.25f*a.x1, my=0.25f*a.y0+0.5f*cy+0.25f*a.y1;
                br->SetColor(Col(19,192,96)); g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(mx,my),2.6f*UI,2.6f*UI),br);
                br->SetColor(Col(255,255,255)); g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(mx,my),2.6f*UI,2.6f*UI),br,1.0f);
                // 首尾节点：尾(起点)空心，头(箭尖)实心，便于识别方向、可拖动
                float nr=5.0f*UI;
                br->SetColor(Col(255,255,255)); g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(a.x0,a.y0),nr,nr),br);
                br->SetColor(Col(19,192,96)); g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(a.x0,a.y0),nr,nr),br,1.6f);
                br->SetColor(Col(19,192,96)); g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(a.x1,a.y1),nr,nr),br);
                br->SetColor(Col(255,255,255)); g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(a.x1,a.y1),nr,nr),br,1.6f);
            }
            // 选中文字：右上角旋转手柄（可任意旋转）
            if(g_selIdx>=0 && g_selIdx<(int)g_annos.size() && g_annos[g_selIdx].type==5 && !g_typing){
                float hx,hy; if(RotHandleCenter(g_selIdx,hx,hy)){ float rr2=6.0f*UI;
                    br->SetColor(Col(255,255,255)); g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(hx,hy),rr2,rr2),br);
                    br->SetColor(Col(19,192,96)); g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(hx,hy),rr2,rr2),br,1.2f);
                    RECT hr2={(LONG)(hx-rr2),(LONG)(hy-rr2),(LONG)(hx+rr2),(LONG)(hy+rr2)}; DrawIconBmp("rotate",hr2,0.82f);
                }
            }
            // 文字输入（直接 D2D 绘制，框随文字自适应，无子窗口/无闪烁）
            if(g_typing){
                IDWriteTextFormat* f=GetTextFmt(g_typeFont,(int)g_typeSize);
                if(f){ IDWriteTextLayout* lay=nullptr;
                    g_dwrite->CreateTextLayout(g_typingText.c_str(),(UINT32)g_typingText.size(),f,4000,4000,&lay);
                    if(lay){ DWRITE_TEXT_METRICS m; lay->GetMetrics(&m);
                        float tx=(float)g_typePos.x, ty=(float)g_typePos.y;
                        float tw=max(m.width, g_typeSize*0.6f), th=max(m.height, g_typeSize*1.2f);
                        br->SetColor(Col(19,192,96)); g_rt->DrawRectangle(D2D1::RectF(tx-3,ty-2,tx+tw+5,ty+th+2),br,1.5f);
                        br->SetColor(PALETTE[g_typeColor]); g_rt->DrawTextLayout(D2D1::Point2F(tx,ty),lay,br);
                        if(((GetTickCount()/500)&1)==0){ float caretX=0,caretY=0; DWRITE_HIT_TEST_METRICS hm; lay->HitTestTextPosition((UINT32)g_typingText.size(),FALSE,&caretX,&caretY,&hm);
                            br->SetColor(PALETTE[g_typeColor]); g_rt->DrawLine(D2D1::Point2F(tx+caretX+1,ty+caretY),D2D1::Point2F(tx+caretX+1,ty+caretY+hm.height),br,max(1.5f,g_typeSize*0.06f)); }
                        lay->Release();
                    }
                }
            }
        }
        br->SetColor(Col(19,192,96)); g_rt->DrawRectangle(selr,br,2.0f);
        // 选区可调手柄（未选工具时显示，可拖动缩放/移动整框）
        if(g_mode==EDIT && g_tool==-1){ RECT s=g_sel; float mx=(s.left+s.right)/2.0f,my=(s.top+s.bottom)/2.0f, hs=4.0f*UI;
            float px[8]={(float)s.left,mx,(float)s.right,(float)s.right,(float)s.right,mx,(float)s.left,(float)s.left};
            float py[8]={(float)s.top,(float)s.top,(float)s.top,my,(float)s.bottom,(float)s.bottom,(float)s.bottom,my};
            for(int i=0;i<8;i++){ D2D1_RECT_F hr=D2D1::RectF(px[i]-hs,py[i]-hs,px[i]+hs,py[i]+hs);
                br->SetColor(Col(255,255,255)); g_rt->FillRectangle(hr,br); br->SetColor(Col(19,192,96)); g_rt->DrawRectangle(hr,br,1.4f); } }
        // 旋转角度实时读数（右侧）
        if(g_dragMode==2 && g_rotIdx>=0 && g_rotIdx<(int)g_annos.size()){
            float deg=g_annos[g_rotIdx].rot*57.2958f; deg=fmodf(deg,360.0f); if(deg>180)deg-=360; if(deg<-180)deg+=360;
            wchar_t ab[24]; swprintf(ab,24,L"%.0f°",deg);
            float bw=120*UI,bh=56*UI, bx=(float)g_vw-bw-26, by=(float)g_vh/2-bh/2;
            br->SetColor(D2D1::ColorF(0.10f,0.11f,0.12f,0.92f)); g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx,by,bx+bw,by+bh),12,12),br);
            br->SetColor(Col(19,192,96)); g_rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx,by,bx+bw,by+bh),12,12),br,1.5f);
            IDWriteTextFormat* af=g_tfHint?g_tfHint:g_tf;
            br->SetColor(Col(255,255,255));
            g_rt->DrawText(ab,(UINT32)wcslen(ab),af,D2D1::RectF(bx,by+bh*0.28f,bx+bw,by+bh),br);
        }
        // 尺寸标签
        wchar_t sz[32]; swprintf(sz,32,L"%d × %d",s.right-s.left,s.bottom-s.top);
        br->SetColor(Col(255,255,255));
        float ly=(float)s.top-(13*UI+12);
        g_rt->DrawText(sz,(UINT32)wcslen(sz),g_tfSmall,D2D1::RectF(s.left+2, ly, s.left+300,(float)s.top-4),br);
        if(g_mode==EDIT){
            // 马赛克笔刷指示（半透明灰色方/圆，跟随鼠标）
            if(g_tool==4){ int mx=g_cursor.x,my=g_cursor.y;
                if(mx>=g_sel.left&&mx<=g_sel.right&&my>=g_sel.top&&my<=g_sel.bottom && !PtIn(g_barRc,mx,my) && !(g_subRc.right>g_subRc.left&&PtIn(g_subRc,mx,my))){
                    float rr=MBRUSH[g_level]/2;
                    br->SetColor(D2D1::ColorF(0.60f,0.63f,0.66f,0.40f));
                    if(g_mshape==1) g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F((float)mx,(float)my),rr,rr),br); else g_rt->FillRectangle(D2D1::RectF(mx-rr,my-rr,mx+rr,my+rr),br);
                    br->SetColor(Col(105,110,116));
                    if(g_mshape==1) g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((float)mx,(float)my),rr,rr),br,1.0f); else g_rt->DrawRectangle(D2D1::RectF(mx-rr,my-rr,mx+rr,my+rr),br,1.0f);
                }
            }
            // 工具栏（液态玻璃）
            PaintPill(g_barRc,br);
            PaintFlowHighlight(br);           // 方案 A：融流高亮（在图标下）
            for(auto&b:g_btns) PaintIcon(b,br);
            if(g_tool>=0 && g_subRc.right>g_subRc.left){
                PaintPill(g_subRc,br);
                for(auto&sw:g_lvlSw){ float cx=(sw.rc.left+sw.rc.right)/2.0f, cy=(sw.rc.top+sw.rc.bottom)/2.0f; float rr=(3+sw.idx*4)*UI/1.6f; br->SetColor(sw.idx==g_level?Col(19,192,96):Col(176,182,189)); g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy),rr,rr),br); }
                if(g_sizeSlider.right>g_sizeSlider.left){ // 字号拖动条
                    float tl=g_sizeSlider.left+9*UI, tr=g_sizeSlider.right-9*UI, cyc=(g_sizeSlider.top+g_sizeSlider.bottom)/2.0f;
                    float t=min(max((g_typeSize-SMIN)/(SMAX-SMIN),0.0f),1.0f), kx=tl+t*(tr-tl);
                    br->SetColor(Col(214,217,222)); g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tl,cyc-2.5f,tr,cyc+2.5f),2.5f,2.5f),br);
                    br->SetColor(Col(19,192,96)); g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tl,cyc-2.5f,kx,cyc+2.5f),2.5f,2.5f),br);
                    float kr=9*UI; br->SetColor(Col(255,255,255)); g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx,cyc),kr,kr),br);
                    br->SetColor(Col(190,194,199)); g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(kx,cyc),kr,kr),br,1.0f);
                }
                for(auto&sw:g_colSw){ D2D1_RECT_F r=D2D1::RectF(sw.rc.left+6,sw.rc.top+6,sw.rc.right-6,sw.rc.bottom-6); br->SetColor(PALETTE[sw.idx]); g_rt->FillRoundedRectangle(D2D1::RoundedRect(r,4,4),br); if(sw.idx==g_colorIdx){ br->SetColor(Col(19,192,96)); g_rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sw.rc.left+3,sw.rc.top+3,sw.rc.right-3,sw.rc.bottom-3),6,6),br,2.0f); } }
                for(auto&sw:g_shpSw){ float cx=(sw.rc.left+sw.rc.right)/2.0f, cy=(sw.rc.top+sw.rc.bottom)/2.0f, s=10*UI; br->SetColor(sw.idx==g_mshape?Col(19,192,96):Col(120,126,132)); if(sw.idx==0) g_rt->DrawRectangle(D2D1::RectF(cx-s,cy-s,cx+s,cy+s),br,2.0f); else g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy),s,s),br,2.0f); }
                if(g_fontBtn.right>g_fontBtn.left){ br->SetColor(Col(70,75,80));
                    std::wstring fn=(g_fontIdx<(int)g_fontNames.size()?g_fontNames[g_fontIdx]:std::wstring(L"(无字体)"))+L"  ▾";
                    IDWriteTextFormat* mf=g_tfMenu?g_tfMenu:g_tfSmall;
                    g_rt->DrawText(fn.c_str(),(UINT32)fn.size(),mf,D2D1::RectF(g_fontBtn.left,g_fontBtn.top,g_fontBtn.right,g_fontBtn.bottom),br); }
            }
            // 字体下拉菜单（字体列表 + 安装 + 卸载）
            if(g_tool==5 && g_fontMenu){ RECT panel; std::vector<RECT> rows; int tot=FontMenuLayout(panel,rows); int nf=(int)g_fontNames.size();
                IDWriteTextFormat* mf=g_tfMenu?g_tfMenu:g_tfSmall;
                D2D1_ROUNDED_RECT pr=D2D1::RoundedRect(D2D1::RectF(panel.left,panel.top,panel.right,panel.bottom),10,10);
                br->SetColor(D2D1::ColorF(0,0,0,0.12f)); g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(panel.left-1,panel.top+2,panel.right+1,panel.bottom+3),11,11),br);
                br->SetColor(Col(255,255,255)); g_rt->FillRoundedRectangle(pr,br);
                br->SetColor(Col(210,214,219)); g_rt->DrawRoundedRectangle(pr,br,1.0f);
                for(int i=0;i<tot;i++){ RECT r=rows[i]; bool hov=PtIn(r,g_cursor.x,g_cursor.y);
                    if(hov){ br->SetColor(Col(235,244,238)); g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left,r.top,r.right,r.bottom),6,6),br); }
                    D2D1_COLOR_F tc=Col(60,64,69); std::wstring s;
                    if(i<nf){ s=g_fontNames[i]; if(i==g_fontIdx){ tc=Col(19,150,80); s=L"● "+s; } else s=L"   "+s; }
                    else if(i==nf) { s=L"＋  安装字体…"; tc=Col(19,150,80); }
                    else           { s=L"－  卸载当前字体"; tc=Col(190,70,70); }
                    br->SetColor(tc); g_rt->DrawText(s.c_str(),(UINT32)s.size(),mf,D2D1::RectF(r.left+8*UI,r.top,r.right-6*UI,r.bottom),br);
                }
                if(nf>=0 && nf<tot){ br->SetColor(Col(228,231,235)); float sy=(float)rows[nf].top-1; g_rt->DrawLine(D2D1::Point2F(panel.left+8,sy),D2D1::Point2F(panel.right-8,sy),br,1.0f); }
            }
            // 悬停名称气泡
            if(g_hoverBtn>=0 && g_hoverBtn<(int)g_btns.size()){
                const Btn& hb=g_btns[g_hoverBtn];
                const wchar_t* nm; if(hb.kind==0){ static const wchar_t* nn[]={L"矩形",L"圆角矩形",L"椭圆",L"箭头",L"马赛克",L"文字"}; nm=nn[hb.id]; }
                else if(hb.kind==1) nm=L"撤销"; else if(hb.kind==4) nm=L"保存"; else if(hb.kind==2) nm=L"取消"; else nm=L"完成(复制)";
                IDWriteTextLayout* lay=nullptr; g_dwrite->CreateTextLayout(nm,(UINT32)wcslen(nm),g_tfSmall,1000,100,&lay);
                if(lay){ DWRITE_TEXT_METRICS m; lay->GetMetrics(&m); float pad2=8*UI;
                    float bw=m.width+pad2*2, bh=m.height+6*UI;
                    float bx=(hb.rc.left+hb.rc.right)/2.0f-bw/2; bx=min(max(bx,6.0f),(float)g_vw-bw-6);
                    float by=hb.rc.top-bh-6*UI; if(by<6) by=hb.rc.bottom+6*UI;
                    br->SetColor(D2D1::ColorF(0.16f,0.16f,0.17f,0.95f)); g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx,by,bx+bw,by+bh),6,6),br);
                    br->SetColor(Col(255,255,255)); g_rt->DrawTextLayout(D2D1::Point2F(bx+pad2,by+3*UI),lay,br);
                    lay->Release();
                }
            }
        }
    } else {
        if(g_haveSnap){ br->SetColor(Col(19,192,96)); g_rt->DrawRectangle(D2D1::RectF(g_snap.left,g_snap.top,g_snap.right,g_snap.bottom),br,2.0f*UI); }
        DrawMagnifier(g_cursor.x,g_cursor.y,br);
        br->SetColor(Col(238,238,238));
        const wchar_t* hint=L"移动自动吸附窗口/模块，单击选取；拖动可自定义框选  ·  双击截屏  ·  右键复制色值  ·  Esc 取消";
        g_rt->DrawText(hint,(UINT32)wcslen(hint),g_tfHint,D2D1::RectF(0,18,(float)g_vw,60),br);
    }
    SafeRelease(&br);
    if(g_rt->EndDraw()==D2DERR_RECREATE_TARGET) DiscardDeviceResources();
}

// ---------- 导出（选区+标注 → WIC 位图）----------
static IWICBitmap* RenderToWic(){
    int W=g_sel.right-g_sel.left, H=g_sel.bottom-g_sel.top; if(W<=0||H<=0) return nullptr;
    IWICBitmap* wb=nullptr;
    // 注意：D2D 的 WicBitmapRenderTarget 只支持 32bppPBGRA(预乘) 的 WIC 位图，
    // 用 32bppBGRA 会返回 WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT(0x88982F80) 导致导出/保存失败。
    g_wic->CreateBitmap(W,H,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,&wb);
    if(!wb) return nullptr;
    ID2D1RenderTarget* rt=nullptr;
    g_factory->CreateWicBitmapRenderTarget(wb,
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)), &rt);
    if(!rt){ wb->Release(); return nullptr; }
    // 选区裁剪像素
    std::vector<BYTE> crop((size_t)W*4*H);
    for(int y=0;y<H;y++) memcpy(&crop[(size_t)y*W*4], &g_pixels[(size_t)(g_sel.top+y)*g_stride+(size_t)g_sel.left*4], (size_t)W*4);
    rt->BeginDraw();
    ID2D1Bitmap* cb=nullptr;
    rt->CreateBitmap(D2D1::SizeU(W,H),crop.data(),W*4,
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE)),&cb);
    rt->DrawBitmap(cb,D2D1::RectF(0,0,(float)W,(float)H));
    // 该 RT 专用的马赛克位图画刷
    ID2D1Bitmap* mbmp=nullptr; ID2D1BitmapBrush* mbr=nullptr;
    rt->CreateBitmap(D2D1::SizeU(g_vw,g_vh),g_mosaicPixels.data(),g_stride,
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE)),&mbmp);
    if(mbmp) rt->CreateBitmapBrush(mbmp,D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP,D2D1_EXTEND_MODE_CLAMP,D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR),D2D1::BrushProperties(),&mbr);
    ID2D1SolidColorBrush* br=nullptr; rt->CreateSolidColorBrush(Col(0,0,0),&br);
    for(auto&a:g_annos) if(a.type==4) DrawShape(rt,a,br,(float)-g_sel.left,(float)-g_sel.top,mbr);
    for(auto&a:g_annos) if(a.type!=4) DrawShape(rt,a,br,(float)-g_sel.left,(float)-g_sel.top,mbr);
    SafeRelease(&br); SafeRelease(&mbr); SafeRelease(&mbmp); SafeRelease(&cb);
    rt->EndDraw(); rt->Release();
    return wb;
}
static void CopyToClipboard(HWND hwnd){
    IWICBitmap* wb=RenderToWic(); if(!wb) return;
    UINT W,H; wb->GetSize(&W,&H);
    WICRect lockRc={0,0,(INT)W,(INT)H}; IWICBitmapLock* lk=nullptr;
    wb->Lock(&lockRc,WICBitmapLockRead,&lk); UINT cb2=0; BYTE* src=nullptr; UINT stride=0;
    lk->GetStride(&stride); lk->GetDataPointer(&cb2,&src);
    // 组装 CF_DIB（自底向上，32bpp BGRA）
    size_t sz=sizeof(BITMAPINFOHEADER)+(size_t)W*4*H;
    HGLOBAL hg=GlobalAlloc(GMEM_MOVEABLE,sz); BYTE* p=(BYTE*)GlobalLock(hg);
    BITMAPINFOHEADER* bh=(BITMAPINFOHEADER*)p; ZeroMemory(bh,sizeof(*bh));
    bh->biSize=sizeof(*bh); bh->biWidth=W; bh->biHeight=H; bh->biPlanes=1; bh->biBitCount=32; bh->biCompression=BI_RGB;
    BYTE* dst=p+sizeof(*bh);
    for(UINT y=0;y<H;y++) memcpy(dst+(size_t)(H-1-y)*W*4, src+(size_t)y*stride, (size_t)W*4);
    GlobalUnlock(hg); lk->Release();
    if(OpenClipboard(hwnd)){ EmptyClipboard(); SetClipboardData(CF_DIB,hg); CloseClipboard(); }
    wb->Release();
}
static void SavePng(HWND hwnd){
    wchar_t path[MAX_PATH]=L"";
    SYSTEMTIME st; GetLocalTime(&st);
    swprintf(path,MAX_PATH,L"TatoMark_%04d%02d%02d_%02d%02d%02d.png",st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond);
    OPENFILENAMEW ofn; ZeroMemory(&ofn,sizeof(ofn)); ofn.lStructSize=sizeof(ofn);
    ofn.hwndOwner=hwnd; ofn.lpstrFilter=L"PNG 图片\0*.png\0所有文件\0*.*\0"; ofn.lpstrFile=path; ofn.nMaxFile=MAX_PATH;
    ofn.lpstrDefExt=L"png"; ofn.Flags=OFN_OVERWRITEPROMPT|OFN_EXPLORER;
    // 临时取消置顶，让"另存为"对话框显示在覆盖窗之上；保存后恢复覆盖窗(不退出)
    SetWindowPos(hwnd,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
    bool ok=GetSaveFileNameW(&ofn);
    if(ok){
        IWICBitmap* wb=RenderToWic(); HRESULT hr=E_FAIL;
        if(wb){
            IWICStream* st2=nullptr; hr=g_wic->CreateStream(&st2);
            if(SUCCEEDED(hr)) hr=st2->InitializeFromFilename(path,GENERIC_WRITE);
            IWICBitmapEncoder* enc=nullptr;
            if(SUCCEEDED(hr)) hr=g_wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,&enc);
            if(SUCCEEDED(hr)) hr=enc->Initialize(st2,WICBitmapEncoderNoCache);
            IWICBitmapFrameEncode* fr=nullptr; IPropertyBag2* pb=nullptr;
            if(SUCCEEDED(hr)) hr=enc->CreateNewFrame(&fr,&pb);
            if(SUCCEEDED(hr)) hr=fr->Initialize(pb);
            if(SUCCEEDED(hr)){ UINT W,H; wb->GetSize(&W,&H); fr->SetSize(W,H);
                WICPixelFormatGUID pf=GUID_WICPixelFormat32bppBGRA; fr->SetPixelFormat(&pf);
                hr=fr->WriteSource(wb,nullptr); if(SUCCEEDED(hr)) hr=fr->Commit(); if(SUCCEEDED(hr)) hr=enc->Commit(); }
            SafeRelease(&fr); SafeRelease(&enc); SafeRelease(&st2); wb->Release();
        }
        if(FAILED(hr)){ MessageBoxW(hwnd,L"保存失败",L"TatoMark",MB_OK|MB_ICONERROR); }
        else { // 保存成功：关闭截图窗口
            g_pasteAfter=false; PostQuitMessage(0); return;
        }
    }
    // 取消保存或保存失败：恢复覆盖窗，继续截图
    SetWindowPos(hwnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
    SetForegroundWindow(hwnd); InvalidateRect(hwnd,nullptr,FALSE);
}

// ---------- 输入 ----------
static void EnterEdit(){ g_mode=EDIT; LayoutToolbar(); }

static bool HandleBarClick(HWND hwnd,int x,int y){
    for(auto&b:g_btns){ if(PtIn(b.rc,x,y)){
        if(b.kind==0){ g_tool=(g_tool==b.id?-1:b.id); g_fontMenu=false; LayoutToolbar(); }
        else if(b.kind==1){ if(!g_annos.empty()){ g_annos.pop_back(); if(g_selArrow>=(int)g_annos.size()) g_selArrow=-1; } }
        else if(b.kind==4){ CommitText(); SavePng(hwnd); }
        else if(b.kind==2){ PostQuitMessage(0); }
        else if(b.kind==3){ CommitText(); CopyToClipboard(hwnd); g_pasteAfter=true; PostQuitMessage(0); }
        return true; } }
    for(auto&sw:g_lvlSw){ if(PtIn(sw.rc,x,y)){ g_level=sw.idx; return true; } }
    for(auto&sw:g_colSw){ if(PtIn(sw.rc,x,y)){ g_colorIdx=sw.idx; if(g_selIdx>=0&&g_annos[g_selIdx].type!=4) g_annos[g_selIdx].color=sw.idx; return true; } }
    for(auto&sw:g_shpSw){ if(PtIn(sw.rc,x,y)){ g_mshape=sw.idx; return true; } }
    if(g_fontBtn.right>g_fontBtn.left && PtIn(g_fontBtn,x,y)){ g_fontMenu=!g_fontMenu; return true; }
    return false;
}
static bool NearArrowDot(int x,int y){
    if(g_selArrow<0||g_selArrow>=(int)g_annos.size()||g_annos[g_selArrow].type!=3) return false;
    const Anno&a=g_annos[g_selArrow]; float cx=a.hasCtrl?a.cx:(a.x0+a.x1)/2, cy=a.hasCtrl?a.cy:(a.y0+a.y1)/2;
    float mx=0.25f*a.x0+0.5f*cx+0.25f*a.x1, my=0.25f*a.y0+0.5f*cy+0.25f*a.y1;
    return (x-mx)*(x-mx)+(y-my)*(y-my) <= (7*UI)*(7*UI);
}
static void ClampSel(float&x,float&y){ x=min(max(x,(float)g_sel.left),(float)g_sel.right); y=min(max(y,(float)g_sel.top),(float)g_sel.bottom); }
// 要素的“框节点”坐标：矩形/圆/文字=8个包围盒节点(文字随旋转)，箭头=两端点。返回节点数
static int NodesOf(const Anno&a, float ox[8], float oy[8]){
    if(a.type==3){ ox[0]=a.x0;oy[0]=a.y0; ox[1]=a.x1;oy[1]=a.y1; return 2; }
    float l,t,r,b; AnnoBBox(a,l,t,r,b); float mx=(l+r)/2,my=(t+b)/2;
    float px[8]={l,mx,r,r,r,mx,l,l}, py[8]={t,t,t,my,b,b,b,my};   // 0TL 1TM 2TR 3RM 4BR 5BM 6BL 7LM
    if(a.type==5 && fabsf(a.rot)>0.0001f){ float cx=(l+r)/2,cy=(t+b)/2,ca=cosf(a.rot),sa=sinf(a.rot);
        for(int i=0;i<8;i++){ float dx=px[i]-cx,dy=py[i]-cy; ox[i]=cx+dx*ca-dy*sa; oy[i]=cy+dx*sa+dy*ca; } }
    else for(int i=0;i<8;i++){ ox[i]=px[i]; oy[i]=py[i]; }
    return 8;
}
static void EnsureIds(){ for(auto&a:g_annos) if(a.id==0) a.id=g_nextId++; }
static bool NodePos(int id,int node,float&x,float&y){ if(id==0||node<0) return false;
    for(auto&a:g_annos) if(a.id==id){ float ox[8],oy[8]; int n=NodesOf(a,ox,oy); if(node>=n) return false; x=ox[node];y=oy[node]; return true; } return false; }
// 让所有已连接的箭头端点跟随其连接要素当前节点位置（移动/缩放/旋转后自动变形保持连接）
static void RefreshConnections(){ EnsureIds();
    for(auto&a:g_annos){ if(a.type!=3) continue; float x,y;
        if(a.cid0&&NodePos(a.cid0,a.cnode0,x,y)){ a.x0=x;a.y0=y; }
        if(a.cid1&&NodePos(a.cid1,a.cnode1,x,y)){ a.x1=x;a.y1=y; } } }
// 箭头端点吸附：找最近的要素框节点。命中则把 x,y 吸到该节点，并输出该节点所属要素 id 与节点序号
static bool SnapNode(float& x,float& y,int exclude,int&outId,int&outNode){ EnsureIds();
    float thr=13*UI, best=thr*thr+1, bx=x,by=y; outId=0; outNode=-1;
    for(int i=0;i<(int)g_annos.size();++i){ if(i==exclude) continue; const Anno&a=g_annos[i]; if(a.type==4) continue;
        float ox[8],oy[8]; int n=NodesOf(a,ox,oy);
        for(int k=0;k<n;k++){ float dx=x-ox[k],dy=y-oy[k],d=dx*dx+dy*dy; if(d<best){best=d;bx=ox[k];by=oy[k];outId=a.id;outNode=k;} }
    }
    if(best<=thr*thr){ x=bx; y=by; return true; } outId=0; outNode=-1; return false;
}
static bool SnapNode(float& x,float& y,int exclude){ int a,b; return SnapNode(x,y,exclude,a,b); }
// 拖动条 → 字号（所见即所得：同步在编字/选中文字要素上）
static void SetSizeFromSlider(int x){
    float tl=g_sizeSlider.left+9*UI, tr=g_sizeSlider.right-9*UI;
    float t=min(max(((float)x-tl)/(tr-tl),0.0f),1.0f);
    g_typeSize=SMIN+t*(SMAX-SMIN);
    if(g_selIdx>=0 && g_selIdx<(int)g_annos.size() && g_annos[g_selIdx].type==5) g_annos[g_selIdx].fontSize=g_typeSize;
}
// 命中旋转手柄：仅文字(g_selIdx)。命中则把目标记入 g_rotIdx
static bool NearRotate(int x,int y){
    if(g_selIdx<0||g_selIdx>=(int)g_annos.size()||g_annos[g_selIdx].type!=5) return false;
    float hx,hy; if(!RotHandleCenter(g_selIdx,hx,hy)) return false;
    if((x-hx)*(x-hx)+(y-hy)*(y-hy) <= (9*UI)*(9*UI)){ g_rotIdx=g_selIdx; return true; }
    return false;
}
// 命中选中箭头的首/尾节点：0 尾(起点)、1 头(箭尖)、-1 无
static int ArrowEndAt(int x,int y){
    if(g_selArrow<0||g_selArrow>=(int)g_annos.size()||g_annos[g_selArrow].type!=3) return -1;
    const Anno&a=g_annos[g_selArrow]; float r=8*UI;
    if((x-a.x0)*(x-a.x0)+(y-a.y0)*(y-a.y0)<=r*r) return 0;
    if((x-a.x1)*(x-a.x1)+(y-a.y1)*(y-a.y1)<=r*r) return 1;
    return -1;
}

// 选中/移动/删除
static void AnnoBBox(const Anno&a,float&l,float&t,float&r,float&b){
    if(a.type==4){ if(a.stamps.empty()){l=t=r=b=0;return;} float rr=a.brush/2; l=t=1e9f;r=b=-1e9f; for(auto&s:a.stamps){l=min(l,s.x-rr);t=min(t,s.y-rr);r=max(r,s.x+rr);b=max(b,s.y+rr);} return; }
    if(a.type==5){ float w,h; TextSize(a,w,h); l=a.x0;t=a.y0;r=a.x0+w;b=a.y0+h; return; }
    l=min(a.x0,a.x1);t=min(a.y0,a.y1);r=max(a.x0,a.x1);b=max(a.y0,a.y1);
}
static float DistSeg(float px,float py,float ax,float ay,float bx,float by){
    float dx=bx-ax,dy=by-ay; float L2=dx*dx+dy*dy;
    float t = L2>0 ? ((px-ax)*dx+(py-ay)*dy)/L2 : 0.0f; t=max(0.0f,min(1.0f,t));
    float qx=ax+t*dx, qy=ay+t*dy; return sqrtf((px-qx)*(px-qx)+(py-qy)*(py-qy));
}
// 只有落在“图形边线”上才算命中（框内不命中，方便在框里继续放要素）
static bool OnAnno(const Anno&a,float x,float y){
    float tol=6.0f*UI;
    if(a.type==5){ float l,t,r,b; AnnoBBox(a,l,t,r,b); return x>=l-tol&&x<=r+tol&&y>=t-tol&&y<=b+tol; } // 文本：整块
    if(a.type==3){ // 箭头：贴近箭身（含弯折贝塞尔）
        float w=max(tol,a.width*0.5f+4.0f*UI);
        if(a.hasCtrl){ float best=1e9f,pxA=a.x0,pyA=a.y0; const int N=24;
            for(int i=1;i<=N;i++){ float t=(float)i/N,mt=1-t;
                float bx=mt*mt*a.x0+2*mt*t*a.cx+t*t*a.x1, by=mt*mt*a.y0+2*mt*t*a.cy+t*t*a.y1;
                best=min(best,DistSeg(x,y,pxA,pyA,bx,by)); pxA=bx;pyA=by; }
            return best<=w; }
        return DistSeg(x,y,a.x0,a.y0,a.x1,a.y1)<=w;
    }
    float l=min(a.x0,a.x1),t=min(a.y0,a.y1),r=max(a.x0,a.x1),b=max(a.y0,a.y1);
    float hw=max(tol,a.width*0.5f+2.0f*UI);
    if(a.type==2){ // 椭圆：径向距离贴近椭圆线
        float cx=(l+r)/2,cy=(t+b)/2,rx=(r-l)/2,ry=(b-t)/2; if(rx<1)rx=1; if(ry<1)ry=1;
        float f=sqrtf(((x-cx)/rx)*((x-cx)/rx)+((y-cy)/ry)*((y-cy)/ry));
        return fabsf(f-1.0f)*min(rx,ry)<=hw;
    }
    // 矩形 / 圆角矩形：只在四条边附近
    bool inX=(x>=l-hw&&x<=r+hw), inY=(y>=t-hw&&y<=b+hw);
    bool nearV=(fabsf(x-l)<=hw||fabsf(x-r)<=hw)&&inY;
    bool nearH=(fabsf(y-t)<=hw||fabsf(y-b)<=hw)&&inX;
    return nearV||nearH;
}
static int AnnoAt(int x,int y){ for(int i=(int)g_annos.size()-1;i>=0;i--){ if(g_annos[i].type==4) continue; if(OnAnno(g_annos[i],(float)x,(float)y)) return i; } return -1; }
static void MoveAnno(int i,float dx,float dy){ Anno&a=g_annos[i]; if(a.type==4){ for(auto&s:a.stamps){s.x+=dx;s.y+=dy;} } else { a.x0+=dx;a.y0+=dy;a.x1+=dx;a.y1+=dy; if(a.hasCtrl){a.cx+=dx;a.cy+=dy;} } }
// 角点：0 TL 1 TR 2 BR 3 BL
static int CornerAt(int idx,int x,int y){ const Anno&a=g_annos[idx]; if(a.type==4) return -1; float l,t,r,b; AnnoBBox(a,l,t,r,b); float fx=(float)x,fy=(float)y;
    if(a.type==5 && fabsf(a.rot)>0.0001f){ float cx=(l+r)/2,cy=(t+b)/2,ca=cosf(-a.rot),sa=sinf(-a.rot),dx=x-cx,dy=y-cy; fx=cx+dx*ca-dy*sa; fy=cy+dx*sa+dy*ca; } // 旋转文字：把光标转回局部坐标
    float cs=(9*UI)*(9*UI); float cx4[4]={l,r,r,l},cy4[4]={t,t,b,b}; for(int i=0;i<4;i++){ float dx=fx-cx4[i],dy=fy-cy4[i]; if(dx*dx+dy*dy<=cs) return i; } return -1; }
// 命中要素“边线”，或其四个角点（角点可能在边线之外，如椭圆）——用于悬停/抓取缩放
static int AnnoOrCornerAt(int x,int y){ int a=AnnoAt(x,y); if(a>=0) return a; for(int i=(int)g_annos.size()-1;i>=0;i--){ if(g_annos[i].type==4) continue; if(CornerAt(i,x,y)>=0) return i; } return -1; }
static void ResizeTo(float mx,float my){
    Anno& a=g_annos[g_selIdx];
    if(a.type==5){ // 文字：等比例缩放，锚点角固定
        float od=hypotf(g_rzGx-g_rzAx,g_rzGy-g_rzAy); if(od<1)od=1; float nd=hypotf(mx-g_rzAx,my-g_rzAy);
        a.fontSize=max(8.0f,g_rzOrig.fontSize*(nd/od)); float w,h; TextSize(a,w,h);
        int anc=(g_rzCorner+2)%4;
        if(anc==0){ a.x0=g_rzAx; a.y0=g_rzAy; } else if(anc==1){ a.x0=g_rzAx-w; a.y0=g_rzAy; }
        else if(anc==2){ a.x0=g_rzAx-w; a.y0=g_rzAy-h; } else { a.x0=g_rzAx; a.y0=g_rzAy-h; }
        return;
    }
    // 按住 Shift：矩形/圆角矩形/椭圆约束为正方/正圆（锚点角固定，取较大边），所见即所得
    if((GetKeyState(VK_SHIFT)&0x8000) && a.type<=2){
        float dx=mx-g_rzAx, dy=my-g_rzAy; float s=max(fabsf(dx),fabsf(dy));
        mx=g_rzAx+(dx<0?-s:s); my=g_rzAy+(dy<0?-s:s);
    }
    float nl=min(g_rzAx,mx),nt=min(g_rzAy,my),nr=max(g_rzAx,mx),nb=max(g_rzAy,my);
    if(nr-nl<4)nr=nl+4; if(nb-nt<4)nb=nt+4;
    float ol,ot,orr,ob; AnnoBBox(g_rzOrig,ol,ot,orr,ob); float ow=orr-ol,oh=ob-ot; if(ow<1)ow=1; if(oh<1)oh=1;
    auto MX=[&](float px){ return nl+(px-ol)/ow*(nr-nl); };
    auto MY=[&](float py){ return nt+(py-ot)/oh*(nb-nt); };
    if(a.type<=2){ a.x0=nl;a.y0=nt;a.x1=nr;a.y1=nb; }
    else if(a.type==3){ a.x0=MX(g_rzOrig.x0);a.y0=MY(g_rzOrig.y0);a.x1=MX(g_rzOrig.x1);a.y1=MY(g_rzOrig.y1); if(g_rzOrig.hasCtrl){a.hasCtrl=true;a.cx=MX(g_rzOrig.cx);a.cy=MY(g_rzOrig.cy);} }
}

// 文字输入：直接用 D2D 绘制，无子窗口
static void PlaceTextEntry(HWND parent,int x,int y){
    CommitText();
    g_typing=true; g_typingText.clear(); g_typePos={x,y};
    g_typeFont=g_fontIdx; g_typeColor=g_colorIdx; // 字号由拖动条控制(g_typeSize 持续保留)
}
static void CommitText(){
    if(!g_typing) return; std::wstring s=g_typingText; g_typing=false; g_typingText.clear();
    bool empty=true; for(wchar_t c:s) if(c!=L' '&&c!=L'\r'&&c!=L'\n'&&c!=L'\t'){empty=false;break;}
    if(!empty){ Anno a; a.type=5; a.x0=(float)g_typePos.x; a.y0=(float)g_typePos.y; a.color=g_typeColor; a.fontIdx=g_typeFont; a.fontSize=g_typeSize; a.text=s; g_annos.push_back(a); g_selIdx=(int)g_annos.size()-1; }
}
static void SendPasteLater(){ Sleep(180); INPUT in[4]={}; for(int i=0;i<4;i++) in[i].type=INPUT_KEYBOARD; in[0].ki.wVk=VK_CONTROL; in[1].ki.wVk='V'; in[2].ki.wVk='V'; in[2].ki.dwFlags=KEYEVENTF_KEYUP; in[3].ki.wVk=VK_CONTROL; in[3].ki.dwFlags=KEYEVENTF_KEYUP; SendInput(4,in,sizeof(INPUT)); }
static HCURSOR MakeCursorFromPng(const std::wstring& path,int size){
    IWICBitmapDecoder* dec=nullptr; if(FAILED(g_wic->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&dec))||!dec) return nullptr;
    IWICBitmapFrameDecode* fr=nullptr; dec->GetFrame(0,&fr);
    IWICFormatConverter* conv=nullptr; g_wic->CreateFormatConverter(&conv); conv->Initialize(fr,GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
    int S=4, hi=size*S;                                     // 4x 超采样，描边后再下采样 → 更细腻的抗锯齿
    IWICBitmapScaler* sc=nullptr; g_wic->CreateBitmapScaler(&sc); sc->Initialize(conv,hi,hi,WICBitmapInterpolationModeFant);
    std::vector<BYTE> px((size_t)hi*hi*4,0); sc->CopyPixels(nullptr,hi*4,(UINT)px.size(),px.data());
    std::vector<BYTE> big=px; int rb=S, rw=S*2;
    for(int y=0;y<hi;y++)for(int x=0;x<hi;x++){ if(px[((size_t)y*hi+x)*4+3]>40) continue;
        int md=99; for(int dy=-rw;dy<=rw;dy++)for(int dx=-rw;dx<=rw;dx++){ int nx=x+dx,ny=y+dy; if(nx<0||ny<0||nx>=hi||ny>=hi)continue; if(px[((size_t)ny*hi+nx)*4+3]>120){ int d=max(abs(dx),abs(dy)); if(d<md)md=d; } }
        BYTE* o=&big[((size_t)y*hi+x)*4]; if(md<=rb){ o[0]=o[1]=o[2]=20; o[3]=255; } else if(md<=rw){ o[0]=o[1]=o[2]=255; o[3]=255; } }
    std::vector<BYTE> out((size_t)size*size*4,0); int SS=S*S;
    for(int oy=0;oy<size;oy++)for(int ox=0;ox<size;ox++){ long pa=0,pr=0,pg=0,pb=0;
        for(int j=0;j<S;j++)for(int i=0;i<S;i++){ BYTE* s=&big[(((size_t)(oy*S+j))*hi+(ox*S+i))*4]; int a=s[3]; pa+=a; pb+=(long)s[0]*a; pg+=(long)s[1]*a; pr+=(long)s[2]*a; }
        BYTE* o=&out[((size_t)oy*size+ox)*4]; o[3]=(BYTE)(pa/SS); if(pa>0){ o[0]=(BYTE)(pb/pa); o[1]=(BYTE)(pg/pa); o[2]=(BYTE)(pr/pa); } }
    BITMAPV5HEADER bi; ZeroMemory(&bi,sizeof(bi)); bi.bV5Size=sizeof(bi); bi.bV5Width=size; bi.bV5Height=-size; bi.bV5Planes=1; bi.bV5BitCount=32; bi.bV5Compression=BI_BITFIELDS; bi.bV5RedMask=0x00FF0000; bi.bV5GreenMask=0x0000FF00; bi.bV5BlueMask=0x000000FF; bi.bV5AlphaMask=0xFF000000;
    HDC hdc=GetDC(nullptr); void* bits=nullptr; HBITMAP color=CreateDIBSection(hdc,(BITMAPINFO*)&bi,DIB_RGB_COLORS,&bits,nullptr,0); ReleaseDC(nullptr,hdc);
    if(color&&bits) memcpy(bits,out.data(),out.size());
    HBITMAP mask=CreateBitmap(size,size,1,1,nullptr);
    int hx=size/2,hy=size/2,best=0x7fffffff;
    for(int y=0;y<hi;y++)for(int x=0;x<hi;x++) if(px[((size_t)y*hi+x)*4+3]>120){ int scr=y*16384+x; if(scr<best){best=scr;hx=x/S;hy=y/S;} }
    ICONINFO ii; ZeroMemory(&ii,sizeof(ii)); ii.fIcon=FALSE; ii.xHotspot=hx; ii.yHotspot=hy; ii.hbmMask=mask; ii.hbmColor=color;
    HCURSOR cur=CreateIconIndirect(&ii);
    if(color)DeleteObject(color); if(mask)DeleteObject(mask);
    SafeRelease(&sc); SafeRelease(&conv); SafeRelease(&fr); SafeRelease(&dec);
    return cur;
}

LRESULT CALLBACK WndProc(HWND hwnd,UINT msg,WPARAM wParam,LPARAM lParam){
    switch(msg){
    case WM_MOUSEMOVE:{
        g_cursor.x=GET_X_LPARAM(lParam); g_cursor.y=GET_Y_LPARAM(lParam);
        if(g_sliding){ SetSizeFromSlider(g_cursor.x); InvalidateRect(hwnd,nullptr,FALSE); return 0; }
        if(g_selRz>=0){ int hx=max(0,min((int)g_cursor.x,g_vw)),hy=max(0,min((int)g_cursor.y,g_vh)); RECT&s=g_sel; int i=g_selRz;
            if(i==0){ s.left=min(hx,(int)s.right-20); s.top=min(hy,(int)s.bottom-20);} else if(i==2){ s.right=max(hx,(int)s.left+20); s.top=min(hy,(int)s.bottom-20);}
            else if(i==4){ s.right=max(hx,(int)s.left+20); s.bottom=max(hy,(int)s.top+20);} else if(i==6){ s.left=min(hx,(int)s.right-20); s.bottom=max(hy,(int)s.top+20);}
            else if(i==1){ s.top=min(hy,(int)s.bottom-20);} else if(i==3){ s.right=max(hx,(int)s.left+20);} else if(i==5){ s.bottom=max(hy,(int)s.top+20);} else if(i==7){ s.left=min(hx,(int)s.right-20);}
            LayoutToolbar(); InvalidateRect(hwnd,nullptr,FALSE); return 0; }
        if(g_selMv){ int dx=g_cursor.x-g_moveLast.x, dy=g_cursor.y-g_moveLast.y; RECT&s=g_sel;
            dx=max(-(int)s.left,min(dx,g_vw-(int)s.right)); dy=max(-(int)s.top,min(dy,g_vh-(int)s.bottom));
            s.left+=dx;s.right+=dx;s.top+=dy;s.bottom+=dy; g_moveLast=g_cursor; LayoutToolbar(); InvalidateRect(hwnd,nullptr,FALSE); return 0; }
        if(g_selecting){ g_sel.left=min((LONG)g_start.x,(LONG)g_cursor.x); g_sel.top=min((LONG)g_start.y,(LONG)g_cursor.y); g_sel.right=max((LONG)g_start.x,(LONG)g_cursor.x); g_sel.bottom=max((LONG)g_start.y,(LONG)g_cursor.y); }
        else if(g_dragMode==1 && g_selArrow>=0){ Anno&a=g_annos[g_selArrow]; float mx=(float)g_cursor.x,my=(float)g_cursor.y; ClampSel(mx,my); a.hasCtrl=true; a.cx=2*mx-0.5f*(a.x0+a.x1); a.cy=2*my-0.5f*(a.y0+a.y1); }
        else if(g_dragMode==2 && g_rotIdx>=0 && g_rotIdx<(int)g_annos.size() && g_annos[g_rotIdx].type==5){ // 文字旋转
            float ang=atan2f((float)g_cursor.y-g_rotCy,(float)g_cursor.x-g_rotCx)-g_rotStart; g_annos[g_rotIdx].rot=g_rotBaseAngle+ang; }
        else if(g_dragMode==2 && g_selArrow>=0){ float ang=atan2f((float)g_cursor.y-g_rotCy,(float)g_cursor.x-g_rotCx)-g_rotStart; float ca=cosf(ang),sa=sinf(ang);
            auto rot=[&](D2D1_POINT_2F p){ float dx=p.x-g_rotCx,dy=p.y-g_rotCy; return D2D1::Point2F(g_rotCx+dx*ca-dy*sa,g_rotCy+dx*sa+dy*ca); };
            D2D1_POINT_2F r0=rot(g_rotBase[0]),r1=rot(g_rotBase[1]),rc=rot(g_rotBase[2]); ClampSel(r0.x,r0.y); ClampSel(r1.x,r1.y); ClampSel(rc.x,rc.y);
            Anno&a=g_annos[g_selArrow]; a.x0=r0.x;a.y0=r0.y;a.x1=r1.x;a.y1=r1.y; a.hasCtrl=true; a.cx=rc.x;a.cy=rc.y; }
        else if(g_dragMode==3 && g_selIdx>=0){ float mx=(float)g_cursor.x,my=(float)g_cursor.y; ClampSel(mx,my); ResizeTo(mx,my); }
        else if(g_moving && g_selIdx>=0){ float dx=(float)(g_cursor.x-g_moveLast.x), dy=(float)(g_cursor.y-g_moveLast.y); float l,t,r,b; AnnoBBox(g_annos[g_selIdx],l,t,r,b); dx=max((float)g_sel.left-l,min(dx,(float)g_sel.right-r)); dy=max((float)g_sel.top-t,min(dy,(float)g_sel.bottom-b)); MoveAnno(g_selIdx,dx,dy); g_moveLast=g_cursor; }
        else if(g_dragMode==4 && g_selArrow>=0){ Anno&a=g_annos[g_selArrow]; float mx=(float)g_cursor.x,my=(float)g_cursor.y; ClampSel(mx,my);
            int eid,en; g_snapOn=SnapNode(mx,my,g_selArrow,eid,en); g_snapX=mx; g_snapY=my;
            if(g_arrowEnd==0){ a.x0=mx; a.y0=my; a.cid0=g_snapOn?eid:0; a.cnode0=g_snapOn?en:-1; }
            else { a.x1=mx; a.y1=my; a.cid1=g_snapOn?eid:0; a.cnode1=g_snapOn?en:-1; } }
        else if(g_mode==EDIT && g_drawing){
            if(g_cur.type==4){ float px=(float)g_cursor.x,py=(float)g_cursor.y; ClampSel(px,py);
                D2D1_POINT_2F last=g_cur.stamps.back(); float d=hypotf(px-last.x,py-last.y); int n=(int)(d/max(1.0f,g_cur.brush*0.35f));
                for(int i=1;i<=n;i++) g_cur.stamps.push_back(D2D1::Point2F(last.x+(px-last.x)*i/n,last.y+(py-last.y)*i/n));
                g_cur.stamps.push_back(D2D1::Point2F(px,py));
            } else { g_cur.x1=(float)g_cursor.x; g_cur.y1=(float)g_cursor.y; ClampSel(g_cur.x1,g_cur.y1);
                if((GetKeyState(VK_SHIFT)&0x8000) && g_cur.type<=2){ float dx=g_cur.x1-g_cur.x0,dy=g_cur.y1-g_cur.y0,s=max(fabsf(dx),fabsf(dy)); g_cur.x1=g_cur.x0+(dx<0?-s:s); g_cur.y1=g_cur.y0+(dy<0?-s:s); ClampSel(g_cur.x1,g_cur.y1); }
                if(g_cur.type==3){ int eid,en; g_snapOn=SnapNode(g_cur.x1,g_cur.y1,-1,eid,en); g_cur.cid1=g_snapOn?eid:0; g_cur.cnode1=g_snapOn?en:-1; g_snapX=g_cur.x1; g_snapY=g_cur.y1; } // 箭头端点吸附+记录连接
            }
        }
        else if(g_mode==EDIT){ g_hoverBtn=-1; for(size_t i=0;i<g_btns.size();++i) if(PtIn(g_btns[i].rc,g_cursor.x,g_cursor.y)){ g_hoverBtn=(int)i; break; } g_hoverIdx = (g_typing||g_hoverBtn>=0||g_tool==4)? -1 : AnnoOrCornerAt(g_cursor.x,g_cursor.y); }
        else if(g_mode==SEL){ g_haveSnap=SnapAt(g_cursor.x,g_cursor.y,g_snap); }
        if(g_mode==EDIT && (g_moving||g_dragMode==3||g_dragMode==2||g_dragMode==4)) RefreshConnections(); // 连接的箭头随要素变形
        InvalidateRect(hwnd,nullptr,FALSE); return 0; }
    case WM_LBUTTONDOWN:{
        int x=g_cursor.x,y=g_cursor.y;
        if(g_mode==SEL){ g_start=g_cursor; g_selecting=true; g_sel={x,y,x,y}; SetCapture(hwnd); }
        else {
            if(g_tool==5 && g_fontMenu){ RECT panel; std::vector<RECT> rows; int tot=FontMenuLayout(panel,rows); int nf=(int)g_fontNames.size(); bool hit=false;
                for(int i=0;i<tot;i++) if(PtIn(rows[i],x,y)){ hit=true;
                    if(i<nf){ g_fontIdx=i; if(g_selIdx>=0&&g_selIdx<(int)g_annos.size()&&g_annos[g_selIdx].type==5) g_annos[g_selIdx].fontIdx=i; g_fontMenu=false; }
                    else if(i==nf){ g_fontMenu=false; InstallFont(hwnd); } else { g_fontMenu=false; UninstallFont(hwnd); } break; }
                if(!hit) g_fontMenu=false;
                InvalidateRect(hwnd,nullptr,FALSE); return 0; }
            bool onSlider=(g_sizeSlider.right>g_sizeSlider.left && PtIn(g_sizeSlider,x,y));
            if(onSlider){ g_sliding=true; SetSizeFromSlider(x); SetCapture(hwnd); InvalidateRect(hwnd,nullptr,FALSE); return 0; }
            if(g_typing){ CommitText(); InvalidateRect(hwnd,nullptr,FALSE); return 0; }
            // 马赛克优先：选中马赛克工具后，在选区内按下一律涂码，不选中/拖动/缩放已有要素
            if(g_tool==4 && PtIn(g_sel,x,y) && !PtIn(g_barRc,x,y) && !(g_subRc.right>g_subRc.left&&PtIn(g_subRc,x,y))){
                g_drawing=true; g_cur=Anno(); g_cur.type=4; g_cur.color=g_colorIdx; g_cur.width=WIDTHS[g_level];
                g_cur.brush=MBRUSH[g_level]; g_cur.mshape=g_mshape; float px=(float)x,py=(float)y; ClampSel(px,py); g_cur.stamps.push_back(D2D1::Point2F(px,py)); SetCapture(hwnd);
                InvalidateRect(hwnd,nullptr,FALSE); return 0; }
            // 选中箭头的首/尾节点：直接拖动该端点（可再吸附到别的要素节点）
            { int ae=ArrowEndAt(x,y); if(ae>=0){ g_dragMode=4; g_arrowEnd=ae; g_selIdx=g_selArrow; SetCapture(hwnd); InvalidateRect(hwnd,nullptr,FALSE); return 0; } }
            // 未选任何工具时(首次框选后的调整阶段)：拖选区手柄=缩放，拖选区边=移动整个选区
            if(g_tool==-1 && !PtIn(g_barRc,x,y) && !(g_subRc.right>g_subRc.left&&PtIn(g_subRc,x,y)) && AnnoAt(x,y)<0){
                int sh2=SelHandleAt(x,y); if(sh2>=0){ g_selRz=sh2; SetCapture(hwnd); InvalidateRect(hwnd,nullptr,FALSE); return 0; }
                if(PtIn(g_sel,x,y)||SelBorderHit(x,y)){ g_selMv=true; g_moveLast=g_cursor; SetCapture(hwnd); InvalidateRect(hwnd,nullptr,FALSE); return 0; } // 框内任意处都可移动整框
            }
            if(NearRotate(x,y)){ // 文字旋转
                Anno&a=g_annos[g_rotIdx]; float l,t,r,b; AnnoBBox(a,l,t,r,b); g_rotCx=(l+r)/2; g_rotCy=(t+b)/2;
                g_rotStart=atan2f((float)y-g_rotCy,(float)x-g_rotCx); g_rotBaseAngle=a.rot; g_dragMode=2; SetCapture(hwnd);
            }
            else if(NearArrowDot(x,y)){ g_dragMode=1; SetCapture(hwnd); }
            else if(PtIn(g_barRc,x,y)||(g_subRc.right>g_subRc.left&&PtIn(g_subRc,x,y))){ HandleBarClick(hwnd,x,y); }
            else if(g_tool==3){ // 箭头工具：节点→画箭头(吸附)；线框→移动该要素；空白→自由画箭头
                float sx=(float)x, sy=(float)y; int sid,sn; bool onNode=SnapNode(sx,sy,-1,sid,sn);
                if(onNode){ g_drawing=true; g_cur=Anno(); g_cur.type=3; g_cur.color=g_colorIdx; g_cur.width=WIDTHS[g_level];
                    g_cur.x0=g_cur.x1=sx; g_cur.y0=g_cur.y1=sy; g_cur.cid0=sid; g_cur.cnode0=sn; SetCapture(hwnd); }
                else { int hit=AnnoAt(x,y);
                    if(hit>=0){ g_selIdx=hit; g_selArrow=(g_annos[hit].type==3?hit:-1); g_moving=true; g_moveLast=g_cursor; SetCapture(hwnd); }
                    else if(PtIn(g_sel,x,y)){ g_drawing=true; g_cur=Anno(); g_cur.type=3; g_cur.color=g_colorIdx; g_cur.width=WIDTHS[g_level];
                        g_cur.x0=g_cur.x1=(float)x; g_cur.y0=g_cur.y1=(float)y; SetCapture(hwnd); }
                    else { g_selIdx=-1; g_selArrow=-1; }
                }
            }
            else {
                int hit=AnnoAt(x,y);
                // 角点缩放优先：对边线命中或当前悬停/选中的要素，即使角点在边线外（椭圆）也可抓
                int ct=(hit>=0)?hit:((g_selIdx>=0)?g_selIdx:g_hoverIdx);
                int cor=(ct>=0)?CornerAt(ct,x,y):-1;
                if(cor>=0){ g_dragMode=3; g_selIdx=ct; g_selArrow=(g_annos[ct].type==3?ct:-1); g_rzCorner=cor; g_rzOrig=g_annos[ct];
                    float l,t,r,b; AnnoBBox(g_annos[ct],l,t,r,b); float cxs[4]={l,r,r,l},cys[4]={t,t,b,b}; int anc=(cor+2)%4; g_rzAx=cxs[anc];g_rzAy=cys[anc];g_rzGx=cxs[cor];g_rzGy=cys[cor]; SetCapture(hwnd); }
                else if(hit>=0){ g_selIdx=hit; g_selArrow=(g_annos[hit].type==3?hit:-1); g_moving=true; g_moveLast=g_cursor; SetCapture(hwnd); }
                else if(g_tool==5 && PtIn(g_sel,x,y)){ PlaceTextEntry(hwnd,x,y); }
                else if(g_tool>=0 && PtIn(g_sel,x,y)){
                    g_drawing=true; g_cur=Anno(); g_cur.type=g_tool; g_cur.color=g_colorIdx; g_cur.width=WIDTHS[g_level];
                    if(g_tool==4){ g_cur.brush=MBRUSH[g_level]; g_cur.mshape=g_mshape; float px=(float)x,py=(float)y; ClampSel(px,py); g_cur.stamps.push_back(D2D1::Point2F(px,py)); }
                    else { g_cur.x0=g_cur.x1=(float)x; g_cur.y0=g_cur.y1=(float)y;
                        if(g_cur.type==3){ SnapNode(g_cur.x0,g_cur.y0,-1); g_cur.x1=g_cur.x0; g_cur.y1=g_cur.y0; } }
                    SetCapture(hwnd);
                } else { g_selIdx=-1; g_selArrow=-1; }
            }
        }
        InvalidateRect(hwnd,nullptr,FALSE); return 0; }
    case WM_LBUTTONUP:{
        ReleaseCapture();
        if(g_sliding){ g_sliding=false; InvalidateRect(hwnd,nullptr,FALSE); return 0; }
        if(g_selRz>=0){ g_selRz=-1; InvalidateRect(hwnd,nullptr,FALSE); return 0; }
        if(g_selMv){ g_selMv=false; InvalidateRect(hwnd,nullptr,FALSE); return 0; }
        if(g_mode==SEL && g_selecting){ g_selecting=false;
            if(g_sel.right-g_sel.left<8||g_sel.bottom-g_sel.top<8){ if(g_haveSnap){ g_sel=g_snap; EnterEdit(); } }
            else EnterEdit();
        } else if(g_dragMode){ g_dragMode=0; g_snapOn=false; }
        else if(g_moving){ g_moving=false; }
        else if(g_mode==EDIT && g_drawing){ g_drawing=false; g_snapOn=false;
            if(g_cur.type==4){ if(g_cur.stamps.size()>=1){ g_annos.push_back(g_cur); } }
            else if(fabs(g_cur.x1-g_cur.x0)>3||fabs(g_cur.y1-g_cur.y0)>3){ g_annos.push_back(g_cur); if(g_cur.type==3) g_selArrow=(int)g_annos.size()-1; }
        }
        InvalidateRect(hwnd,nullptr,FALSE); return 0; }
    case WM_LBUTTONDBLCLK:{
        if(g_mode==SEL){ // 磁吸阶段双击 = 取该窗口/模块并复制+自动粘贴
            if(g_haveSnap){ g_sel=g_snap; g_mode=EDIT; CopyToClipboard(hwnd); g_pasteAfter=true; PostQuitMessage(0); }
        } else if(g_mode==EDIT && !g_typing){ // 编辑阶段双击空白 = 完成复制+粘贴
            int x=g_cursor.x,y=g_cursor.y;
            if(!PtIn(g_barRc,x,y)&&!(g_subRc.right>g_subRc.left&&PtIn(g_subRc,x,y))&&AnnoAt(x,y)<0){ CopyToClipboard(hwnd); g_pasteAfter=true; PostQuitMessage(0); }
        }
        return 0; }
    case WM_RBUTTONDOWN:{
        if(g_mode==SEL){ int r,g,b; GetPixel(g_cursor.x,g_cursor.y,r,g,b); wchar_t hex[16]; swprintf(hex,16,L"#%02X%02X%02X",r,g,b);
            if(OpenClipboard(hwnd)){ EmptyClipboard(); size_t by=(wcslen(hex)+1)*2; HGLOBAL hg=GlobalAlloc(GMEM_MOVEABLE,by); memcpy(GlobalLock(hg),hex,by); GlobalUnlock(hg); SetClipboardData(CF_UNICODETEXT,hg); CloseClipboard(); }
            g_pasteAfter=true; PostQuitMessage(0);
        } return 0; }
    case WM_CHAR:
        if(g_typing){ wchar_t ch=(wchar_t)wParam;
            if(ch==8){ if(!g_typingText.empty()) g_typingText.pop_back(); }
            else if(ch==27){ g_typing=false; g_typingText.clear(); }
            else if(ch==13){ /* 回车在 WM_KEYDOWN 处理 */ }
            else if(ch>=32 || ch==9){ g_typingText.push_back(ch); }
            InvalidateRect(hwnd,nullptr,FALSE); return 0; }
        return 0;
    case WM_KEYDOWN:
        if(g_typing){
            if(wParam==VK_RETURN){ if(GetKeyState(VK_SHIFT)&0x8000) g_typingText.push_back(L'\n'); else CommitText(); InvalidateRect(hwnd,nullptr,FALSE); return 0; }
            if(wParam==VK_ESCAPE){ g_typing=false; g_typingText.clear(); InvalidateRect(hwnd,nullptr,FALSE); return 0; }
            return 0;
        }
        if(wParam==VK_ESCAPE){ PostQuitMessage(0); return 0; }
        if(wParam=='Z' && (GetKeyState(VK_CONTROL)&0x8000)){ if(!g_annos.empty()){ g_annos.pop_back(); g_selIdx=g_selArrow=-1; InvalidateRect(hwnd,nullptr,FALSE);} return 0; }
        if(wParam==VK_DELETE){ if(g_selIdx>=0&&g_selIdx<(int)g_annos.size()){ g_annos.erase(g_annos.begin()+g_selIdx); g_selIdx=g_selArrow=-1; } else if(!g_annos.empty()){ g_annos.pop_back(); } InvalidateRect(hwnd,nullptr,FALSE); return 0; }
        if(wParam=='S' && (GetKeyState(VK_CONTROL)&0x8000)){ if(g_mode==EDIT){ CommitText(); SavePng(hwnd);} return 0; }
        return 0;
    case WM_SETCURSOR:
        if(LOWORD(lParam)==HTCLIENT){
            POINT p; GetCursorPos(&p); ScreenToClient(hwnd,&p);
            LPCSTR c=IDC_ARROW;
            if(g_mode==EDIT){
                if(PtIn(g_barRc,p.x,p.y)||(g_subRc.right>g_subRc.left&&PtIn(g_subRc,p.x,p.y))) c=IDC_ARROW;
                else if(g_selRz>=0||g_selMv) c=IDC_ARROW;
                else if(g_tool==-1) c=IDC_ARROW;
                else if(g_moving||(!g_typing&&AnnoAt(p.x,p.y)>=0)) c=IDC_SIZEALL;
                else if(g_tool>=0) c=IDC_ARROW;   // 选了工具 → 箭头
            }
            SetCursor(LoadCursor(nullptr,c));
            return TRUE;
        }
        break;
    case WM_TIMER: if(g_typing) InvalidateRect(hwnd,nullptr,FALSE); return 0;
    case WM_PAINT:{ PAINTSTRUCT ps; BeginPaint(hwnd,&ps); Render(hwnd); EndPaint(hwnd,&ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,msg,wParam,lParam);
}

int WINAPI WinMain(HINSTANCE hInst,HINSTANCE,LPSTR,int){
    HMODULE sh=LoadLibraryW(L"shcore.dll");
    if(sh){ typedef HRESULT(WINAPI*F)(int); F f=(F)GetProcAddress(sh,"SetProcessDpiAwareness"); if(f) f(2); else SetProcessDPIAware(); } else SetProcessDPIAware();
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(!CaptureDesktop()) return 1; EnumWindowsForSnap();
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,&g_factory);
    g_factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND,D2D1_CAP_STYLE_ROUND,D2D1_CAP_STYLE_ROUND,D2D1_LINE_JOIN_ROUND),nullptr,0,&g_round);
    { float dashes[]={7.0f,4.0f}; g_factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT,D2D1_CAP_STYLE_FLAT,D2D1_CAP_STYLE_FLAT,D2D1_LINE_JOIN_MITER,10.0f,D2D1_DASH_STYLE_CUSTOM,0.0f),dashes,2,&g_dash); }
    ComputeMosaic();
    ComputeBlur();
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(&g_dwrite));
    RegisterFonts();
    CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&g_wic));
    LoadIcons();
    // 使用系统光标(标准箭头指针)，不加载自定义光标
    g_dwrite->CreateTextFormat(L"Microsoft YaHei",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,14.0f*UI,L"zh-cn",&g_tf);
    g_dwrite->CreateTextFormat(L"Microsoft YaHei",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,11.5f*UI,L"zh-cn",&g_tfSmall);
    g_dwrite->CreateTextFormat(L"Microsoft YaHei",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,15.0f*UI,L"zh-cn",&g_tfHint);
    g_tfHint->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    g_dwrite->CreateTextFormat(L"Segoe UI Symbol",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,20.0f*UI,L"en-us",&g_tfIcon);
    g_tfIcon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); g_tfIcon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g_dwrite->CreateTextFormat(L"Microsoft YaHei",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,12.0f*UI,L"zh-cn",&g_tfMenu);
    if(g_tfMenu) g_tfMenu->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    HICON hAppIco=(HICON)LoadImageW(hInst,MAKEINTRESOURCEW(1),IMAGE_ICON,0,0,LR_DEFAULTSIZE|LR_SHARED);
    WNDCLASSEXW wc; ZeroMemory(&wc,sizeof(wc)); wc.cbSize=sizeof(wc); wc.style=CS_DBLCLKS; wc.lpfnWndProc=WndProc; wc.hInstance=hInst; wc.hCursor=LoadCursor(nullptr,IDC_ARROW); wc.hIcon=hAppIco; wc.hIconSm=hAppIco; wc.lpszClassName=L"TatoMarkD2D"; RegisterClassExW(&wc);
    HWND hwnd=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,wc.lpszClassName,L"TatoMark",WS_POPUP,g_vx,g_vy,g_vw,g_vh,nullptr,nullptr,hInst,nullptr);
    if(hAppIco){ SendMessageW(hwnd,WM_SETICON,ICON_BIG,(LPARAM)hAppIco); SendMessageW(hwnd,WM_SETICON,ICON_SMALL,(LPARAM)hAppIco); }
    ShowWindow(hwnd,SW_SHOW); SetForegroundWindow(hwnd); SetFocus(hwnd);
    SetTimer(hwnd,1,300,nullptr);   // 文字输入光标闪烁
    MSG msg; while(GetMessage(&msg,nullptr,0,0)){ TranslateMessage(&msg); DispatchMessage(&msg); }
    DestroyWindow(hwnd);
    if(g_pasteAfter) SendPasteLater();     // 覆盖窗关闭后在焦点处自动粘贴
    DiscardDeviceResources(); SafeRelease(&g_tf); SafeRelease(&g_tfSmall); SafeRelease(&g_tfHint); SafeRelease(&g_tfIcon);
    SafeRelease(&g_wic); SafeRelease(&g_dwrite); SafeRelease(&g_factory); CoUninitialize();
    return 0;
}
