// ---- Alerts (P6b): Caps Lock, battery, Bluetooth, DND, notifications, clipboard, devices, privacy dots ----
// One module, one queue (specs/alerts.md section 2). Everything that mutates the queue runs on the UI thread;
// workers (keyboard hook, Bluetooth, notification listener, registry watcher) only post. The module owns a
// hidden top-level window: WM_DEVICECHANGE needs a top-level window, and the registered messages sent by
// panel-split (toast shown) and tray-bridge (volume popup shown) are addressed to it by class name.
// Not built here, by decision: Progress, Num Lock, privacy-dot tap. DND off goes through QuietHoursSettings (levels.inc).

namespace wfn = winrt::Windows::Foundation;
namespace wun = winrt::Windows::UI::Notifications;
namespace wunm = winrt::Windows::UI::Notifications::Management;
namespace wbt = winrt::Windows::Devices::Bluetooth;
namespace wde = winrt::Windows::Devices::Enumeration;

constexpr wchar_t kAlertsClass[] = L"WindhawkTopBarAlerts";
constexpr UINT WM_AL_CAPS = WM_APP + 40;     // lParam: Caps Lock on.
constexpr UINT WM_AL_PRIVACY = WM_APP + 41;  // A ConsentStore key changed.
constexpr UINT_PTR kAlTick = 1, kAlHeartbeat = 2, kAlClipboard = 3, kAlOsd = 4, kAlPrivacy = 5;

std::atomic<HWND> g_alHwnd{nullptr};
HANDLE g_alStop = nullptr;
HANDLE g_alWake = nullptr;  // Auto-reset: a toast appeared, read the notification list.
UINT g_alMsgToast = 0, g_alMsgOsd = 0;
std::atomic<bool> g_alHookSeen{false};
wunm::UserNotificationListener g_alListener{nullptr};

const wchar_t* AlT(const wchar_t* key, const wchar_t* en) {
    const wchar_t* v = Loc(key);
    return v == key ? en : v;
}

std::wstring AlFmt(const wchar_t* fmt, ...) {
    wchar_t buf[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

enum class AK { Caps, Battery, Bluetooth, Device, Dnd, Notification, Clipboard, Volume, Brightness, Phone };
constexpr UINT WM_AL_LEVEL = WM_APP + 42;  // wParam: volume 0-100 (-1 unknown) | 0x10000 muted; lParam: brightness (-1 unknown).
HANDLE g_alLevelRead = nullptr, g_alLevelSet = nullptr;  // Auto-reset: read both levels / apply the requested one.
std::atomic<int> g_alSetVolume{-1}, g_alSetBright{-1};
HANDLE g_alDndOff = nullptr;               // Auto-reset: turn Do Not Disturb off (on the level thread, it is MTA).
constexpr UINT WM_AL_DNDFAIL = WM_APP + 43;  // That failed: open Settings instead.
DWORD WINAPI AlLevelThread(void*);  // alerts/levels.inc
// Level card size, DIP: normal, and while hovered (bigger, so it is easy to drag).
constexpr float kAlLevelW = 280.0f, kAlLevelH = 44.0f, kAlLevelHoverW = 360.0f, kAlLevelHoverH = 64.0f;

struct AItem {
    AK kind = AK::Caps;
    std::wstring title, sub, hint;
    std::wstring key;     // Bluetooth: device id. Notification: AUMID.
    std::shared_ptr<Pixmap> icon;
    int ms = 3000;
    int remaining = 0;
    int level = -1;       // Battery percent, for the glyph.
    bool charging = false;
    bool danger = false;  // Red status dot.
    bool good = false;    // Green status dot.
    bool on = false;      // Caps: now on.
    uint32_t nid = 0;     // Notification id in the bell list.
    bool more = false;    // The "+N more" line.
};

int AlPriority(AK k) {
    switch (k) {
        case AK::Volume:
        case AK::Brightness: return 95;  // Answers a key press: above everything.
        case AK::Caps: return 90;
        case AK::Notification: return 60;
        case AK::Clipboard: return 10;
        default: return 40;
    }
}

// ---- Decoding a notification logo into a Pixmap (WIC, premultiplied BGRA) ----
std::shared_ptr<Pixmap> AlDecodeImage(const std::vector<uint8_t>& bytes) {
    std::shared_ptr<Pixmap> out;
    if (bytes.empty()) return out;
    Com<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.Put())))) return out;
    Com<IWICStream> stream;
    if (FAILED(wic->CreateStream(stream.Put())) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), (DWORD)bytes.size()))) return out;
    Com<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, dec.Put()))) return out;
    Com<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, frame.Put()))) return out;
    Com<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(conv.Put())) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return out;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (!w || !h || w > 512 || h > 512) return out;
    auto pm = std::make_shared<Pixmap>();
    pm->w = (int)w;
    pm->h = (int)h;
    pm->px.resize((size_t)w * h);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, (UINT)(pm->px.size() * 4), reinterpret_cast<BYTE*>(pm->px.data())))) return out;
    return pm;
}

ID2D1Bitmap* AlBitmap(ID2D1RenderTarget* rt, Pixmap& p) {
    if (p.bmp && p.gen == g_isSurface.Generation()) return p.bmp.Get();
    p.bmp.Reset();
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(rt->CreateBitmap(D2D1::SizeU(p.w, p.h), p.px.data(), p.w * 4, &props, p.bmp.Put()))) return nullptr;
    p.gen = g_isSurface.Generation();
    return p.bmp.Get();
}

std::shared_ptr<Pixmap> AlIconOfExe(const wchar_t* path) {
    SHFILEINFOW fi{};
    if (!SHGetFileInfoW(path, 0, &fi, sizeof(fi), SHGFI_ICON | SHGFI_LARGEICON) || !fi.hIcon) return nullptr;
    std::shared_ptr<Pixmap> pm(PixmapFromIcon(fi.hIcon));
    DestroyIcon(fi.hIcon);
    return pm;
}

std::wstring AlExeOfWindow(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return L"";
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    if (!QueryFullProcessImageNameW(proc, 0, path, &size)) path[0] = 0;
    CloseHandle(proc);
    return path;
}

// ---- Do Not Disturb: WNF (undocumented, read-only) ----
typedef LONG AlNtStatus;
typedef AlNtStatus(NTAPI* AlWnfCallback)(ULONG64, ULONG, void*, void*, const void*, ULONG);
typedef AlNtStatus(NTAPI* AlWnfSubscribe)(void**, ULONG64, ULONG, AlWnfCallback, void*, const void*, ULONG, ULONG);
typedef AlNtStatus(NTAPI* AlWnfUnsubscribe)(void*);
typedef AlNtStatus(NTAPI* AlWnfQuery)(const ULONG64*, const void*, const void*, ULONG*, void*, ULONG*);
constexpr ULONG64 kAlWnfQuietHours = 0xD83063EA3BF1C75ULL;
void* g_alWnfSub = nullptr;
std::atomic<bool> g_alDndFirst{true};
class AlertsModule;
AlertsModule* g_alMod = nullptr;
AlNtStatus NTAPI AlWnfProc(ULONG64 name, ULONG, void*, void*, const void* buf, ULONG size);

// ---- Privacy: ConsentStore ----
std::wstring AlCleanApp(const wchar_t* sub) {
    std::wstring n = sub;
    size_t p = n.rfind(L'#');
    if (p != std::wstring::npos) n = n.substr(p + 1);
    p = n.rfind(L'\\');
    if (p != std::wstring::npos) n = n.substr(p + 1);
    if (n.size() > 4 && !_wcsicmp(n.c_str() + n.size() - 4, L".exe")) n.resize(n.size() - 4);
    p = n.find(L'_');
    if (p != std::wstring::npos && p > 0 && n.find(L'.') != std::wstring::npos) n.resize(p);
    return n;
}

