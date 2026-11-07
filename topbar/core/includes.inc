
// Runs as a tool mod in its own process. Nothing polls: the bar redraws on Windows
// events, one timer per minute for the clock, and a short timer only while the pill moves.

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
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
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <setupapi.h>
#include <dbt.h>
#undef GetCurrentTime  // winrt headers need this macro gone
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>

// From iphlpapi. Windhawk includes windows.h first, which rules out winsock2.h,
// so these two are declared directly.
extern "C" DWORD WINAPI NotifyIpInterfaceChange(USHORT family, void* callback, void* context,
                                                BOOLEAN initialNotification, HANDLE* handle);
extern "C" DWORD WINAPI CancelMibChangeNotify2(HANDLE handle);


namespace {

