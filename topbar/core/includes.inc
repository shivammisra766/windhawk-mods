
// Runs as a tool mod in its own process. Nothing polls: the bar redraws on Windows
// events, one timer per minute for the clock, and a short timer only while the pill moves.

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <ole2.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <sddl.h>
#include <wininet.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <atomic>
#include <unordered_map>
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// From iphlpapi. Windhawk includes windows.h first, which rules out winsock2.h,
// so these two are declared directly.
extern "C" DWORD WINAPI NotifyIpInterfaceChange(USHORT family, void* callback, void* context,
                                                BOOLEAN initialNotification, HANDLE* handle);
extern "C" DWORD WINAPI CancelMibChangeNotify2(HANDLE handle);


namespace {