bool AlKeyInUse(HKEY key, std::wstring* app, const wchar_t* name, int depth) {
    DWORD type = 0, size = 8;
    ULONGLONG start = 0, stop = 0;
    DWORD s1 = 8, s2 = 8, t1 = 0, t2 = 0;
    const bool a = RegQueryValueExW(key, L"LastUsedTimeStart", nullptr, &t1, reinterpret_cast<BYTE*>(&start), &s1) == ERROR_SUCCESS && t1 == REG_QWORD && s1 == 8;
    const bool b = RegQueryValueExW(key, L"LastUsedTimeStop", nullptr, &t2, reinterpret_cast<BYTE*>(&stop), &s2) == ERROR_SUCCESS && t2 == REG_QWORD && s2 == 8;
    (void)type; (void)size;
    if (a && b && stop == 0 && start != 0) {
        if (app && name) *app = AlCleanApp(name);
        return true;
    }
    if (depth <= 0) return false;
    wchar_t sub[512];
    for (DWORD i = 0;; i++) {
        DWORD len = 512;
        if (RegEnumKeyExW(key, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY k;
        if (RegOpenKeyExW(key, sub, 0, KEY_READ | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
            const bool r = AlKeyInUse(k, app, sub, depth - 1);
            RegCloseKey(k);
            if (r) return true;
        }
    }
    return false;
}

bool AlCapInUse(const wchar_t* cap, std::wstring* app) {
    wchar_t path[256];
    swprintf_s(path, L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\%s", cap);
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        HKEY k;
        if (RegOpenKeyExW(root, path, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) continue;
        const bool r = AlKeyInUse(k, app, nullptr, 2);
        RegCloseKey(k);
        if (r) return true;
    }
    return false;
}

struct AlBtInfo { std::wstring name; int battery = -1; bool le = false; };
struct AlBtJob { std::wstring id; uint64_t addr; bool le; int tries; ULONGLONG next; };

class AlertsModule : public IModule {
public:
    const wchar_t* Id() const override { return L"alerts"; }
    void Start(Services& s) override;
    void Stop() override;
    Size2 Measure(View v) override;
    void Draw(const DrawCtx& c) override;
    bool OnPointer(const PointerEvent& e, View v) override;
    void OnClickAlert() override;
    // The slider's fill runs on its own spring; the frame clock runs only while it moves.
    bool Animating(View v) const override { return v == View::Alert && IsLevel() && !fill_.Settled(); }
    void OnSettings(const Settings&) override { pctLayout_.Reset(); }  // The font may have changed.
    void OnVisibility(View v, bool shown) override {
        if (v != View::Alert) return;
        visible_ = shown;
        if (!shown) hovered_ = false;
    }

    // ---- UI thread entry points ----
    void Enqueue(AItem it);
    void UpdateBtBattery(const std::wstring& id, int pct);
    void OnDnd(int value);
    void OnListenerOk(bool ok) { listenerOk_ = ok; UpdateToastFlag(); }
    void UpdateToastFlag();
    void PublishGeometry();
    void FullscreenChanged();
    int Dots() const { return (mic_ ? 1 : 0) + (cam_ ? 1 : 0); }
    // A slider, or the short "Sent to phone" that follows closing the shelf under the pointer.
    bool HoldsHover() const { return IsLevel() || (hasCur_ && cur_.kind == AK::Phone); }
    void DrawOverlay(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, float L, float R, float B, float s);
    LRESULT Wnd(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    void Present();
    void Next();
    void Tick();
    void Finish() { hasCur_ = false; Next(); }
    void OnCaps(bool on);
    void OnPower();
    void OnClipboard();
    void ReadClipboardNow();
    void OnDevice(WPARAM wp, LPARAM lp);
    void ReadPrivacy();
    void OsdShown(bool fromHook);
    void OsdHidden();
    void OnLevels(WPARAM wp, LPARAM lp);
    bool IsLevel() const { return hasCur_ && (cur_.kind == AK::Volume || cur_.kind == AK::Brightness); }
    void SetLevelAt(float x);
    void AimFill(int level);
    void Launch(const wchar_t* uri) { ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL); }

    Services* sv_ = nullptr;
    HWND hwnd_ = nullptr;
    AItem cur_;
    bool hasCur_ = false;
    std::deque<AItem> pending_;
    int moreCount_ = 0;
    bool visible_ = false, hovered_ = false;
    bool listenerOk_ = false;
    int batPct_ = -1, batAc_ = -1, dndValue_ = 0;
    int clipTries_ = 0;
    bool osdSeen_ = false;
    // Own volume and brightness slider: last readings, to tell which one a key press changed.
    int lastVol_ = -1, lastBright_ = -1;
    bool lastMute_ = false, levelsPrimed_ = false, dragging_ = false;
    AK lastLevelKind_ = AK::Volume;
    float trackL_ = 0, trackR_ = 0;  // Slider track, pixels from the card's left edge.
    int levelFrom_ = -1;             // The reading before the one being shown, for the fill to start from.
    Spring fill_;                    // Fill fraction as drawn: springs to key presses, follows a drag at once.
    double fillT_ = 0;               // QPC time of the last fill step.
    Com<IDWriteTextLayout> pctLayout_;  // The percent, laid out once per value at the hover size.
    int pctLevel_ = -1;
    float pctSize_ = 0, pctScale_ = 0;
    bool mic_ = false, cam_ = false;
    std::wstring micApp_, camApp_;
    HPOWERNOTIFY powerA_ = nullptr, powerB_ = nullptr;
    HWINEVENTHOOK osdHook_ = nullptr;
    std::vector<HANDLE> threads_;
    RECT lastGeo_{};
    ULONGLONG lastCaps_ = 0;
    // Bluetooth worker state.
    std::mutex btLock_;
    std::map<std::wstring, AlBtInfo> btInfo_;
    std::vector<AlBtJob> btJobs_;
    std::map<std::wstring, ULONGLONG> btRecent_;
    friend DWORD WINAPI AlBtThread(void*);
    friend DWORD WINAPI AlNotifThread(void*);
    friend DWORD WINAPI AlCapsThread(void*);
    friend DWORD WINAPI AlPrivacyThread(void*);
    friend void CALLBACK AlOsdEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    friend AlNtStatus NTAPI AlWnfProc(ULONG64, ULONG, void*, void*, const void*, ULONG);
};

// ---- Queue ----

void AlertsModule::Present() {
    hovered_ = false;
    if (cur_.remaining <= 0) cur_.remaining = cur_.ms;
    sv_->ShowAlert(this, AlPriority(cur_.kind), 0);
    if (hwnd_) SetTimer(hwnd_, kAlTick, 100, nullptr);
}

void AlertsModule::Next() {
    if (!pending_.empty()) {
        cur_ = std::move(pending_.front());
        pending_.pop_front();
        hasCur_ = true;
        Present();
        return;
    }
    if (moreCount_ > 0) {  // The notifications that did not fit: one line, tap opens the bell.
        AItem it;
        it.kind = AK::Notification;
        it.more = true;
        it.title = AlFmt(AlT(L"alert.more", L"+%d more notifications"), moreCount_);
        it.sub = AlT(L"alert.more.sub", L"Tap to open the notification list");
        it.ms = 5000;
        moreCount_ = 0;
        cur_ = it;
        hasCur_ = true;
        Present();
        return;
    }
    hasCur_ = false;
    if (hwnd_) KillTimer(hwnd_, kAlTick);
    sv_->ClearAlert(this);
}

void AlertsModule::Enqueue(AItem it) {
    if (g_fullscreen || !sv_) return;  // The bar is hidden; toasts are back, nothing to show later.
    it.remaining = it.ms;
    switch (it.kind) {
        case AK::Volume:
        case AK::Brightness:
            if (hasCur_ && cur_.kind == it.kind) {  // Same slider: new value, full time again, no re-present.
                cur_.level = it.level;
                cur_.on = it.on;
                cur_.remaining = it.ms;
                AimFill(it.level);
                IslandRecompute();
                return;
            }
            if (hasCur_ && cur_.kind != AK::Clipboard && !IsLevel()) pending_.push_front(cur_);
            cur_ = it;
            hasCur_ = true;
            // A new card starts from the previous reading and springs to the new one.
            fill_.Snap(std::clamp((levelFrom_ >= 0 ? levelFrom_ : it.level) / 100.0f, 0.0f, 1.0f));
            AimFill(it.level);
            Present();
            return;
        case AK::Caps:
            if (hasCur_ && cur_.kind == AK::Caps) { cur_ = it; Present(); return; }
            // The interrupted alert resumes with the time it had left. A clipboard alert is just dropped.
            if (hasCur_ && cur_.kind != AK::Clipboard) pending_.push_front(cur_);
            cur_ = it;
            hasCur_ = true;
            Present();
            return;
        case AK::Clipboard:
            if (hasCur_ || !pending_.empty()) return;
            break;
        case AK::Notification: {
            if (hasCur_ && cur_.kind == AK::Clipboard) { cur_ = it; Present(); return; }
            int n = 0;
            for (const AItem& p : pending_) if (p.kind == AK::Notification) n++;
            if (n >= 5) { moreCount_++; return; }  // Never dropped silently: counted, then shown as one line.
            pending_.push_back(it);
            if (!hasCur_) Next();
            return;
        }
        default: {
            if (hasCur_ && cur_.kind == it.kind && !cur_.more) { cur_ = it; Present(); return; }
            for (AItem& p : pending_) if (p.kind == it.kind) { p = it; return; }
            pending_.push_back(it);
            if (!hasCur_) Next();
            return;
        }
    }
    cur_ = it;
    hasCur_ = true;
    Present();
}

void AlertsModule::Tick() {
    if (!hasCur_) { KillTimer(hwnd_, kAlTick); return; }
    // Waiting, reading, or under the volume popup. The phone confirmation runs out under the pointer too.
    if (!visible_ || (hovered_ && cur_.kind != AK::Phone) || g_covered) return;
    cur_.remaining -= 100;
    if (cur_.remaining <= 0) Finish();
}

void AlertsModule::UpdateBtBattery(const std::wstring& id, int pct) {
    auto apply = [&](AItem& a) {
        if (a.kind != AK::Bluetooth || a.key != id || pct < 0) return false;
        a.level = pct;
        a.sub = AlFmt(AlT(L"alert.bt.connected.bat", L"Connected, battery %d%%"), pct);
        return true;
    };
    if (hasCur_ && apply(cur_)) { sv_->Invalidate(); return; }
    for (AItem& p : pending_) if (apply(p)) return;
}

// ---- Sources ----

void AlertsModule::OnCaps(bool on) {
    const ULONGLONG now = GetTickCount64();
    if (now - lastCaps_ < 80) return;
    lastCaps_ = now;
    AItem it;
    it.kind = AK::Caps;
    it.on = on;
    it.title = on ? AlT(L"alert.caps.on", L"Caps Lock on") : AlT(L"alert.caps.off", L"Caps Lock off");
    it.sub = on ? AlT(L"alert.caps.tap", L"Tap to turn off") : L"";
    it.hint = on ? L"" : L"";
    it.danger = on;
    it.ms = 2500;
    Enqueue(it);
}

void AlertsModule::OnPower() {
    SYSTEM_POWER_STATUS st{};
    if (!GetSystemPowerStatus(&st) || st.BatteryFlag == 128 || st.BatteryLifePercent > 100) return;
    const int pct = st.BatteryLifePercent;
    const int ac = st.ACLineStatus == 1 ? 1 : 0;
    if (batPct_ < 0) { batPct_ = pct; batAc_ = ac; return; }  // The first read is a baseline, not an event.
    AItem it;
    it.kind = AK::Battery;
    it.level = pct;
    it.charging = ac != 0;
    it.ms = 4000;
    std::wstring left;
    if (!ac && st.BatteryLifeTime != (DWORD)-1) {
        const DWORD m = st.BatteryLifeTime / 60;
        left = AlFmt(L", %uh %02um", m / 60, m % 60);
    }
    bool fire = false;
    if (ac != batAc_) {
        fire = true;
        it.title = ac ? AlT(L"alert.power.on", L"Power connected") : AlT(L"alert.power.off", L"On battery power");
        it.sub = AlFmt(L"%d%%", pct) + left;
        it.good = ac != 0;
    } else if (!ac && ((batPct_ > 20 && pct <= 20) || (batPct_ > 10 && pct <= 10))) {  // Crossed, not equal: sleep can skip a value.
        fire = true;
        it.title = pct <= 10 ? AlT(L"alert.bat.crit", L"Battery critical") : AlT(L"alert.bat.low", L"Battery low");
        it.sub = AlFmt(L"%d%%", pct) + left;
        it.danger = true;
        it.ms = 6000;
    }
    batPct_ = pct;
    batAc_ = ac;
    if (fire) Enqueue(it);
}

void AlertsModule::OnDnd(int value) {
    if (g_alDndFirst.exchange(false)) { dndValue_ = value; return; }  // The first callback only reports the current state.
    if (value == dndValue_) return;
    dndValue_ = value;
    if (g_fullscreen) return;  // Automatic rules (games, fullscreen) flip it; not worth an alert.
    AItem it;
    it.kind = AK::Dnd;
    it.on = value != 0;
    it.title = value ? AlT(L"alert.dnd.on", L"Do Not Disturb on") : AlT(L"alert.dnd.off", L"Do Not Disturb off");
    it.sub = value ? AlT(L"alert.dnd.tapoff", L"Tap to turn off") : L"";
    it.ms = 3000;
    Enqueue(it);
}

void AlertsModule::OnDevice(WPARAM wp, LPARAM lp) {
    if ((wp != DBT_DEVICEARRIVAL && wp != DBT_DEVICEREMOVECOMPLETE) || !lp) return;
    const auto* hdr = reinterpret_cast<const DEV_BROADCAST_HDR*>(lp);
    const bool arrive = wp == DBT_DEVICEARRIVAL;
    AItem it;
    it.kind = AK::Device;
    it.ms = 3000;
    it.good = arrive;
    it.hint = AlT(L"alert.device.tap", L"Tap to open USB settings");
    if (hdr->dbch_devicetype == DBT_DEVTYP_VOLUME) {
        const auto* vol = reinterpret_cast<const DEV_BROADCAST_VOLUME*>(lp);
        if (vol->dbcv_flags & (DBTF_NET | DBTF_MEDIA)) return;  // Network drives and discs are not USB.
        wchar_t letter = 0;
        for (int i = 0; i < 26; i++) if (vol->dbcv_unitmask & (1u << i)) { letter = L'A' + i; break; }
        if (!letter) return;
        std::wstring label;
        if (arrive) {
            wchar_t root[4] = {letter, L':', L'\\', 0};
            wchar_t name[64] = {};
            if (GetVolumeInformationW(root, name, 64, nullptr, nullptr, nullptr, nullptr, 0)) label = name;
        }
        it.title = arrive ? AlT(L"alert.drive.in", L"Drive connected") : AlT(L"alert.drive.out", L"Drive removed");
        it.sub = label.empty() ? AlFmt(L"%c:", letter) : AlFmt(L"%s (%c:)", label.c_str(), letter);
    } else if (hdr->dbch_devicetype == DBT_DEVTYP_PORT) {
        const auto* port = reinterpret_cast<const DEV_BROADCAST_PORT_W*>(lp);
        it.title = arrive ? AlT(L"alert.port.in", L"Serial port connected") : AlT(L"alert.port.out", L"Serial port removed");
        it.sub = port->dbcp_name;
    } else {
        return;
    }
    Enqueue(it);
}

void AlertsModule::ReadClipboardNow() {
    if (!OpenClipboard(hwnd_)) {
        if (++clipTries_ < 6) SetTimer(hwnd_, kAlClipboard, 40, nullptr);  // The owner may still hold it.
        return;
    }
    clipTries_ = 0;
    AItem it;
    it.kind = AK::Clipboard;
    it.ms = 2500;
    it.hint = AlT(L"alert.clip.tap", L"Tap to open clipboard history");
    // Password managers mark their data so history and monitors skip it: show no content then.
    bool priv = false;
    static const UINT fmtExclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    static const UINT fmtHistory = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    if (IsClipboardFormatAvailable(fmtExclude)) priv = true;
    if (IsClipboardFormatAvailable(fmtHistory)) {
        if (HANDLE h = GetClipboardData(fmtHistory)) {
            if (const DWORD* p = static_cast<const DWORD*>(GlobalLock(h))) {
                if (GlobalSize(h) >= sizeof(DWORD) && *p == 0) priv = true;
                GlobalUnlock(h);
            }
        }
    }
    std::wstring text;
    int files = 0;
    bool image = false;
    if (!priv) {
        if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                    const size_t max = GlobalSize(h) / sizeof(wchar_t);
                    for (size_t i = 0; i < max && p[i] && text.size() < 96; i++) text += (p[i] == L'\r' || p[i] == L'\n' || p[i] == L'\t') ? L' ' : p[i];
                    GlobalUnlock(h);
                }
            }
        } else if (IsClipboardFormatAvailable(CF_HDROP)) {
            if (HANDLE h = GetClipboardData(CF_HDROP)) files = (int)DragQueryFileW(static_cast<HDROP>(h), 0xFFFFFFFF, nullptr, 0);
        } else if (IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_BITMAP)) {
            image = true;
        }
    }
    HWND owner = GetClipboardOwner();
    CloseClipboard();
    it.title = AlT(L"alert.clip", L"Copied");
    if (!priv) {
        if (!text.empty()) it.sub = text;
        else if (files) it.sub = AlFmt(AlT(L"alert.clip.files", L"%d files"), files);
        else if (image) it.sub = AlT(L"alert.clip.image", L"Image");
    }
    if (!owner) owner = GetForegroundWindow();
    if (owner) {
        const std::wstring exe = AlExeOfWindow(owner);
        const wchar_t* base = wcsrchr(exe.c_str(), L'\\');
        base = base ? base + 1 : exe.c_str();
        if (!exe.empty() && _wcsicmp(base, L"explorer.exe") && !wcsstr(base, L"indhawk")) it.icon = AlIconOfExe(exe.c_str());
    }
    Enqueue(it);
}

