// ---- Volume and brightness for the island's own slider ----
// Read on request (tray-bridge hid the native popup: a key was pressed), set on request (the slider).
// Its own MTA thread, so Core Audio and WMI calls never run on the UI thread.

struct AlLevels {
    Com<IAudioEndpointVolume> ep;
    Com<IWbemServices> wmi;

    void Connect() {
        ep.Reset();
        Com<IMMDeviceEnumerator> en;
        Com<IMMDevice> dev;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(en.Put()))) &&
            SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, dev.Put())))
            dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(ep.Put()));
    }

    int Volume(bool* mute) {
        Connect();  // The default device may have changed since the last press; cheap enough per press.
        float v = 0;
        BOOL m = FALSE;
        if (!ep || FAILED(ep->GetMasterVolumeLevelScalar(&v))) return -1;
        ep->GetMute(&m);
        *mute = m != FALSE;
        return (int)std::lround(v * 100.0f);
    }

    bool Wmi() {
        if (wmi) return true;
        Com<IWbemLocator> loc;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(loc.Put())))) return false;
        BSTR ns = SysAllocString(L"ROOT\\WMI");
        const HRESULT hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, wmi.Put());
        SysFreeString(ns);
        if (FAILED(hr)) return false;
        CoSetProxyBlanket(wmi.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        return true;
    }

    // First instance of a class. Brightness exists on the internal panel only (a desktop gets -1).
    bool First(const wchar_t* query, Com<IWbemClassObject>& out) {
        if (!Wmi()) return false;
        BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(query);
        Com<IEnumWbemClassObject> en;
        const HRESULT hr = wmi->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, en.Put());
        SysFreeString(lang);
        SysFreeString(q);
        ULONG n = 0;
        return SUCCEEDED(hr) && en && SUCCEEDED(en->Next(2000, 1, out.Put(), &n)) && n == 1;
    }

    int Brightness() {
        Com<IWbemClassObject> o;
        if (!First(L"SELECT CurrentBrightness FROM WmiMonitorBrightness WHERE Active=TRUE", o)) return -1;
        VARIANT v;
        VariantInit(&v);
        int r = -1;
        if (SUCCEEDED(o->Get(L"CurrentBrightness", 0, &v, nullptr, nullptr))) r = v.vt == VT_UI1 ? v.bVal : v.vt == VT_I4 ? v.lVal : -1;
        VariantClear(&v);
        return r;
    }

    void SetBrightness(int pct) {
        Com<IWbemClassObject> inst;
        if (!First(L"SELECT * FROM WmiMonitorBrightnessMethods WHERE Active=TRUE", inst)) return;
        VARIANT path;
        VariantInit(&path);
        if (FAILED(inst->Get(L"__PATH", 0, &path, nullptr, nullptr)) || path.vt != VT_BSTR) { VariantClear(&path); return; }
        Com<IWbemClassObject> cls, def, args;
        BSTR clsName = SysAllocString(L"WmiMonitorBrightnessMethods"), method = SysAllocString(L"WmiSetBrightness");
        if (SUCCEEDED(wmi->GetObject(clsName, 0, nullptr, cls.Put(), nullptr)) &&
            SUCCEEDED(cls->GetMethod(method, 0, def.Put(), nullptr)) && def && SUCCEEDED(def->SpawnInstance(0, args.Put()))) {
            VARIANT t, b;
            VariantInit(&t);
            t.vt = VT_I4;
            t.lVal = 0;
            VariantInit(&b);
            b.vt = VT_UI1;
            b.bVal = (BYTE)std::clamp(pct, 0, 100);
            args->Put(L"Timeout", 0, &t, 0);
            args->Put(L"Brightness", 0, &b, 0);
            wmi->ExecMethod(path.bstrVal, method, 0, nullptr, args.Get(), nullptr, nullptr);
        }
        SysFreeString(clsName);
        SysFreeString(method);
        VariantClear(&path);
    }
};

// ---- Do Not Disturb off: the QuietHoursSettings COM class (undocumented) ----
// The object Settings and Quick Settings drive: CLSID_QuietHoursSettings, hosted out of process by
// WpnUserService, interface IQuietHoursSettings (layout from Rafael Rivera's quiethours.idl, also used
// by WinDndToggle and others). Only the first two methods are declared. The class is registered in the
// 64-bit view only, so a 32-bit host asks for the 64-bit server; the interface's proxy is registered in
// both views. Setting the user profile to Unrestricted is what turning DND off in Quick Settings does.
struct IAlQuietHoursSettings : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_UserSelectedProfile(LPWSTR* profile) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_UserSelectedProfile(LPCWSTR profile) = 0;
};
constexpr GUID kAlClsidQuietHours = {0xf53321fa, 0x34f8, 0x4b7f, {0xb9, 0xa3, 0x36, 0x18, 0x77, 0xcb, 0x94, 0xcf}};
constexpr GUID kAlIidQuietHours = {0x6bff4732, 0x81ec, 0x4ffb, {0xae, 0x67, 0xb6, 0xc1, 0xbc, 0x29, 0x63, 0x1f}};
constexpr wchar_t kAlProfileOff[] = L"Microsoft.QuietHoursProfile.Unrestricted";

