#include <windows.h>
#include <dwmapi.h>
#include <cstdio>
static LRESULT CALLBACK P(HWND h,UINT m,WPARAM w,LPARAM l){return DefWindowProcW(h,m,w,l);}
static void Pump(){MSG m;while(PeekMessageW(&m,0,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
HWND Mk(const wchar_t*c,COLORREF col,int x,int y,int w,int h,DWORD ex){WNDCLASSW a{};a.lpfnWndProc=P;a.hInstance=GetModuleHandleW(0);a.lpszClassName=c;a.hbrBackground=col==1?nullptr:CreateSolidBrush(col);RegisterClassW(&a);
 return CreateWindowExW(ex,c,L"",WS_POPUP|WS_VISIBLE,x,y,w,h,0,0,0,0);}
int main(){
 SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
 DWORD tw=WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE;
 HWND R=Mk(L"R",RGB(255,0,0),5000,100,200,200,tw); HWND G=Mk(L"G",RGB(0,255,0),5000,400,200,200,tw);
 BOOL c=TRUE;DwmSetWindowAttribute(R,DWMWA_CLOAK,&c,4);DwmSetWindowAttribute(G,DWMWA_CLOAK,&c,4);
 Pump();
 HWND H=Mk(L"H",1,100,100,600,300,tw|WS_EX_TOPMOST|WS_EX_NOREDIRECTIONBITMAP|WS_EX_TRANSPARENT);
 HTHUMBNAIL tr,tg;HRESULT h1=DwmRegisterThumbnail(H,R,&tr),h2=DwmRegisterThumbnail(H,G,&tg);
 DWM_THUMBNAIL_PROPERTIES p{};p.dwFlags=DWM_TNP_RECTDESTINATION|DWM_TNP_VISIBLE|DWM_TNP_OPACITY;p.fVisible=TRUE;p.opacity=255;
 p.rcDestination={0,0,200,200};DwmUpdateThumbnailProperties(tr,&p);
 p.rcDestination={100,0,300,200};p.opacity=128;DwmUpdateThumbnailProperties(tg,&p);
 for(int i=0;i<10;i++){Pump();DwmFlush();}
 HDC s=GetDC(0);
 printf("hr %08lx %08lx\nred only=%06lx overlap=%06lx green only=%06lx empty host=%06lx\n",h1,h2,GetPixel(s,150,200),GetPixel(s,250,200),GetPixel(s,350,200),GetPixel(s,550,200));
 ReleaseDC(0,s);DestroyWindow(H);Pump();DwmFlush();DwmFlush();
 s=GetDC(0);printf("after destroy empty=%06lx\n",GetPixel(s,550,200));ReleaseDC(0,s);
}