void AlertsModule::OnClipboard() {
    clipTries_ = 0;
    SetTimer(hwnd_, kAlClipboard, 100, nullptr);  // Apps fire several updates per copy: coalesce.
}

void AlertsModule::ReadPrivacy() {
    std::wstring m, c;
    const bool mic = AlCapInUse(L"microphone", &m);
    const bool cam = AlCapInUse(L"webcam", &c);
    if (mic == mic_ && cam == cam_) return;
    mic_ = mic;
    cam_ = cam;
    micApp_ = m;
    camApp_ = c;
    IslandRecompute();  // The pill makes room for the dots.
}

void AlertsModule::OsdShown(bool fromHook) {
    if (!fromHook && osdSeen_) return;
    if (fromHook) osdSeen_ = true;
    SetCovered(true);
    SetTimer(hwnd_, kAlOsd, 3500, nullptr);  // Fallback: the popup's hide is not always an event.
}

void AlertsModule::OsdHidden() {
    KillTimer(hwnd_, kAlOsd);
    SetCovered(false);
}

void AlertsModule::OnLevels(WPARAM wp, LPARAM lp) {
    const int vol = (int)(short)(wp & 0xFFFF);
    const bool mute = (wp & 0x10000) != 0;
    const int bright = (int)lp;
    if (!levelsPrimed_) {  // The reading taken at start: a baseline, nothing to show.
        levelsPrimed_ = true;
        lastVol_ = vol; lastMute_ = mute; lastBright_ = bright;
        return;
    }
    AK kind = lastLevelKind_;
    if (vol >= 0 && (vol != lastVol_ || mute != lastMute_)) kind = AK::Volume;
    else if (bright >= 0 && bright != lastBright_) kind = AK::Brightness;
    levelFrom_ = kind == AK::Volume ? lastVol_ : lastBright_;
    lastVol_ = vol; lastMute_ = mute; lastBright_ = bright;
    if (dragging_) return;  // The card already shows what the drag set.
    lastLevelKind_ = kind;
    AItem it;
    it.kind = kind;
    it.level = kind == AK::Volume ? vol : bright;
    it.on = kind == AK::Volume && mute;
    it.ms = 2000;
    if (it.level < 0) return;
    Enqueue(it);
}

