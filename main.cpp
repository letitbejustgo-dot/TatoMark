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
// 角点缩放 (dragMode 3)
static int   g_rzCorner=-1; static float g_rzAx=0,g_rzAy=0,g_rzGx=0,g_rzGy=0; static Anno g_rzOrig;
static ID2D1StrokeStyle* g_round=nullptr;
static const float MBRUSH[3]={27,51,84};
static const float TSIZE[3]={24,40,64};

// 文字/字体
static const wchar_t* FONT_FAM[2] = { L"Source Han Serif CN", L"HappyZcool-2016" };
static const wchar_t* FONT_NAME[2]= { L"思源宋体", L"站酷快乐体" };
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
static void CommitText();

// 马赛克位图(整屏像素化)+ 位图画刷
static std::vector<BYTE> g_mosaicPixels;
static ID2D1Bitmap*      g_mosaicBmp=nullptr;
static ID2D1BitmapBrush* g_mosaicBrush=nullptr;

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
}
static void DiscardDeviceResources(){ for(auto&kv:g_iconBmp) if(kv.second) kv.second->Release(); g_iconBmp.clear(); SafeRelease(&g_mosaicBrush); SafeRelease(&g_mosaicBmp); SafeRelease(&g_shot); SafeRelease(&g_rt); }

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
    long long key=(long long)fi*100000+sz; auto it=g_tfCache.find(key); if(it!=g_tfCache.end()) return it->second;
    IDWriteTextFormat* f=nullptr;
    g_dwrite->CreateTextFormat(FONT_FAM[fi],g_fontColl,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,(float)sz,L"zh-cn",&f);
    if(f) g_tfCache[key]=f; return f;
}
static void TextSize(const Anno&a,float&w,float&h){
    IDWriteTextFormat* f=GetTextFmt(a.fontIdx,(int)a.fontSize); if(!f){w=h=0;return;}
    IDWriteTextLayout* lay=nullptr; g_dwrite->CreateTextLayout(a.text.c_str(),(UINT32)a.text.size(),f,4000,4000,&lay);
    if(!lay){w=h=0;return;} DWRITE_TEXT_METRICS m; lay->GetMetrics(&m); w=m.width; h=m.height; lay->Release();
}
static std::wstring ExeDir(){ wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr,p,MAX_PATH); std::wstring s(p); size_t k=s.find_last_of(L"\\/"); return k==std::wstring::npos?L"":s.substr(0,k+1); }
static void RegisterFonts(){
    std::wstring d=ExeDir()+L"fonts\\";
    g_fontFiles={ d+L"SourceHanSerifCN-Regular.ttf", d+L"ZhanKuKuaiLeTi.ttf" };
    // 系统级注册（供 GDI/其他），并构建自定义字体集（供 DirectWrite 直接用文件）
    for(auto&p:g_fontFiles) AddFontResourceExW(p.c_str(),0,0);
    g_fontLoader=new FontCollLoader();
    if(SUCCEEDED(g_dwrite->RegisterFontCollectionLoader(g_fontLoader))){
        static const wchar_t* key=L"tatomark";
        g_dwrite->CreateCustomFontCollection(g_fontLoader,key,(UINT32)((wcslen(key)+1)*sizeof(wchar_t)),&g_fontColl);
    }
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
    if(a.type==5){ IDWriteTextFormat* f=GetTextFmt(a.fontIdx,(int)a.fontSize); if(f){ br->SetColor(PALETTE[a.color]); rt->DrawText(a.text.c_str(),(UINT32)a.text.size(),f,D2D1::RectF(a.x0+ox,a.y0+oy,a.x0+ox+4000,a.y0+oy+4000),br);} return; }
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
        float sh=40*UI, step=28*UI, sp=12*UI, fw=tx5?150*UI:0;
        float cw = mo ? (sp+3*step+14*UI+2*step+sp) : (sp+3*step+14*UI+6*step+fw+sp);
        float sx=min((float)max((long)tx,8L),(float)(g_vw-cw-8));
        float sy=ty+h+10;
        if(sy+sh>g_vh-8) sy=ty-sh-10;
        g_subRc={(LONG)sx,(LONG)sy,(LONG)(sx+cw),(LONG)(sy+sh)};
        float x=sx+sp, cy=sy+sh/2; g_fontBtn={0,0,0,0};
        for(int i=0;i<3;i++){ RECT r={(LONG)x,(LONG)(cy-step/2),(LONG)(x+step),(LONG)(cy+step/2)}; g_lvlSw.push_back({r,i}); x+=step; }
        x+=14*UI;
        if(mo){ for(int i=0;i<2;i++){ RECT r={(LONG)x,(LONG)(cy-step/2),(LONG)(x+step),(LONG)(cy+step/2)}; g_shpSw.push_back({r,i}); x+=step; } }
        else  { for(int i=0;i<6;i++){ RECT r={(LONG)x,(LONG)(cy-step/2),(LONG)(x+step),(LONG)(cy+step/2)}; g_colSw.push_back({r,i}); x+=step; }
                if(tx5){ x+=8*UI; g_fontBtn={(LONG)x,(LONG)(cy-14*UI),(LONG)(x+fw-8*UI),(LONG)(cy+14*UI)}; } }
    } else g_subRc={0,0,0,0};
}
static bool PtIn(const RECT&r,int x,int y){ return x>=r.left&&x<=r.right&&y>=r.top&&y<=r.bottom; }