bool AlDndOff() {
    IAlQuietHoursSettings* qh = nullptr;
    HRESULT hr = CoCreateInstance(kAlClsidQuietHours, nullptr, CLSCTX_LOCAL_SERVER | CLSCTX_ACTIVATE_64_BIT_SERVER, kAlIidQuietHours,
                                  reinterpret_cast<void**>(&qh));
    if (FAILED(hr)) hr = CoCreateInstance(kAlClsidQuietHours, nullptr, CLSCTX_LOCAL_SERVER, kAlIidQuietHours, reinterpret_cast<void**>(&qh));
    if (FAILED(hr) || !qh) {
        Wh_Log(L"alerts: QuietHoursSettings unavailable, 0x%08lX", (unsigned long)hr);
        return false;
    }
    hr = qh->put_UserSelectedProfile(kAlProfileOff);
    bool ok = SUCCEEDED(hr);
    if (ok) {  // Read it back: a changed vtable would succeed and do something else.
        LPWSTR now = nullptr;
        ok = SUCCEEDED(qh->get_UserSelectedProfile(&now)) && now && !wcscmp(now, kAlProfileOff);
        if (now) CoTaskMemFree(now);
    }
    Wh_Log(L"alerts: DND off via QuietHoursSettings: hr 0x%08lX, %s", (unsigned long)hr, ok ? L"verified" : L"not verified");
    qh->Release();
    return ok;
}

// Volume changes as Core Audio reports them, one per key step, so the slider follows a held key
// instead of jumping to wherever the level was when the popup appeared. It also shows the slider
// when the popup hook isn't working. Brightness has no cheap equivalent: the hook trigger starts a
// short burst of reads instead (see AlLevelThread).
std::atomic<int> g_alLastBright{-1};

struct AlVolumeCallback : IAudioEndpointVolumeCallback {
    std::atomic<ULONG> refs{1};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioEndpointVolumeCallback)) {
            *out = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA d) override {
        if (!d) return S_OK;
        const int vol = (int)std::lround(d->fMasterVolume * 100.0f);
        if (HWND h = g_alHwnd.load())
            PostMessageW(h, WM_AL_LEVEL, (WPARAM)(vol & 0xFFFF) | (d->bMuted ? 0x10000 : 0), (LPARAM)g_alLastBright.load());
        return S_OK;
    }
};

// Keeps the callback on the current default output device (it changes with headphones).
struct AlVolumeWatch {
    Com<IAudioEndpointVolume> ep;
    AlVolumeCallback* cb = nullptr;
    std::wstring id;

    void Ensure() {
        Com<IMMDeviceEnumerator> en;
        Com<IMMDevice> dev;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(en.Put()))) ||
            FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, dev.Put())))
            return;
        LPWSTR now = nullptr;
        if (FAILED(dev->GetId(&now)) || !now) return;
        const bool same = id == now;
        std::wstring nowId = now;
        CoTaskMemFree(now);
        if (same && ep) return;
        Stop();
        if (FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(ep.Put()))) || !ep)
            return;
        cb = new AlVolumeCallback();
        if (FAILED(ep->RegisterControlChangeNotify(cb))) {
            cb->Release();
            cb = nullptr;
            ep.Reset();
            return;
        }
        id = nowId;
    }
    void Stop() {
        if (ep && cb) ep->UnregisterControlChangeNotify(cb);
        if (cb) cb->Release();
        cb = nullptr;
        ep.Reset();
        id.clear();
    }
};

DWORD WINAPI AlLevelThread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        AlLevels lv;
        AlVolumeWatch watch;
        HANDLE waits[] = {g_alStop, g_alLevelRead, g_alLevelSet, g_alDndOff};
        auto post = [&] {
            bool mute = false;
            const int vol = lv.Volume(&mute);
            const int bright = lv.Brightness();
            g_alLastBright = bright;
            if (HWND h = g_alHwnd.load()) PostMessageW(h, WM_AL_LEVEL, (WPARAM)(vol & 0xFFFF) | (mute ? 0x10000 : 0), (LPARAM)bright);
        };
        post();  // Baseline, so the first key press can be told apart.
        watch.Ensure();
        bool stop = false;
        for (;;) {
            const DWORD w = WaitForMultipleObjects(4, waits, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0 + 1) {
                post();
                watch.Ensure();  // Follow a new default device from the next step on.
                // A held brightness key steps several times while the popup is up, and the new level can
                // trail the popup: read every 100 ms for 1.5 s and pass on each change.
                int last = g_alLastBright.load();
                HANDLE burstWaits[] = {g_alStop, g_alLevelSet};
                for (int i = 0; i < 15 && !stop; i++) {
                    const DWORD b = WaitForMultipleObjects(2, burstWaits, FALSE, 100);
                    if (b == WAIT_OBJECT_0) stop = true;
                    else if (b == WAIT_OBJECT_0 + 1) {  // A drag wants the level set: stop reading, let it through.
                        SetEvent(g_alLevelSet);
                        break;
                    } else if (lv.Brightness() != last) {
                        post();
                        last = g_alLastBright.load();
                    }
                }
                if (stop) break;
            } else if (w == WAIT_OBJECT_0 + 2) {
                const int v = g_alSetVolume.exchange(-1), b = g_alSetBright.exchange(-1);
                if (v >= 0) {
                    lv.Connect();
                    if (lv.ep) {
                        lv.ep->SetMute(FALSE, nullptr);
                        lv.ep->SetMasterVolumeLevelScalar(v / 100.0f, nullptr);
                    }
                }
                if (b >= 0) lv.SetBrightness(b);
            } else if (w == WAIT_OBJECT_0 + 3) {
                if (!AlDndOff())  // Settings is the fallback; opened from the UI thread.
                    if (HWND h = g_alHwnd.load()) PostMessageW(h, WM_AL_DNDFAIL, 0, 0);
            } else {
                break;
            }
        }
        watch.Stop();
    }
    CoUninitialize();
    return 0;
}