void AlertsModule::AimFill(int level) {
    const float to = std::clamp(level / 100.0f, 0.0f, 1.0f);
    if (fill_.Settled() && fabsf(fill_.x - to) < fill_.eps) return;
    if (fill_.Settled()) fillT_ = QpcSeconds();  // Moving again after a rest: no stale step.
    fill_.Aim(to, 300.0f, 34.0f);                // About critically damped, settles in some 250 ms.
    if (sv_) sv_->WakeAnimation();
}

void AlertsModule::SetLevelAt(float x) {
    if (!IsLevel() || trackR_ <= trackL_) return;
    const float frac = std::clamp((x - trackL_) / (trackR_ - trackL_), 0.0f, 1.0f);
    fill_.Snap(frac);  // A drag is followed exactly, between whole percents too.
    const int v = (int)std::lround(frac * 100.0f);
    if (v == cur_.level) { sv_->Invalidate(); return; }
    cur_.level = v;
    cur_.remaining = cur_.ms;
    if (cur_.kind == AK::Volume) { g_alSetVolume = v; lastVol_ = v; cur_.on = false; }
    else { g_alSetBright = v; lastBright_ = v; }
    if (g_alLevelSet) SetEvent(g_alLevelSet);
    IslandRecompute();
}

// ---- Island-facing parts ----

Size2 AlertsModule::Measure(View v) {
    if (v != View::Alert || !hasCur_) return {40.0f, kNotchH};
    if (IsLevel()) return hovered_ ? Size2{kAlLevelHoverW + 2.0f * AlertsDotsReserve(), kAlLevelHoverH}  // Bigger to drag.
                                   : Size2{kAlLevelW + 2.0f * AlertsDotsReserve(), kAlLevelH};
    float w = 300.0f;
    switch (cur_.kind) {
        case AK::Caps: w = 210.0f; break;
        case AK::Notification: w = 360.0f; break;
        case AK::Clipboard: w = 340.0f; break;
        case AK::Device: w = 300.0f; break;
        case AK::Phone: w = 250.0f; break;
        default: break;
    }
    return {w + 2.0f * AlertsDotsReserve(), hovered_ && cur_.kind != AK::Phone ? 66.0f : 44.0f};
}

bool AlertsModule::OnPointer(const PointerEvent& e, View v) {
    if (v != View::Alert) return false;
    if (IsLevel()) {  // Press or drag on the card sets the level; hovering holds it open and grows it.
        if (e.kind == PointerEvent::Down && e.left) { dragging_ = true; SetLevelAt(e.pt.x); }
        else if (e.kind == PointerEvent::Move && dragging_) SetLevelAt(e.pt.x);
        else if (e.kind == PointerEvent::Up) dragging_ = false;
        else if (e.kind == PointerEvent::Leave) {
            dragging_ = false;
            if (hovered_) {
                hovered_ = false;
                cur_.remaining = cur_.ms;  // The dismiss timer starts over from leaving.
                IslandRecompute();
            }
        }
        if (e.kind == PointerEvent::Move && !hovered_) { hovered_ = true; IslandRecompute(); }
        return true;
    }
    if (e.kind == PointerEvent::Move && !hovered_) { hovered_ = true; IslandRecompute(); }
    else if (e.kind == PointerEvent::Leave && hovered_) { hovered_ = false; IslandRecompute(); }
    return false;
}

