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

DWORD WINAPI AlLevelThread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        AlLevels lv;
        HANDLE waits[] = {g_alStop, g_alLevelRead, g_alLevelSet};
        auto post = [&] {
            bool mute = false;
            const int vol = lv.Volume(&mute);
            const int bright = lv.Brightness();
            if (HWND h = g_alHwnd.load()) PostMessageW(h, WM_AL_LEVEL, (WPARAM)(vol & 0xFFFF) | (mute ? 0x10000 : 0), (LPARAM)bright);
        };
        post();  // Baseline, so the first key press can be told apart.
        for (;;) {
            const DWORD w = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0 + 1) {
                post();
                // The popup can show before the new brightness is readable: look once more.
                if (WaitForSingleObject(g_alStop, 300) == WAIT_OBJECT_0) break;
                post();
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
            } else {
                break;
            }
        }
    }
    CoUninitialize();
    return 0;
}