// ---------- 绘制工具栏 ----------
static void PaintPill(const RECT& r, ID2D1SolidColorBrush* br){
    float rad=12*UI;
    br->SetColor(Col(255,255,255));
    D2D1_ROUNDED_RECT rr=D2D1::RoundedRect(D2D1::RectF(r.left,r.top,r.right,r.bottom),rad,rad);
    g_rt->FillRoundedRectangle(rr,br);
    br->SetColor(Col(228,230,234)); g_rt->DrawRoundedRectangle(rr,br,1.0f);
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
    bool active=(b.kind==0 && b.id==g_tool);
    if(active){ br->SetColor(Col(225,246,234)); g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(b.rc.left+3,b.rc.top+3,b.rc.right-3,b.rc.bottom-3),8,8),br); }
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
    float ox=cx+20, oy=cy+20;
    if(ox+D>g_vw) ox=cx-20-D; if(oy+D+phH>g_vh) oy=cy-20-(D+phH);
    g_rt->DrawBitmap(g_shot, D2D1::RectF(ox,oy,ox+D,oy+D),1.0f,
        D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
        D2D1::RectF((float)(cx-half),(float)(cy-half),(float)(cx+half+1),(float)(cy+half+1)));
    br->SetColor(Col(19,192,96));
    float mid=ox+half*Z+Z/2.0f;
    for(float a=oy;a<oy+D;a+=6) g_rt->DrawLine(D2D1::Point2F(mid,a),D2D1::Point2F(mid,a+3),br,UI);
    for(float a=ox;a<ox+D;a+=6) g_rt->DrawLine(D2D1::Point2F(a,oy+half*Z+Z/2.0f),D2D1::Point2F(a+3,oy+half*Z+Z/2.0f),br,UI);
    g_rt->DrawRectangle(D2D1::RectF(ox+half*Z,oy+half*Z,ox+half*Z+Z,oy+half*Z+Z),br,2.0f);
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
            // 可拖动要素：灰色长虚线框(压在线条上) + 四周圆点
            { int tgt = (g_moving||g_dragMode==3)? g_selIdx : g_hoverIdx;
              if(tgt>=0 && tgt<(int)g_annos.size()){
                float l,t,r,b; AnnoBBox(g_annos[tgt],l,t,r,b);
                br->SetColor(Col(154,160,166));
                g_rt->DrawRectangle(D2D1::RectF(l,t,r,b),br,2.0f,g_dash);
                float mx=(l+r)/2,my=(t+b)/2, nr=3.5f*UI;
                float px[8]={l,mx,r,r,r,mx,l,l}, py[8]={t,t,t,my,b,b,b,my};
                for(int i=0;i<8;i++){ br->SetColor(Col(255,255,255)); g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px[i],py[i]),nr,nr),br); br->SetColor(Col(154,160,166)); g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(px[i],py[i]),nr,nr),br,1.0f); }
              }
            }
            // 选中箭头的弯折控制点（曲线中点）
            if(g_selArrow>=0 && g_selArrow<(int)g_annos.size() && g_annos[g_selArrow].type==3){
                const Anno& a=g_annos[g_selArrow]; float cx=a.hasCtrl?a.cx:(a.x0+a.x1)/2, cy=a.hasCtrl?a.cy:(a.y0+a.y1)/2;
                float mx=0.25f*a.x0+0.5f*cx+0.25f*a.x1, my=0.25f*a.y0+0.5f*cy+0.25f*a.y1;
                br->SetColor(Col(19,192,96)); g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(mx,my),2.6f*UI,2.6f*UI),br);
                br->SetColor(Col(255,255,255)); g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(mx,my),2.6f*UI,2.6f*UI),br,1.0f);
                // 右上角旋转手柄，小巧
                float bl,bt,brr,bb; AnnoBBox(a,bl,bt,brr,bb); float rr2=4.0f*UI;
                br->SetColor(Col(255,255,255)); g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(brr,bt),rr2,rr2),br);
                br->SetColor(Col(19,192,96)); g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(brr,bt),rr2,rr2),br,1.2f);
                RECT hr={(LONG)(brr-rr2),(LONG)(bt-rr2),(LONG)(brr+rr2),(LONG)(bt+rr2)}; DrawIconBmp("rotate",hr,0.82f);
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
            // 工具栏
            PaintPill(g_barRc,br);
            for(auto&b:g_btns) PaintIcon(b,br);
            if(g_tool>=0 && g_subRc.right>g_subRc.left){
                PaintPill(g_subRc,br);
                for(auto&sw:g_lvlSw){ float cx=(sw.rc.left+sw.rc.right)/2.0f, cy=(sw.rc.top+sw.rc.bottom)/2.0f; float rr=(3+sw.idx*4)*UI/1.6f; br->SetColor(sw.idx==g_level?Col(19,192,96):Col(176,182,189)); g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy),rr,rr),br); }
                for(auto&sw:g_colSw){ D2D1_RECT_F r=D2D1::RectF(sw.rc.left+6,sw.rc.top+6,sw.rc.right-6,sw.rc.bottom-6); br->SetColor(PALETTE[sw.idx]); g_rt->FillRoundedRectangle(D2D1::RoundedRect(r,4,4),br); if(sw.idx==g_colorIdx){ br->SetColor(Col(19,192,96)); g_rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sw.rc.left+3,sw.rc.top+3,sw.rc.right-3,sw.rc.bottom-3),6,6),br,2.0f); } }
                for(auto&sw:g_shpSw){ float cx=(sw.rc.left+sw.rc.right)/2.0f, cy=(sw.rc.top+sw.rc.bottom)/2.0f, s=10*UI; br->SetColor(sw.idx==g_mshape?Col(19,192,96):Col(120,126,132)); if(sw.idx==0) g_rt->DrawRectangle(D2D1::RectF(cx-s,cy-s,cx+s,cy+s),br,2.0f); else g_rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy),s,s),br,2.0f); }
                if(g_fontBtn.right>g_fontBtn.left){ br->SetColor(Col(65,70,75)); std::wstring fn=std::wstring(FONT_NAME[g_fontIdx])+L" ▾"; g_rt->DrawText(fn.c_str(),(UINT32)fn.size(),g_tf,D2D1::RectF(g_fontBtn.left,g_fontBtn.top-4,g_fontBtn.right,g_fontBtn.bottom+4),br); }
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
        if(b.kind==0){ g_tool=(g_tool==b.id?-1:b.id); LayoutToolbar(); }
        else if(b.kind==1){ if(!g_annos.empty()){ g_annos.pop_back(); if(g_selArrow>=(int)g_annos.size()) g_selArrow=-1; } }
        else if(b.kind==4){ CommitText(); SavePng(hwnd); }
        else if(b.kind==2){ PostQuitMessage(0); }
        else if(b.kind==3){ CommitText(); CopyToClipboard(hwnd); g_pasteAfter=true; PostQuitMessage(0); }
        return true; } }
    for(auto&sw:g_lvlSw){ if(PtIn(sw.rc,x,y)){ g_level=sw.idx; return true; } }
    for(auto&sw:g_colSw){ if(PtIn(sw.rc,x,y)){ g_colorIdx=sw.idx; if(g_selIdx>=0&&g_annos[g_selIdx].type!=4) g_annos[g_selIdx].color=sw.idx; return true; } }
    for(auto&sw:g_shpSw){ if(PtIn(sw.rc,x,y)){ g_mshape=sw.idx; return true; } }
    if(g_fontBtn.right>g_fontBtn.left && PtIn(g_fontBtn,x,y)){ g_fontIdx=(g_fontIdx+1)%2; if(g_selIdx>=0&&g_annos[g_selIdx].type==5) g_annos[g_selIdx].fontIdx=g_fontIdx; return true; }
    return false;
}
static bool NearArrowDot(int x,int y){
    if(g_selArrow<0||g_selArrow>=(int)g_annos.size()||g_annos[g_selArrow].type!=3) return false;
    const Anno&a=g_annos[g_selArrow]; float cx=a.hasCtrl?a.cx:(a.x0+a.x1)/2, cy=a.hasCtrl?a.cy:(a.y0+a.y1)/2;
    float mx=0.25f*a.x0+0.5f*cx+0.25f*a.x1, my=0.25f*a.y0+0.5f*cy+0.25f*a.y1;
    return (x-mx)*(x-mx)+(y-my)*(y-my) <= (7*UI)*(7*UI);
}
static void ClampSel(float&x,float&y){ x=min(max(x,(float)g_sel.left),(float)g_sel.right); y=min(max(y,(float)g_sel.top),(float)g_sel.bottom); }
static bool NearRotate(int x,int y){
    if(g_selArrow<0||g_selArrow>=(int)g_annos.size()||g_annos[g_selArrow].type!=3) return false;
    float l,t,r,b; AnnoBBox(g_annos[g_selArrow],l,t,r,b);
    return (x-r)*(x-r)+(y-t)*(y-t) <= (7*UI)*(7*UI);
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
static int CornerAt(int idx,int x,int y){ if(g_annos[idx].type==4) return -1; float l,t,r,b; AnnoBBox(g_annos[idx],l,t,r,b); float cs=(9*UI)*(9*UI); float cx[4]={l,r,r,l},cy[4]={t,t,b,b}; for(int i=0;i<4;i++){ float dx=x-cx[i],dy=y-cy[i]; if(dx*dx+dy*dy<=cs) return i; } return -1; }
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
    g_typeFont=g_fontIdx; g_typeSize=TSIZE[g_level]; g_typeColor=g_colorIdx;
}
static void CommitText(){
    if(!g_typing) return; std::wstring s=g_typingText; g_typing=false; g_typingText.clear();
    bool empty=true; for(wchar_t c:s) if(c!=L' '&&c!=L'\r'&&c!=L'\n'&&c!=L'\t'){empty=false;break;}
    if(!empty){ Anno a; a.type=5; a.x0=(float)g_typePos.x; a.y0=(float)g_typePos.y; a.color=g_typeColor; a.fontIdx=g_typeFont; a.fontSize=g_typeSize; a.text=s; g_annos.push_back(a); g_selIdx=(int)g_annos.size()-1; }
}
static void SendPasteLater(){ Sleep(180); INPUT in[4]={}; for(int i=0;i<4;i++) in[i].type=INPUT_KEYBOARD; in[0].ki.wVk=VK_CONTROL; in[1].ki.wVk='V'; in[2].ki.wVk='V'; in[2].ki.dwFlags=KEYEVENTF_KEYUP; in[3].ki.wVk=VK_CONTROL; in[3].ki.dwFlags=KEYEVENTF_KEYUP; SendInput(4,in,sizeof(INPUT)); }