void AlertsModule::Draw(const DrawCtx& c) {
    // Once finished, the card is still drawn while the island fades it out, so it leaves with the body.
    if (c.view != View::Alert || (!hasCur_ && c.alpha >= 0.999f)) return;
    const float s = c.scale;
    const D2D1_RECT_F r = c.rect;
    const float badge = 28.0f * s, bx = r.left + 10.0f * s, by = r.top + 8.0f * s;
    auto col = [&](D2D1_COLOR_F k, float a = 1.0f) { k.a *= a * c.alpha; c.brush->SetColor(k); };

    if (cur_.kind == AK::Volume || cur_.kind == AK::Brightness) {  // Glyph, track with fill, percent.
        const bool vol = cur_.kind == AK::Volume;
        // Lay out in the body as it stands now, so the hover growth rides the island's size springs:
        // g goes 0 to 1 as the body grows from the normal card to the hover card.
        const float ew = std::min(r.right - r.left, std::max(g_isW.x, 1.0f));
        const float eh = std::min(r.bottom - r.top, std::max(g_isH.x, 1.0f));
        const float g = Clamp01((eh / s - kAlLevelH) / (kAlLevelHoverH - kAlLevelH));
        const float left = (r.left + r.right) / 2.0f - ew / 2.0f, right = left + ew;
        const float gs = s * (1.0f + 0.4f * g);  // Glyph scale.
        const float lw = 1.4f * gs;
        const float cx = left + (24.0f + 6.0f * g) * s, cy = r.top + eh / 2.0f;
        col(kFg);
        if (vol) {
            D2D1_POINT_2F p[] = {{cx - 8 * gs, cy - 3 * gs}, {cx - 4 * gs, cy - 3 * gs}, {cx + 1 * gs, cy - 7 * gs}, {cx + 1 * gs, cy + 7 * gs}, {cx - 4 * gs, cy + 3 * gs}, {cx - 8 * gs, cy + 3 * gs}};
            for (int i = 0; i < 6; i++) c.rt->DrawLine(p[i], p[(i + 1) % 6], c.brush, lw);
            if (cur_.on) {  // Muted: a cross instead of waves.
                c.rt->DrawLine(D2D1::Point2F(cx + 4 * gs, cy - 3 * gs), D2D1::Point2F(cx + 10 * gs, cy + 3 * gs), c.brush, lw);
                c.rt->DrawLine(D2D1::Point2F(cx + 10 * gs, cy - 3 * gs), D2D1::Point2F(cx + 4 * gs, cy + 3 * gs), c.brush, lw);
            } else if (cur_.level > 0) {
                c.rt->DrawLine(D2D1::Point2F(cx + 5 * gs, cy - 3 * gs), D2D1::Point2F(cx + 5 * gs, cy + 3 * gs), c.brush, lw);
                if (cur_.level > 50) c.rt->DrawLine(D2D1::Point2F(cx + 9 * gs, cy - 6 * gs), D2D1::Point2F(cx + 9 * gs, cy + 6 * gs), c.brush, lw);
            }
        } else {
            c.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 3.5f * gs, 3.5f * gs), c.brush, lw);
            for (int i = 0; i < 8; i++) {
                const float a = i * 3.14159265f / 4.0f, ca = std::cos(a), sa = std::sin(a);
                c.rt->DrawLine(D2D1::Point2F(cx + ca * 6.0f * gs, cy + sa * 6.0f * gs), D2D1::Point2F(cx + ca * 8.5f * gs, cy + sa * 8.5f * gs), c.brush, lw);
            }
        }
        const float tl = left + (46.0f + 14.0f * g) * s, th = (6.0f + 6.0f * g) * s;
        const float tr = std::max(tl, right - (52.0f + 8.0f * g + 12.0f * Dots()) * s);  // Never inverted while the body is narrow.
        trackL_ = tl - r.left;  // Pointer x is measured from the view's left edge, r.left.
        trackR_ = tr - r.left;
        if (!fill_.Settled()) {  // Stepped by real time, here, so it moves at the display's rate.
            double dt = c.now - fillT_;
            if (dt > 0.05) dt = 0.05;
            if (dt > 0) fill_.Step(dt);
        }
        fillT_ = c.now;
        const float frac = std::clamp(fill_.x, 0.0f, 1.0f);
        col(Rgb(255, 255, 255, 0x2e));
        c.rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tl, cy - th / 2, tr, cy + th / 2), th / 2, th / 2), c.brush);
        if (frac > 0) {
            col(kFg, cur_.on ? 0.4f : 1.0f);
            c.rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tl, cy - th / 2, tl + std::max(th, (tr - tl) * frac), cy + th / 2), th / 2, th / 2), c.brush);
        }
        // The percent is laid out once per value at the hover size and scaled down: no new layout per frame.
        const float big = g_text.DefaultSize() * 1.3f;
        if (!pctLayout_ || pctLevel_ != cur_.level || pctSize_ != big || pctScale_ != s) {
            TextOpts o;
            o.maxW = 48.0f * s;
            o.maxH = 26.0f * s;
            o.size = big;
            pctLayout_ = g_text.Layout(std::to_wstring(cur_.level), o);
            pctLevel_ = cur_.level;
            pctSize_ = big;
            pctScale_ = s;
        }
        if (pctLayout_) {
            const float k = (1.0f + 0.3f * g) / 1.3f, x0 = tr + 10.0f * s;
            D2D1_MATRIX_3X2_F old;
            c.rt->GetTransform(&old);
            c.rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k, D2D1::Point2F(x0, cy)) * old);
            col(kFg, 0.8f);
            c.rt->DrawTextLayout(D2D1::Point2F(x0, cy - 13.0f * s), pctLayout_.Get(), c.brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
            c.rt->SetTransform(old);
        }
        return;
    }

    // Badge: app icon, or a glyph for the kind.
    const D2D1_RECT_F br = D2D1::RectF(bx, by, bx + badge, by + badge);
    if (cur_.icon && cur_.icon->w > 0) {
        if (ID2D1Bitmap* bmp = AlBitmap(c.rt, *cur_.icon)) c.rt->DrawBitmap(bmp, br, c.alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        col(Rgb(255, 255, 255, 0x1e));
        c.rt->FillRoundedRectangle(D2D1::RoundedRect(br, 8.0f * s, 8.0f * s), c.brush);
        col(kFg);
        const float cx = bx + badge / 2, cy = by + badge / 2;
        TextOpts o;
        o.center = true;
        o.maxW = badge;
        o.maxH = badge;
        switch (cur_.kind) {
            case AK::Battery: {
                const int p = cur_.level;
                g_icons.Draw(c.rt, c.brush, p < 10 ? I_batwarn : p < 30 ? I_batlow : p < 70 ? I_batmed : p < 95 ? I_bathigh : I_batfull, cx, cy, 16.0f * s);
                if (cur_.charging) g_icons.Draw(c.rt, c.brush, I_bolt, cx + 8.0f * s, cy - 6.0f * s, 9.0f * s);
                break;
            }
            case AK::Bluetooth: g_icons.Draw(c.rt, c.brush, I_bluetooth, cx, cy, 16.0f * s); break;
            case AK::Notification: g_icons.Draw(c.rt, c.brush, I_bell, cx, cy, 16.0f * s); break;
            case AK::Caps: o.size = 15.0f * s; g_text.Draw(c.rt, L"A", bx, by, c.brush, o); break;
            case AK::Dnd: o.size = 15.0f * s; g_text.Draw(c.rt, L"☾", bx, by, c.brush, o); break;
            case AK::Device: o.size = 9.0f * s; g_text.Draw(c.rt, wcsstr(cur_.title.c_str(), L"erial") ? L"COM" : L"USB", bx, by, c.brush, o); break;
            case AK::Phone:
                c.rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 5.0f * s, cy - 8.0f * s, cx + 5.0f * s, cy + 8.0f * s), 2.5f * s, 2.5f * s), c.brush, 1.4f * s);
                c.rt->DrawLine(D2D1::Point2F(cx - 1.5f * s, cy + 5.0f * s), D2D1::Point2F(cx + 1.5f * s, cy + 5.0f * s), c.brush, 1.2f * s);
                break;
            case AK::Volume:
            case AK::Brightness:
                break;
            case AK::Clipboard:
                c.rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 6.0f * s, cy - 7.0f * s, cx + 6.0f * s, cy + 8.0f * s), 2.0f * s, 2.0f * s), c.brush, 1.4f * s);
                c.rt->DrawLine(D2D1::Point2F(cx - 3.0f * s, cy), D2D1::Point2F(cx + 3.0f * s, cy), c.brush, 1.2f * s);
                c.rt->DrawLine(D2D1::Point2F(cx - 3.0f * s, cy + 3.5f * s), D2D1::Point2F(cx + 3.0f * s, cy + 3.5f * s), c.brush, 1.2f * s);
                break;
        }
    }
    if (cur_.danger || cur_.good) {  // Status dot: the only colour in the cards.
        col(cur_.danger ? Rgb(255, 69, 58) : Rgb(48, 209, 88));
        c.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(bx + badge - 1.0f * s, by + badge - 1.0f * s), 4.0f * s, 4.0f * s), c.brush);
    }

    const float tx = bx + badge + 10.0f * s;
    const float reserve = (12.0f + 12.0f * Dots()) * s;
    TextOpts o;
    o.ellipsis = true;
    o.maxW = std::max(10.0f, r.right - reserve - tx);
    o.maxH = 18.0f * s;
    col(kFg);
    g_text.Draw(c.rt, cur_.title, tx, r.top + 6.0f * s, c.brush, o);
    if (!cur_.sub.empty()) {
        col(kFg, 0.62f);
        o.size = g_text.DefaultSize() * 0.9f;
        g_text.Draw(c.rt, cur_.sub, tx, r.top + 24.0f * s, c.brush, o);
    }
    if (hovered_ && !cur_.hint.empty()) {
        col(kFg, 0.45f);
        o.size = g_text.DefaultSize() * 0.85f;
        g_text.Draw(c.rt, cur_.hint, tx, r.top + 44.0f * s, c.brush, o);
    }
}

void AlertsModule::OnClickAlert() {
    if (!hasCur_ || IsLevel()) return;  // A slider is set by pressing on it, not dismissed.
    const AItem it = cur_;
    switch (it.kind) {
        case AK::Caps:
            if (it.on) {  // Turn it off. UIPI blocks this when an elevated window has focus: then nothing changes.
                INPUT in[2]{};
                in[0].type = in[1].type = INPUT_KEYBOARD;
                in[0].ki.wVk = in[1].ki.wVk = VK_CAPITAL;
                in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(2, in, sizeof(INPUT));
            }
            break;
        case AK::Battery: Launch(L"ms-settings:batterysaver"); break;
        case AK::Bluetooth: Launch(L"ms-settings:bluetooth"); break;
        case AK::Device: Launch(L"ms-settings:usb"); break;
        case AK::Dnd:  // Turn it off (levels.inc, AlDndOff); Settings opens only if that fails.
            if (!it.on) break;
            if (g_alDndOff) SetEvent(g_alDndOff);
            else Launch(L"ms-settings:quietmomentshome");
            break;
        case AK::Clipboard: SendWinKey('V'); break;
        case AK::Phone:
        case AK::Volume:
        case AK::Brightness:
            break;
        case AK::Notification:
            if (it.more) {
                RECT body = g_isBody;
                sv_->OpenPanel(PanelKind::Notifications, (body.left + body.right) / 2);
            } else {
                if (!it.key.empty()) Launch((L"shell:AppsFolder\\" + it.key).c_str());
                if (it.nid && g_alListener) {  // Tapped means handled: it leaves the bell list too.
                    const uint32_t id = it.nid;
                    try { g_alListener.RemoveNotification(id); } catch (...) {}
                }
            }
            break;
    }
    Finish();
}

void AlertsModule::DrawOverlay(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, float L, float R, float B, float s) {
    if (!Dots() || B > 55.0f * s) return;
    float x = R - 12.0f * s;
    const float y = B / 2.0f;
    auto dot = [&](D2D1_COLOR_F c) {
        brush->SetColor(c);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), 4.0f * s, 4.0f * s), brush);
        x -= 12.0f * s;
    };
    if (mic_) dot(Rgb(255, 159, 10));
    if (cam_) dot(Rgb(48, 209, 88));
}

void AlertsModule::UpdateToastFlag() {
    if (!g_hwnd) return;
    if (listenerOk_ && !g_fullscreen) {
        SetPropW(g_hwnd, L"TopBarToastsOk", reinterpret_cast<HANDLE>(1));
        SetPropW(g_hwnd, L"TopBarToastsTick", reinterpret_cast<HANDLE>((UINT_PTR)GetTickCount()));
    } else {
        RemovePropW(g_hwnd, L"TopBarToastsOk");
    }
}

