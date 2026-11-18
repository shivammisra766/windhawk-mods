#include <windows.h>
#include <dwmapi.h>
#include <cstdio>
static LRESULT CALLBACK P(HWND h,UINT m,WPARAM w,LPARAM l){return DefWindowProcW(h,m,w,l);}
static void Pump(){MSG m;while(PeekMessageW(&m,0,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
int n=0;
HWND Mk(COLORREF col,int x,int y,int w,int h,DWORD ex){wchar_t c[8];wsprintfW(c,L"C%d",n++);WNDCLASSW a{};a.lpfnWndProc=P;a.hInstance=GetModuleHandleW(0);a.lpszClassName=c;a.hbrBackground=col==1?nullptr:CreateSolidBrush(col);RegisterClassW(&a);
 return CreateWindowExW(ex,c,L"",WS_POPUP|WS_VISIBLE,x,y,w,h,0,0,0,0);}
void Run(int srcX,DWORD hostEx,const char*name){
 DWORD tw=WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE;
 HWND R=Mk(RGB(255,0,0),srcX,700,200,200,tw);BOOL c=TRUE;DwmSetWindowAttribute(R,DWMWA_CLOAK,&c,4);Pump();
 HWND H=Mk(hostEx&WS_EX_NOREDIRECTIONBITMAP?1:RGB(0,0,255),100,100,600,300,tw|WS_EX_TOPMOST|hostEx);
 HTHUMBNAIL t;DwmRegisterThumbnail(H,R,&t);
 DWM_THUMBNAIL_PROPERTIES p{};p.dwFlags=DWM_TNP_RECTDESTINATION|DWM_TNP_VISIBLE|DWM_TNP_OPACITY;p.fVisible=TRUE;p.opacity=255;p.rcDestination={0,0,200,200};DwmUpdateThumbnailProperties(t,&p);
 for(int i=0;i<10;i++){Pump();DwmFlush();}
 HDC s=GetDC(0);printf("%s: thumb=%06lx host=%06lx\n",name,GetPixel(s,150,200),GetPixel(s,550,200));ReleaseDC(0,s);
 DwmUnregisterThumbnail(t);DestroyWindow(H);DestroyWindow(R);Pump();DwmFlush();
}
int main(){SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
 Run(900,0,"onscreen src, normal host");
 Run(5000,0,"offscreen src, normal host");
 Run(900,WS_EX_NOREDIRECTIONBITMAP,"onscreen src, noredir host");
 Run(900,WS_EX_LAYERED,"onscreen src, layered host(no attrs)");
}
