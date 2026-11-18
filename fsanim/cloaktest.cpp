#include <windows.h>
#include <dwmapi.h>
#include <cstdio>
static LRESULT CALLBACK P(HWND h,UINT m,WPARAM w,LPARAM l){return DefWindowProcW(h,m,w,l);}
static void Pump(){MSG m;while(PeekMessageW(&m,0,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
int main(){
 SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
 WNDCLASSW a{};a.lpfnWndProc=P;a.hInstance=GetModuleHandleW(0);a.lpszClassName=L"RedA";a.hbrBackground=CreateSolidBrush(RGB(255,0,0));RegisterClassW(&a);
 WNDCLASSW b=a;b.lpszClassName=L"BlkB";b.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);RegisterClassW(&b);
 HWND A=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST,L"RedA",L"",WS_POPUP|WS_VISIBLE,100,100,300,300,0,0,0,0);
 Pump();DwmFlush();DwmFlush();
 BOOL c=TRUE;HRESULT hc=DwmSetWindowAttribute(A,DWMWA_CLOAK,&c,sizeof c);
 HWND B=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE,L"BlkB",L"",WS_POPUP|WS_VISIBLE,500,100,300,300,0,0,0,0);
 HTHUMBNAIL t;HRESULT hr=DwmRegisterThumbnail(B,A,&t);
 DWM_THUMBNAIL_PROPERTIES p{};p.dwFlags=DWM_TNP_RECTDESTINATION|DWM_TNP_VISIBLE|DWM_TNP_OPACITY;p.rcDestination={0,0,300,300};p.fVisible=TRUE;p.opacity=255;
 DwmUpdateThumbnailProperties(t,&p);
 for(int i=0;i<10;i++){Pump();DwmFlush();}
 HDC s=GetDC(0);COLORREF atB=GetPixel(s,650,250),atA=GetPixel(s,250,250);ReleaseDC(0,s);
 printf("cloak hr=%08lx thumb hr=%08lx  B=%06lx A=%06lx\n",hc,hr,atB,atA);
 DwmUnregisterThumbnail(t);DestroyWindow(B);DestroyWindow(A);
}