void AlertsModule::FullscreenChanged() {
    if (g_fullscreen) {  // Toasts are back; whatever is queued would be stale when the bar returns.
        pending_.clear();
        moreCount_ = 0;
        hasCur_ = false;
        if (hwnd_) KillTimer(hwnd_, kAlTick);
        if (sv_) sv_->ClearAlert(this);
    }
    UpdateToastFlag();
}

// The collapsed notch's rectangle in screen pixels, for the popup hook. Widened to what a slider needs.
void AlertsModule::PublishGeometry() {
    if (!g_hwnd || g_monitor.right <= g_monitor.left) return;
    const float s = Scale();
    float w = 196.0f;
    if (IModule* pill = PillModule()) w = std::max(w, pill->Measure(View::Pill).w);
    const int cx = (g_monitor.left + g_monitor.right) / 2;
    const RECT r{(LONG)(cx - w * s / 2.0f), g_monitor.top, (LONG)(cx + w * s / 2.0f), g_monitor.top + (LONG)(kNotchH * s + 0.5f)};
    if (EqualRect(&r, &lastGeo_)) return;
    lastGeo_ = r;
    SetPropW(g_hwnd, L"TopBarCentreLeft", reinterpret_cast<HANDLE>((INT_PTR)r.left));
    SetPropW(g_hwnd, L"TopBarCentreTop", reinterpret_cast<HANDLE>((INT_PTR)r.top));
    SetPropW(g_hwnd, L"TopBarCentreRight", reinterpret_cast<HANDLE>((INT_PTR)r.right));
    SetPropW(g_hwnd, L"TopBarCentreBottom", reinterpret_cast<HANDLE>((INT_PTR)r.bottom));
    Wh_Log(L"alerts: popup rect published %ld,%ld-%ld,%ld (px)", r.left, r.top, r.right, r.bottom);
}

// ---- Workers ----

