#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdlib>
int main(int c,char**v){DWORD pid=atoi(v[1]),tid=atoi(v[2]);
 HANDLE p=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,0,pid);HANDLE t=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,0,tid);
 if(!p||!t){printf("open failed %lu\n",GetLastError());return 1;}
 SymSetOptions(SYMOPT_UNDNAME|SYMOPT_DEFERRED_LOADS|SYMOPT_EXACT_SYMBOLS*0);
 SymInitialize(p,nullptr,TRUE);
 SuspendThread(t);CONTEXT ctx{};ctx.ContextFlags=CONTEXT_FULL;GetThreadContext(t,&ctx);
 STACKFRAME64 f{};f.AddrPC.Offset=ctx.Rip;f.AddrPC.Mode=AddrModeFlat;f.AddrFrame.Offset=ctx.Rbp;f.AddrFrame.Mode=AddrModeFlat;f.AddrStack.Offset=ctx.Rsp;f.AddrStack.Mode=AddrModeFlat;
 for(int i=0;i<60&&StackWalk64(IMAGE_FILE_MACHINE_AMD64,p,t,&f,&ctx,nullptr,SymFunctionTableAccess64,SymGetModuleBase64,nullptr);i++){
  char buf[sizeof(SYMBOL_INFO)+256]{};auto*s=(SYMBOL_INFO*)buf;s->SizeOfStruct=sizeof(SYMBOL_INFO);s->MaxNameLen=255;DWORD64 d=0;
  IMAGEHLP_MODULE64 m{};m.SizeOfStruct=sizeof m;SymGetModuleInfo64(p,f.AddrPC.Offset,&m);
  if(SymFromAddr(p,f.AddrPC.Offset,&d,s))printf("%2d %s!%s+0x%llx\n",i,m.ModuleName,s->Name,d);else printf("%2d %s+0x%llx\n",i,m.ModuleName,f.AddrPC.Offset-m.BaseOfImage);}
 ResumeThread(t);}