LRESULT CALLBACK WndProc(HWND hwnd,UINT msg,WPARAM wParam,LPARAM lParam){
    switch(msg){
    case WM_MOUSEMOVE:{
        g_cursor.x=GET_X_LPARAM(lParam); g_cursor.y=GET_Y_LPARAM(lParam);
        if(g_selecting){ g_sel.left=min((LONG)g_start.x,(LONG)g_cursor.x); g_sel.top=min((LONG)g_start.y,(LONG)g_cursor.y); g_sel.right=max((LONG)g_start.x,(LONG)g_cursor.x); g_sel.bottom=max((LONG)g_start.y,(LONG)g_cursor.y); }
        else if(g_dragMode==1 && g_selArrow>=0){ Anno&a=g_annos[g_selArrow]; float mx=(float)g_cursor.x,my=(float)g_cursor.y; ClampSel(mx,my); a.hasCtrl=true; a.cx=2*mx-0.5f*(a.x0+a.x1); a.cy=2*my-0.5f*(a.y0+a.y1); }
        else if(g_dragMode==2 && g_selArrow>=0){ float ang=atan2f((float)g_cursor.y-g_rotCy,(float)g_cursor.x-g_rotCx)-g_rotStart; float ca=cosf(ang),sa=sinf(ang);
            auto rot=[&](D2D1_POINT_2F p){ float dx=p.x-g_rotCx,dy=p.y-g_rotCy; return D2D1::Point2F(g_rotCx+dx*ca-dy*sa,g_rotCy+dx*sa+dy*ca); };
            D2D1_POINT_2F r0=rot(g_rotBase[0]),r1=rot(g_rotBase[1]),rc=rot(g_rotBase[2]); ClampSel(r0.x,r0.y); ClampSel(r1.x,r1.y); ClampSel(rc.x,rc.y);
            Anno&a=g_annos[g_selArrow]; a.x0=r0.x;a.y0=r0.y;a.x1=r1.x;a.y1=r1.y; a.hasCtrl=true; a.cx=rc.x;a.cy=rc.y; }
        else if(g_dragMode==3 && g_selIdx>=0){ float mx=(float)g_cursor.x,my=(float)g_cursor.y; ClampSel(mx,my); ResizeTo(mx,my); }
        else if(g_moving && g_selIdx>=0){ float dx=(float)(g_cursor.x-g_moveLast.x), dy=(float)(g_cursor.y-g_moveLast.y); float l,t,r,b; AnnoBBox(g_annos[g_selIdx],l,t,r,b); dx=max((float)g_sel.left-l,min(dx,(float)g_sel.right-r)); dy=max((float)g_sel.top-t,min(dy,(float)g_sel.bottom-b)); MoveAnno(g_selIdx,dx,dy); g_moveLast=g_cursor; }
        else if(g_mode==EDIT && g_drawing){
            if(g_cur.type==4){ float px=(float)g_cursor.x,py=(float)g_cursor.y; ClampSel(px,py);
                D2D1_POINT_2F last=g_cur.stamps.back(); float d=hypotf(px-last.x,py-last.y); int n=(int)(d/max(1.0f,g_cur.brush*0.35f));
                for(int i=1;i<=n;i++) g_cur.stamps.push_back(D2D1::Point2F(last.x+(px-last.x)*i/n,last.y+(py-last.y)*i/n));
                g_cur.stamps.push_back(D2D1::Point2F(px,py));
            } else { g_cur.x1=(float)g_cursor.x; g_cur.y1=(float)g_cursor.y; ClampSel(g_cur.x1,g_cur.y1);
                if((GetKeyState(VK_SHIFT)&0x8000) && g_cur.type<=2){ float dx=g_cur.x1-g_cur.x0,dy=g_cur.y1-g_cur.y0,s=max(fabsf(dx),fabsf(dy)); g_cur.x1=g_cur.x0+(dx<0?-s:s); g_cur.y1=g_cur.y0+(dy<0?-s:s); ClampSel(g_cur.x1,g_cur.y1); }
            }
        }
        else if(g_mode==EDIT){ g_hoverBtn=-1; for(size_t i=0;i<g_btns.size();++i) if(PtIn(g_btns[i].rc,g_cursor.x,g_cursor.y)){ g_hoverBtn=(int)i; break; } g_hoverIdx = (g_typing||g_hoverBtn>=0)? -1 : AnnoOrCornerAt(g_cursor.x,g_cursor.y); }
        else if(g_mode==SEL){ g_haveSnap=SnapAt(g_cursor.x,g_cursor.y,g_snap); }
        InvalidateRect(hwnd,nullptr,FALSE); return 0; }
    case WM_LBUTTONDOWN:{
        int x=g_cursor.x,y=g_cursor.y;
        if(g_mode==SEL){ g_start=g_cursor; g_selecting=true; g_sel={x,y,x,y}; SetCapture(hwnd); }
        else {
            if(g_typing){ CommitText(); InvalidateRect(hwnd,nullptr,FALSE); return 0; }
            if(NearRotate(x,y)){ Anno&a=g_annos[g_selArrow]; g_rotCx=(a.x0+a.x1)/2; g_rotCy=(a.y0+a.y1)/2; g_rotBase[0]=D2D1::Point2F(a.x0,a.y0); g_rotBase[1]=D2D1::Point2F(a.x1,a.y1); g_rotBase[2]=a.hasCtrl?D2D1::Point2F(a.cx,a.cy):D2D1::Point2F(g_rotCx,g_rotCy); g_rotStart=atan2f((float)y-g_rotCy,(float)x-g_rotCx); g_dragMode=2; SetCapture(hwnd); }
            else if(NearArrowDot(x,y)){ g_dragMode=1; SetCapture(hwnd); }
            else if(PtIn(g_barRc,x,y)||(g_subRc.right>g_subRc.left&&PtIn(g_subRc,x,y))){ HandleBarClick(hwnd,x,y); }
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
                    else { g_cur.x0=g_cur.x1=(float)x; g_cur.y0=g_cur.y1=(float)y; }
                    SetCapture(hwnd);
                } else { g_selIdx=-1; g_selArrow=-1; }
            }
        }
        InvalidateRect(hwnd,nullptr,FALSE); return 0; }
    case WM_LBUTTONUP:{
        ReleaseCapture();
        if(g_mode==SEL && g_selecting){ g_selecting=false;
            if(g_sel.right-g_sel.left<8||g_sel.bottom-g_sel.top<8){ if(g_haveSnap){ g_sel=g_snap; EnterEdit(); } }
            else EnterEdit();
        } else if(g_dragMode){ g_dragMode=0; }
        else if(g_moving){ g_moving=false; }
        else if(g_mode==EDIT && g_drawing){ g_drawing=false;
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
            LPCSTR c=IDC_CROSS;
            if(g_mode==EDIT){
                if(PtIn(g_barRc,p.x,p.y)||(g_subRc.right>g_subRc.left&&PtIn(g_subRc,p.x,p.y))) c=IDC_ARROW;
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
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(&g_dwrite));
    RegisterFonts();
    CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&g_wic));
    LoadIcons();
    g_dwrite->CreateTextFormat(L"Microsoft YaHei",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,14.0f*UI,L"zh-cn",&g_tf);
    g_dwrite->CreateTextFormat(L"Microsoft YaHei",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,11.5f*UI,L"zh-cn",&g_tfSmall);
    g_dwrite->CreateTextFormat(L"Microsoft YaHei",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,15.0f*UI,L"zh-cn",&g_tfHint);
    g_tfHint->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    g_dwrite->CreateTextFormat(L"Segoe UI Symbol",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,20.0f*UI,L"en-us",&g_tfIcon);
    g_tfIcon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); g_tfIcon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    WNDCLASSEXW wc; ZeroMemory(&wc,sizeof(wc)); wc.cbSize=sizeof(wc); wc.style=CS_DBLCLKS; wc.lpfnWndProc=WndProc; wc.hInstance=hInst; wc.hCursor=LoadCursor(nullptr,IDC_CROSS); wc.lpszClassName=L"TatoMarkD2D"; RegisterClassExW(&wc);
    HWND hwnd=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,wc.lpszClassName,L"TatoMark",WS_POPUP,g_vx,g_vy,g_vw,g_vh,nullptr,nullptr,hInst,nullptr);
    ShowWindow(hwnd,SW_SHOW); SetForegroundWindow(hwnd); SetFocus(hwnd);
    SetTimer(hwnd,1,300,nullptr);   // 文字输入光标闪烁
    MSG msg; while(GetMessage(&msg,nullptr,0,0)){ TranslateMessage(&msg); DispatchMessage(&msg); }
    DestroyWindow(hwnd);
    if(g_pasteAfter) SendPasteLater();     // 覆盖窗关闭后在焦点处自动粘贴
    DiscardDeviceResources(); SafeRelease(&g_tf); SafeRelease(&g_tfSmall); SafeRelease(&g_tfHint); SafeRelease(&g_tfIcon);
    SafeRelease(&g_wic); SafeRelease(&g_dwrite); SafeRelease(&g_factory); CoUninitialize();
    return 0;
}