LRESULT CALLBACK AlCapsHook(int code, WPARAM wp, LPARAM lp) {
    // System-wide input path: post and return, nothing else.
    if (code == HC_ACTION && (wp == WM_KEYUP || wp == WM_SYSKEYUP)) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if (k->vkCode == VK_CAPITAL) {
            if (HWND h = g_alHwnd.load()) PostMessageW(h, WM_AL_CAPS, 0, (GetKeyState(VK_CAPITAL) & 1) ? 1 : 0);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

DWORD WINAPI AlCapsThread(void*) {
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, AlCapsHook, GetModuleHandleW(nullptr), 0);
    // A hook dies with its thread: pump until unload.
    for (;;) {
        if (MsgWaitForMultipleObjects(1, &g_alStop, FALSE, INFINITE, QS_ALLINPUT) != WAIT_OBJECT_0 + 1) break;
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
    }
    if (hook) UnhookWindowsHookEx(hook);
    return 0;
}

DWORD WINAPI AlPrivacyThread(void*) {
    static const wchar_t* kCaps[] = {L"microphone", L"webcam"};
    HKEY keys[4] = {};
    HANDLE ev[5] = {g_alStop};
    int n = 0;
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        for (const wchar_t* cap : kCaps) {
            wchar_t path[256];
            swprintf_s(path, L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\%s", cap);
            HKEY k;
            if (RegOpenKeyExW(root, path, 0, KEY_NOTIFY | KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) continue;
            HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (RegNotifyChangeKeyValue(k, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, e, TRUE) != ERROR_SUCCESS) { CloseHandle(e); RegCloseKey(k); continue; }
            keys[n] = k;
            ev[n + 1] = e;
            n++;
        }
    }
    if (HWND h = g_alHwnd.load()) PostMessageW(h, WM_AL_PRIVACY, 0, 0);
    while (n > 0) {
        const DWORD w = WaitForMultipleObjects(n + 1, ev, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0 || w == WAIT_FAILED) break;
        const int i = (int)(w - WAIT_OBJECT_0) - 1;
        if (HWND h = g_alHwnd.load()) PostMessageW(h, WM_AL_PRIVACY, 0, 0);
        if (i >= 0 && i < n) RegNotifyChangeKeyValue(keys[i], TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, ev[i + 1], TRUE);
    }
    for (int i = 0; i < n; i++) { RegCloseKey(keys[i]); CloseHandle(ev[i + 1]); }
    return 0;
}

AlNtStatus NTAPI AlWnfProc(ULONG64 name, ULONG, void*, void*, const void* buf, ULONG size) {
    if (name != kAlWnfQuietHours) return 0;
    const int v = (buf && size >= sizeof(int)) ? *static_cast<const int*>(buf) : 0;
    g_services.Post([v] { if (g_alMod) g_alMod->OnDnd(v); });
    return 0;
}

// Bluetooth battery: the devnode property Settings uses, then the GATT battery service.
int AlBtBatteryClassic(uint64_t addr) {
    if (!addr) return -1;
    wchar_t hex[16];
    swprintf_s(hex, L"%012llX", (unsigned long long)addr);
    std::wstring want = hex;
    std::transform(want.begin(), want.end(), want.begin(), ::towlower);
    HDEVINFO set = SetupDiGetClassDevsExW(nullptr, L"BTHENUM", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT, nullptr, nullptr, nullptr);
    if (set == INVALID_HANDLE_VALUE) return -1;
    static const DEVPROPKEY key = {{0x104ea319, 0x6ee2, 0x4701, {0xbd, 0x47, 0x8d, 0xdb, 0xf4, 0x25, 0xbb, 0xe5}}, 2};
    int result = -1;
    SP_DEVINFO_DATA d{};
    d.cbSize = sizeof(d);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); i++) {
        wchar_t inst[512] = {};
        if (!SetupDiGetDeviceInstanceIdW(set, &d, inst, 512, nullptr)) continue;
        std::wstring low = inst;
        std::transform(low.begin(), low.end(), low.begin(), ::towlower);
        if (low.find(want) == std::wstring::npos) continue;
        DEVPROPTYPE type = 0;
        BYTE b = 0;
        DWORD need = 0;
        if (SetupDiGetDevicePropertyW(set, &d, &key, &type, &b, sizeof(b), &need, 0) && type == DEVPROP_TYPE_BYTE && b != 0xFF) {
            result = std::min<int>(b, 100);
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return result;
}

int AlBtBatteryGatt(const winrt::hstring& id) {
    using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
    try {
        auto dev = wbt::BluetoothLEDevice::FromIdAsync(id).get();
        if (!dev) return -1;
        auto svc = dev.GetGattServicesForUuidAsync(GattServiceUuids::Battery(), wbt::BluetoothCacheMode::Uncached).get();
        if (svc.Status() != GattCommunicationStatus::Success || svc.Services().Size() == 0) return -1;
        auto ch = svc.Services().GetAt(0).GetCharacteristicsForUuidAsync(GattCharacteristicUuids::BatteryLevel(), wbt::BluetoothCacheMode::Uncached).get();
        if (ch.Status() != GattCommunicationStatus::Success || ch.Characteristics().Size() == 0) return -1;
        auto rd = ch.Characteristics().GetAt(0).ReadValueAsync(wbt::BluetoothCacheMode::Uncached).get();
        if (rd.Status() != GattCommunicationStatus::Success || rd.Value().Length() < 1) return -1;
        return std::min<int>(winrt::Windows::Storage::Streams::DataReader::FromBuffer(rd.Value()).ReadByte(), 100);
    } catch (...) {
        return -1;
    }
}

uint64_t AlBtAddress(const std::wstring& id) {
    const size_t p = id.rfind(L'-');
    if (p == std::wstring::npos || id.size() - p - 1 != 17) return 0;
    uint64_t a = 0;
    for (size_t i = p + 1; i < id.size(); i++) {
        if (id[i] == L':') continue;
        const wchar_t ch = id[i];
        const int v = ch >= L'0' && ch <= L'9' ? ch - L'0' : (ch | 0x20) >= L'a' && (ch | 0x20) <= L'f' ? (ch | 0x20) - L'a' + 10 : -1;
        if (v < 0) return 0;
        a = a * 16 + v;
    }
    return a;
}

DWORD WINAPI AlBtThread(void*) {
    AlertsModule& m = *g_alMod;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        auto make = [&](const winrt::hstring& selector, bool le) {
            auto watcher = wde::DeviceInformation::CreateWatcher(selector);
            auto ready = std::make_shared<std::atomic<bool>>(false);  // Devices listed before the enumeration ends are not events.
            watcher.EnumerationCompleted([ready](auto&&, auto&&) { *ready = true; });
            watcher.Added([&m, ready, le](wde::DeviceWatcher const&, wde::DeviceInformation const& info) {
                if (!*ready) {
                    std::lock_guard lock(m.btLock_);
                    m.btInfo_[info.Id().c_str()] = {info.Name().c_str(), -1, le};
                    return;
                }
                const std::wstring id = info.Id().c_str(), name = info.Name().c_str();
                if (name.empty()) return;
                const ULONGLONG now = GetTickCount64();
                {
                    std::lock_guard lock(m.btLock_);
                    ULONGLONG& last = m.btRecent_[name];
                    const bool dup = now - last < 1000;  // A dual-mode device shows on both watchers.
                    last = now;
                    m.btInfo_[id] = {name, -1, le};
                    if (dup) return;
                    m.btJobs_.push_back({id, AlBtAddress(id), le, 0, now + 300});
                }
                g_services.Post([id, name] {
                    AItem it;
                    it.kind = AK::Bluetooth;
                    it.key = id;
                    it.title = name;
                    it.sub = AlT(L"alert.bt.connected", L"Connected");
                    it.hint = AlT(L"alert.bt.tap", L"Tap to open Bluetooth settings");
                    it.good = true;
                    it.ms = 4000;
                    if (g_alMod) g_alMod->Enqueue(it);
                });
            });
            watcher.Removed([&m, ready](wde::DeviceWatcher const&, wde::DeviceInformationUpdate const& upd) {
                if (!*ready) return;
                const std::wstring id = upd.Id().c_str();
                AlBtInfo info;
                const ULONGLONG now = GetTickCount64();
                {
                    std::lock_guard lock(m.btLock_);
                    auto f = m.btInfo_.find(id);  // Removed carries only the id.
                    if (f == m.btInfo_.end() || f->second.name.empty()) return;
                    info = f->second;
                    ULONGLONG& last = m.btRecent_[info.name + L"\x01off"];
                    if (now - last < 1000) return;
                    last = now;
                    m.btJobs_.erase(std::remove_if(m.btJobs_.begin(), m.btJobs_.end(), [&](const AlBtJob& j) { return j.id == id; }), m.btJobs_.end());
                }
                g_services.Post([id, info] {
                    AItem it;
                    it.kind = AK::Bluetooth;
                    it.key = id + L"\x01off";
                    it.title = info.name;
                    it.sub = info.battery >= 0 ? AlFmt(AlT(L"alert.bt.disconnected.bat", L"Disconnected, last battery %d%%"), info.battery)
                                               : AlT(L"alert.bt.disconnected", L"Disconnected");
                    it.hint = AlT(L"alert.bt.tap", L"Tap to open Bluetooth settings");
                    it.level = info.battery;
                    it.ms = 4000;
                    if (g_alMod) g_alMod->Enqueue(it);
                });
            });
            watcher.Start();
            return watcher;
        };
        auto w1 = make(wbt::BluetoothDevice::GetDeviceSelectorFromConnectionStatus(wbt::BluetoothConnectionStatus::Connected), false);
        auto w2 = make(wbt::BluetoothLEDevice::GetDeviceSelectorFromConnectionStatus(wbt::BluetoothConnectionStatus::Connected), true);
        // One loop serves the battery retries (6 tries, 1.5 s apart: the level appears late) and waits on the stop event.
        while (WaitForSingleObject(g_alStop, 250) == WAIT_TIMEOUT) {
            AlBtJob job{};
            bool have = false;
            const ULONGLONG now = GetTickCount64();
            {
                std::lock_guard lock(m.btLock_);
                for (size_t i = 0; i < m.btJobs_.size(); i++) {
                    if (m.btJobs_[i].next <= now) { job = m.btJobs_[i]; m.btJobs_.erase(m.btJobs_.begin() + i); have = true; break; }
                }
            }
            if (!have) continue;
            int pct = AlBtBatteryClassic(job.addr);
            if (pct < 0 && job.le) pct = AlBtBatteryGatt(job.id.c_str());
            if (pct >= 0) {
                { std::lock_guard lock(m.btLock_); m.btInfo_[job.id].battery = pct; }
                const std::wstring id = job.id;
                g_services.Post([id, pct] { if (g_alMod) g_alMod->UpdateBtBattery(id, pct); });
            } else if (++job.tries < 6) {
                job.next = GetTickCount64() + 1500;
                std::lock_guard lock(m.btLock_);
                m.btJobs_.push_back(job);
            }
        }
        w1.Stop();
        w2.Stop();
    } catch (...) {
        Wh_Log(L"alerts: Bluetooth watcher failed");
    }
    return 0;
}

// Notifications: the listener is only read when a toast appears (panel-split posts a message), with a slow
// safety poll. Without that hook the safety poll runs every 5 s and shows what it finds, as the old mod did.
DWORD WINAPI AlNotifThread(void*) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        // Early init can bind to a dead proxy for good, so toasts stay native for the first 30 s.
        if (WaitForSingleObject(g_alStop, 30000) == WAIT_OBJECT_0) return 0;
        auto listener = wunm::UserNotificationListener::Current();
        bool granted = false;
        for (int tries = 0; !granted; tries++) {
            const auto st = listener.RequestAccessAsync().get();
            granted = st == wunm::UserNotificationListenerAccessStatus::Allowed;
            if (granted) break;
            if (tries >= 2) g_services.Post([] { if (g_alMod) g_alMod->OnListenerOk(false); });  // Toasts stay native.
            if (WaitForSingleObject(g_alStop, tries < 2 ? 3000 : 60000) == WAIT_OBJECT_0) return 0;
        }
        g_alListener = listener;
        g_services.Post([] { if (g_alMod) g_alMod->OnListenerOk(true); });
        std::set<uint32_t> seen;
        bool first = true;
        auto poll = [&](bool pop) {
            int found = 0;
            auto list = listener.GetNotificationsAsync(wun::NotificationKinds::Toast).get();
            std::set<uint32_t> current;
            for (uint32_t i = 0; i < list.Size(); i++) current.insert(list.GetAt(i).Id());
            if (first) { seen = current; first = false; return 0; }
            for (uint32_t i = 0; i < list.Size(); i++) {
                try {
                    auto un = list.GetAt(i);
                    const uint32_t id = un.Id();
                    if (seen.count(id)) continue;
                    seen.insert(id);
                    found++;
                    if (!pop) continue;
                    AItem it;
                    it.kind = AK::Notification;
                    it.nid = id;
                    it.ms = 7000;  // The toast is gone, so this is the reading time.
                    auto info = un.AppInfo();
                    auto disp = info.DisplayInfo();
                    std::wstring app = disp.DisplayName().c_str();
                    std::wstring lower = app;
                    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
                    if (lower.find(L"powershell") != std::wstring::npos || lower.find(L"pwsh") != std::wstring::npos ||
                        lower.find(L"cmd.exe") != std::wstring::npos || lower.find(L"conhost.exe") != std::wstring::npos) continue;
                    it.key = info.AppUserModelId().c_str();
                    try {
                        auto stream = disp.GetLogo({32.0f, 32.0f}).OpenReadAsync().get();
                        const uint32_t size = (uint32_t)stream.Size();
                        if (size > 0 && size < (4u << 20)) {
                            winrt::Windows::Storage::Streams::DataReader rd(stream);
                            rd.LoadAsync(size).get();
                            std::vector<uint8_t> bytes(size);
                            rd.ReadBytes(bytes);
                            it.icon = AlDecodeImage(bytes);
                        }
                    } catch (...) {}
                    auto binding = un.Notification().Visual().GetBinding(wun::KnownNotificationBindings::ToastGeneric());
                    if (binding) {
                        auto texts = binding.GetTextElements();
                        if (texts.Size() > 0) it.title = texts.GetAt(0).Text().c_str();
                        if (texts.Size() > 1) it.sub = texts.GetAt(1).Text().c_str();
                    }
                    if (it.title.empty()) it.title = app.empty() ? L"New notification" : app;
                    if (it.title.size() > 120) { it.title.resize(120); it.title += L"..."; }
                    if (it.sub.size() > 160) { it.sub.resize(160); it.sub += L"..."; }
                    it.hint = app.empty() ? AlT(L"alert.notif.tap", L"Tap to open") : AlFmt(AlT(L"alert.notif.tap.app", L"Tap to open %s"), app.c_str());
                    g_services.Post([it] { if (g_alMod) g_alMod->Enqueue(it); });
                } catch (...) {}
            }
            for (auto it = seen.begin(); it != seen.end();) it = current.count(*it) ? std::next(it) : seen.erase(it);
            return found;
        };
        try { poll(false); } catch (...) {}
        HANDLE h[2] = {g_alStop, g_alWake};
        for (;;) {
            const DWORD w = WaitForMultipleObjects(2, h, FALSE, g_alHookSeen ? 30000 : 5000);
            if (w == WAIT_OBJECT_0) break;
            try {
                if (w == WAIT_OBJECT_0 + 1) {
                    // The window can appear before the listener lists it: look again at 100, 300 and 800 ms.
                    for (DWORD d : {100u, 200u, 500u}) {
                        if (WaitForSingleObject(g_alStop, d) == WAIT_OBJECT_0) return 0;
                        if (poll(true) > 0) break;
                    }
                } else {
                    poll(!g_alHookSeen);
                }
            } catch (const winrt::hresult_error&) {
                // E_NOTIMPL and REGDB_E_CLASSNOTREG while the subsystem is not ready: quiet retry.
            } catch (...) {}
        }
    } catch (...) {
        Wh_Log(L"alerts: notification listener failed");
    }
    return 0;
}

