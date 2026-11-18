#include <windows.h>
#include <dwmapi.h>
#include <cstdio>
static LRESULT CALLBACK P(HWND h,UINT m,WPARAM w,LPARAM l){if(m==WM_PAINT){PAINTSTRUCT ps;BeginPaint(h,&ps);EndPaint(h,&ps);return 0;}return DefWindowProcW(h,m,w,l);}
static void Pump(){MSG m;while(PeekMessageW(&m,0,0,0,PM_REMOVE)){DispatchMessageW(&m);}}
int main(){SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
 WNDCLASSW a{};a.lpfnWndProc=P;a.hInstance=GetModuleHandleW(0);a.lpszClassName=L"S";RegisterClassW(&a);
 HWND S=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_LAYERED|WS_EX_TRANSPARENT,L"S",L"",WS_POPUP|WS_VISIBLE,900,700,200,200,0,0,0,0);
 HDC s0=GetDC(0);COLORREF before=GetPixel(s0,1000,800);ReleaseDC(0,s0);
 BOOL c=TRUE;DwmSetWindowAttribute(S,DWMWA_CLOAK,&c,4);
 SetLayeredWindowAttributes(S,0,255,LWA_ALPHA);
 HDC d=GetDC(S);HBRUSH r=CreateSolidBrush(RGB(255,0,0));RECT rc{0,0,200,200};FillRect(d,&rc,r);ReleaseDC(S,d);GdiFlush();
 WNDCLASSW b=a;b.lpszClassName=L"H";RegisterClassW(&b);
 HWND H=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST|WS_EX_NOREDIRECTIONBITMAP|WS_EX_TRANSPARENT,L"H",L"",WS_POPUP|WS_VISIBLE,100,100,600,300,0,0,0,0);
 HTHUMBNAIL t;DwmRegisterThumbnail(H,S,&t);
 DWM_THUMBNAIL_PROPERTIES p{};p.dwFlags=DWM_TNP_RECTDESTINATION|DWM_TNP_VISIBLE|DWM_TNP_OPACITY;p.fVisible=TRUE;p.opacity=255;p.rcDestination={0,0,200,200};DwmUpdateThumbnailProperties(t,&p);
 for(int i=0;i<10;i++){Pump();DwmFlush();}
 HDC s=GetDC(0);printf("thumb=%06lx  srcspot=%06lx (before %06lx)\n",GetPixel(s,150,200),GetPixel(s,1000,800),before);ReleaseDC(0,s);
}