void CALLBACK AlOsdEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG, DWORD, DWORD) {
    if (object != OBJID_WINDOW || !hwnd || !g_alMod) return;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"XamlExplorerHostIslandWindow")) return;
    RECT r{};
    GetWindowRect(hwnd, &r);
    if (r.right - r.left > 700 || r.bottom - r.top > 300 || r.right <= r.left) return;  // Not the fullscreen-sized host.
    AlertsModule& m = *g_alMod;
    if (event == EVENT_OBJECT_SHOW) m.OsdShown(false);
    else m.OsdHidden();
}

LRESULT CALLBACK AlWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_alMod) return g_alMod->Wnd(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT AlertsModule::Wnd(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_alMsgToast && g_alMsgToast) { g_alHookSeen = true; SetEvent(g_alWake); return 0; }
    if (msg == g_alMsgOsd && g_alMsgOsd) {  // lParam 1: the hook moved the popup onto the bubble, so the island yields.
        if (lp == 2) {  // 2: the hook hid the popup; the island shows its own slider.
            osdSeen_ = true;
            if (g_alLevelRead) SetEvent(g_alLevelRead);
            return 0;
        }
        if (lp) OsdShown(true);
        else osdSeen_ = true;
        return 0;
    }
    switch (msg) {
        case WM_AL_CAPS: OnCaps(lp != 0); return 0;
        case WM_AL_PRIVACY: SetTimer(hwnd, kAlPrivacy, 250, nullptr); return 0;
        case WM_AL_LEVEL: OnLevels(wp, lp); return 0;
        case WM_AL_DNDFAIL: Launch(L"ms-settings:quietmomentshome"); return 0;
        case WM_POWERBROADCAST: OnPower(); return TRUE;
        case WM_CLIPBOARDUPDATE: OnClipboard(); return 0;
        case WM_DEVICECHANGE: OnDevice(wp, lp); return TRUE;
        case WM_TIMER:
            if (wp == kAlTick) Tick();
            else if (wp == kAlHeartbeat) UpdateToastFlag();
            else if (wp == kAlClipboard) { KillTimer(hwnd, kAlClipboard); ReadClipboardNow(); }
            else if (wp == kAlOsd) OsdHidden();
            else if (wp == kAlPrivacy) { KillTimer(hwnd, kAlPrivacy); ReadPrivacy(); }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- Lifetime ----

void AlertsModule::Start(Services& s) {
    sv_ = &s;
    g_alMod = this;
    g_alStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_alWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_alLevelRead = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_alLevelSet = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_alDndOff = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_alMsgToast = RegisterWindowMessageW(L"Windhawk_TopBar_ToastShown");
    g_alMsgOsd = RegisterWindowMessageW(L"Windhawk_TopBar_Osd");
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = AlWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kAlertsClass;
    RegisterClassW(&wc);
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kAlertsClass, L"Top bar alerts", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!hwnd_) return;
    g_alHwnd = hwnd_;
    ChangeWindowMessageFilterEx(hwnd_, g_alMsgToast, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd_, g_alMsgOsd, MSGFLT_ALLOW, nullptr);
    powerA_ = RegisterPowerSettingNotification(hwnd_, &kGuidBatteryPercent, DEVICE_NOTIFY_WINDOW_HANDLE);
    powerB_ = RegisterPowerSettingNotification(hwnd_, &kGuidAcDc, DEVICE_NOTIFY_WINDOW_HANDLE);
    AddClipboardFormatListener(hwnd_);
    SetTimer(hwnd_, kAlHeartbeat, 10000, nullptr);
    // DND state through WNF (read-only).
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
        if (auto query = reinterpret_cast<AlWnfQuery>(GetProcAddress(nt, "NtQueryWnfStateData"))) {
            ULONG stamp = 0, size = sizeof(int);
            int v = 0;
            ULONG64 name = kAlWnfQuietHours;
            if (query(&name, nullptr, nullptr, &stamp, &v, &size) == 0 && size >= sizeof(int)) dndValue_ = v;
        }
        if (auto sub = reinterpret_cast<AlWnfSubscribe>(GetProcAddress(nt, "RtlSubscribeWnfStateChangeNotification")))
            sub(&g_alWnfSub, kAlWnfQuietHours, 0, AlWnfProc, nullptr, nullptr, 0, 0);
    }
    // The volume and brightness popup: window events from Explorer (log, and a fallback signal).
    DWORD pid = 0;
    if (HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr)) GetWindowThreadProcessId(tray, &pid);
    if (pid) osdHook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE, nullptr, AlOsdEvent, pid, 0, WINEVENT_OUTOFCONTEXT);
    threads_.push_back(CreateThread(nullptr, 0, AlCapsThread, nullptr, 0, nullptr));
    threads_.push_back(CreateThread(nullptr, 0, AlPrivacyThread, nullptr, 0, nullptr));
    threads_.push_back(CreateThread(nullptr, 0, AlBtThread, nullptr, 0, nullptr));
    threads_.push_back(CreateThread(nullptr, 0, AlNotifThread, nullptr, 0, nullptr));
    threads_.push_back(CreateThread(nullptr, 0, AlLevelThread, nullptr, 0, nullptr));
    OnPower();
}

void AlertsModule::Stop() {
    if (g_alStop) SetEvent(g_alStop);
    if (osdHook_) UnhookWinEvent(osdHook_);
    osdHook_ = nullptr;
    if (g_alWnfSub) {
        if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
            if (auto unsub = reinterpret_cast<AlWnfUnsubscribe>(GetProcAddress(nt, "RtlUnsubscribeWnfStateChangeNotification"))) unsub(g_alWnfSub);
        g_alWnfSub = nullptr;
    }
    for (HANDLE t : threads_) {
        if (!t) continue;
        WaitForSingleObject(t, 3000);
        CloseHandle(t);
    }
    threads_.clear();
    if (hwnd_) {
        RemoveClipboardFormatListener(hwnd_);
        if (powerA_) UnregisterPowerSettingNotification(powerA_);
        if (powerB_) UnregisterPowerSettingNotification(powerB_);
        g_alHwnd = nullptr;
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    g_alListener = nullptr;
    g_alMod = nullptr;
    if (g_hwnd) {
        RemovePropW(g_hwnd, L"TopBarToastsOk");
        RemovePropW(g_hwnd, L"TopBarToastsTick");
    }
    UnregisterClassW(kAlertsClass, GetModuleHandleW(nullptr));
    if (g_alStop) CloseHandle(g_alStop);
    if (g_alWake) CloseHandle(g_alWake);
    if (g_alLevelRead) CloseHandle(g_alLevelRead);
    if (g_alLevelSet) CloseHandle(g_alLevelSet);
    if (g_alDndOff) CloseHandle(g_alDndOff);
    g_alStop = g_alWake = g_alLevelRead = g_alLevelSet = g_alDndOff = nullptr;
}

IModule* AlertsModuleInstance() {
    static AlertsModule m;
    return &m;
}

// Hooks called from the island and the pill.
void AlertsDrawOverlay(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, float L, float R, float B, float s) {
    if (g_alMod) g_alMod->DrawOverlay(rt, brush, L, R, B, s);
}
void AlertsPublishGeometry() {
    if (g_alMod) g_alMod->PublishGeometry();
}
// The shelf handed something to the phone (or could not): a short confirmation.
void AlertsPhoneSent(int sent, int failed) {
    if (!g_alMod) return;
    AItem it;
    it.kind = AK::Phone;
    it.ms = 2200;
    if (sent) {
        it.title = AlT(L"alert.phone.sent", L"Sent to phone");
        if (failed) it.sub = AlFmt(AlT(L"alert.phone.some", L"%d sent, %d failed"), sent, failed);
        else if (sent > 1) it.sub = AlFmt(AlT(L"alert.phone.n", L"%d items"), sent);
        it.good = !failed;
        it.danger = failed > 0;
    } else {
        it.title = AlT(L"alert.phone.fail", L"Could not send to phone");
        it.danger = true;
    }
    g_alMod->Enqueue(it);
}
bool AlertsHoldsHover() {
    return g_alMod && g_alMod->HoldsHover();
}
void AlertsFullscreenChanged() {
    if (g_alMod) g_alMod->FullscreenChanged();
}
float AlertsDotsReserve() {
    return g_alMod ? 12.0f * g_alMod->Dots() : 0.0f;
}
