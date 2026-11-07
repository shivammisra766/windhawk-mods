// ---- Media engine: SMTC on one MTA thread, event driven. See specs/media.md. ----
// Nothing here touches Direct2D. It publishes immutable snapshots; the UI thread (media.inc) draws them.

extern Services g_services;  // Defined in island.inc.
extern LayeredSurface g_isSurface;

namespace wm = winrt::Windows::Media::Control;
namespace ws = winrt::Windows::Storage::Streams;

struct MArt {
    int w = 0, h = 0;
    std::vector<DWORD> px;  // premultiplied BGRA
    float acc[3] = {0.298f, 0.788f, 0.941f};
    bool hasAccent = false;
    size_t hash = 0;
};

struct MSource {
    std::wstring aumid, name;
    bool playing = false;
    std::shared_ptr<const MArt> icon;
};

struct MSnap {
    bool has = false, playing = false;
    std::wstring title, artist, album, aumid, sourceName;
    std::shared_ptr<const MArt> art;
    std::vector<MSource> sources;
    int active = -1;
    long long posMs = 0, durMs = 0;
    ULONGLONG baseTick = 0;  // GetTickCount64 when posMs was true
    double rate = 1.0;
    bool canNext = true, canPrev = true, canSeek = false, canToggle = true;
    unsigned trackSeq = 0;  // Bumped for each new track that may open the panel.
};

std::wstring LowerW(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)towlower(c);
    return s;
}

bool HasWord(const std::wstring& lower, const wchar_t* w) { return lower.find(w) != std::wstring::npos; }

// Display name. Fix for the old substring bug: "edge" and "media" matched unrelated ids, so use whole tokens.
std::wstring FriendlySourceName(const std::wstring& aumid) {
    const std::wstring l = LowerW(aumid);
    struct { const wchar_t* key; const wchar_t* name; } table[] = {
        {L"spotify", L"Spotify"}, {L"msedge", L"Edge"}, {L"chrome", L"Chrome"}, {L"firefox", L"Firefox"},
        {L"brave", L"Brave"}, {L"opera", L"Opera"}, {L"vlc", L"VLC"}, {L"zunemusic", L"Media Player"},
        {L"zunevideo", L"Movies & TV"}, {L"applemusic", L"Apple Music"}, {L"itunes", L"iTunes"},
        {L"musicbee", L"MusicBee"}, {L"foobar", L"foobar2000"}, {L"tidal", L"TIDAL"}, {L"vivaldi", L"Vivaldi"}};
    for (auto& t : table) if (HasWord(l, t.key)) return t.name;
    std::wstring s = aumid;
    size_t p = s.find_last_of(L"!\\/");
    if (p != std::wstring::npos) s = s.substr(p + 1);
    if (s.size() > 4 && !_wcsicmp(s.c_str() + s.size() - 4, L".exe")) s.resize(s.size() - 4);
    p = s.find_last_of(L'.');
    if (p != std::wstring::npos && p + 1 < s.size()) s = s.substr(p + 1);
    if (!s.empty()) s[0] = (wchar_t)towupper(s[0]);
    return s.empty() ? aumid : s;
}

// The base name of the process that owns `hwnd`, lowercase, without .exe. Also fills the full path.
std::wstring ProcessOfWindow(HWND hwnd, std::wstring* path) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || pid == GetCurrentProcessId()) return L"";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = (DWORD)std::size(buf);
    std::wstring out;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
        if (path) *path = buf;
        std::wstring base = buf;
        const size_t p = base.find_last_of(L"\\/");
        if (p != std::wstring::npos) base = base.substr(p + 1);
        if (base.size() > 4 && !_wcsicmp(base.c_str() + base.size() - 4, L".exe")) base.resize(base.size() - 4);
        out = LowerW(base);
    }
    CloseHandle(h);
    return out;
}

bool ProcessMatchesAumid(const std::wstring& procBase, const std::wstring& aumidLower) {
    if (procBase.size() < 3) return false;
    if (HasWord(aumidLower, procBase.c_str())) return true;
    if (procBase == L"msedge" && HasWord(aumidLower, L"msedge")) return true;
    return false;
}

struct WinFind {
    const std::wstring* aumidLower;
    const std::wstring* title;
    HWND byTitle = nullptr, byProcess = nullptr;
    std::wstring exePath;
};

BOOL CALLBACK FindWinProc(HWND hwnd, LPARAM lp) {
    auto* f = reinterpret_cast<WinFind*>(lp);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (!wcscmp(cls, kClassName) || !wcscmp(cls, kIslandClass) || !wcscmp(cls, L"Shell_TrayWnd")) return TRUE;
    wchar_t cap[256] = {};
    GetWindowTextW(hwnd, cap, 256);
    if (!cap[0]) return TRUE;
    std::wstring path;
    const std::wstring proc = ProcessOfWindow(hwnd, &path);
    if (proc.empty() || !ProcessMatchesAumid(proc, *f->aumidLower)) return TRUE;
    if (f->exePath.empty()) f->exePath = path;
    if (!f->byProcess) f->byProcess = hwnd;
    // Bug 7 of the old mod: an empty title matched the first window. Only match a non-empty one.
    if (!f->title->empty() && !f->byTitle && LowerW(cap).find(LowerW(*f->title)) != std::wstring::npos) f->byTitle = hwnd;
    return TRUE;
}

// VLC with no title in SMTC: use its window caption, which ends in "VLC media player".
struct VlcCtx { std::wstring out; };
BOOL CALLBACK VlcEnumProc(HWND hwnd, LPARAM lp) {
    auto* c = reinterpret_cast<VlcCtx*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t cap[256] = {};
    GetWindowTextW(hwnd, cap, 256);
    const std::wstring s = cap;
    const std::wstring tail = L" - VLC media player";
    if (s.size() > tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0 &&
        ProcessOfWindow(hwnd, nullptr) == L"vlc") {
        c->out = s.substr(0, s.size() - tail.size());
        return FALSE;
    }
    return TRUE;
}

std::wstring VlcCaption() {
    VlcCtx ctx;
    EnumWindows(VlcEnumProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.out;
}

// ---- Art decode and accent ----

void RgbToHsl(float r, float g, float b, float& h, float& s, float& l) {
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    l = (mx + mn) / 2;
    const float d = mx - mn;
    h = 0; s = 0;
    if (d > 1e-5f) {
        s = l > 0.5f ? d / (2 - mx - mn) : d / (mx + mn);
        if (mx == r) h = (g - b) / d + (g < b ? 6 : 0);
        else if (mx == g) h = (b - r) / d + 2;
        else h = (r - g) / d + 4;
        h /= 6;
    }
}

float HueToRgb(float p, float q, float t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0f / 6) return p + (q - p) * 6 * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
    return p;
}

void HslToRgb(float h, float s, float l, float& r, float& g, float& b) {
    if (s <= 1e-5f) { r = g = b = l; return; }
    const float q = l < 0.5f ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    r = HueToRgb(p, q, h + 1.0f / 3);
    g = HueToRgb(p, q, h);
    b = HueToRgb(p, q, h - 1.0f / 3);
}

float RelLum(float r, float g, float b) {
    auto f = [](float c) { return c <= 0.03928f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); };
    return 0.2126f * f(r) + 0.7152f * f(g) + 0.0722f * f(b);
}

// Spec section 9.2: vivid dominant colour, 16^3 buckets, at least 3:1 against the bar's surface.
void ComputeAccent(MArt& a) {
    struct B { float w = 0; int n = 0; double r = 0, g = 0, b = 0, sat = 0; };
    std::vector<B> bk(4096);
    for (DWORD px : a.px) {
        const int a8 = px >> 24;
        if (a8 < 32) continue;
        const int r = std::min(255, (int)(((px >> 16) & 255) * 255 / a8)), g = std::min(255, (int)(((px >> 8) & 255) * 255 / a8)),
                  b = std::min(255, (int)((px & 255) * 255 / a8));
        const int lum = (299 * r + 587 * g + 114 * b) / 1000;
        if (lum < 36 || lum > 220) continue;
        const int mx = std::max({r, g, b}), mn = std::min({r, g, b});
        const int sat = mx ? (mx - mn) * 255 / mx : 0;
        if (sat < 28) continue;
        B& x = bk[((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)];
        x.w += 1.0f + sat / 32.0f;
        x.n++; x.r += r; x.g += g; x.b += b; x.sat += sat;
    }
    int top[3] = {-1, -1, -1};
    for (int i = 0; i < 4096; i++) {
        if (!bk[i].n) continue;
        for (int k = 0; k < 3; k++) {
            if (top[k] < 0 || bk[i].w > bk[top[k]].w) {
                for (int m = 2; m > k; m--) top[m] = top[m - 1];
                top[k] = i;
                break;
            }
        }
    }
    int best = -1;
    for (int k = 0; k < 3; k++) {
        if (top[k] < 0) continue;
        if (best < 0 || bk[top[k]].sat / bk[top[k]].n > bk[best].sat / bk[best].n) best = top[k];
    }
    if (best < 0) return;  // Black and white art keeps the default.
    double sr = 0, sg = 0, sb = 0;
    long cnt = 0;
    const int br = best >> 8, bg = (best >> 4) & 15, bb = best & 15;
    for (int dr = -1; dr <= 1; dr++) for (int dg = -1; dg <= 1; dg++) for (int db = -1; db <= 1; db++) {
        const int r = br + dr, g = bg + dg, b = bb + db;
        if (r < 0 || g < 0 || b < 0 || r > 15 || g > 15 || b > 15) continue;
        const B& x = bk[(r << 8) | (g << 4) | b];
        sr += x.r; sg += x.g; sb += x.b; cnt += x.n;
    }
    if (!cnt) return;
    float r = (float)(sr / cnt / 255), g = (float)(sg / cnt / 255), b = (float)(sb / cnt / 255);
    float h, s, l;
    RgbToHsl(r, g, b, h, s, l);
    s = std::max(s, 0.35f);
    const float bgLum = RelLum(0x10 / 255.0f, 0x10 / 255.0f, 0x10 / 255.0f);
    for (int i = 0; i < 20; i++) {
        HslToRgb(h, s, l, r, g, b);
        if ((RelLum(r, g, b) + 0.05f) / (bgLum + 0.05f) >= 3.0f) break;
        l = std::min(1.0f, l + 0.04f);
    }
    a.acc[0] = r; a.acc[1] = g; a.acc[2] = b;
    a.hasAccent = true;
}

// Bytes -> 128x128 premultiplied BGRA (centre crop). Bug 10 of the old mod: big art was rejected; now it is scaled.
std::shared_ptr<MArt> DecodeArt(const std::vector<uint8_t>& bytes) {
    Com<IWICImagingFactory> wic;
    if (bytes.empty() || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                                 __uuidof(IWICImagingFactory), reinterpret_cast<void**>(wic.Put())))) return nullptr;
    Com<IWICStream> stream;
    Com<IWICBitmapDecoder> dec;
    Com<IWICBitmapFrameDecode> frame;
    Com<IWICFormatConverter> conv;
    Com<IWICBitmapScaler> scaler;
    if (FAILED(wic->CreateStream(stream.Put())) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), (DWORD)bytes.size())) ||
        FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, dec.Put())) ||
        FAILED(dec->GetFrame(0, frame.Put())) || FAILED(wic->CreateFormatConverter(conv.Put())) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom))) return nullptr;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (!w || !h || w > 16384 || h > 16384) return nullptr;
    constexpr UINT N = 128;
    const float k = (float)N / (float)std::min(w, h);
    const UINT sw = std::max<UINT>(N, (UINT)lroundf(w * k)), sh = std::max<UINT>(N, (UINT)lroundf(h * k));
    if (FAILED(wic->CreateBitmapScaler(scaler.Put())) ||
        FAILED(scaler->Initialize(conv.Get(), sw, sh, WICBitmapInterpolationModeFant))) return nullptr;
    auto art = std::make_shared<MArt>();
    art->w = art->h = N;
    art->px.resize((size_t)N * N);
    WICRect rc{(INT)((sw - N) / 2), (INT)((sh - N) / 2), (INT)N, (INT)N};
    if (FAILED(scaler->CopyPixels(&rc, N * 4, (UINT)(art->px.size() * 4), reinterpret_cast<BYTE*>(art->px.data())))) return nullptr;
    ComputeAccent(*art);
    return art;
}

// ---- The engine ----

enum class MCmdKind { Toggle, Next, Prev, Seek, Pick, Open };
struct MCmd { MCmdKind kind; long long arg = 0; std::wstring s; };

template <class Op>
bool AwaitOp(const Op& op, int ms = 2000) {
    try {
        if (op.wait_for(std::chrono::milliseconds(ms)) == winrt::Windows::Foundation::AsyncStatus::Completed) return true;
        op.Cancel();
    } catch (...) {}
    return false;
}

class MediaEngine {
public:
    void Start(std::function<void()> notify) {
        notify_ = std::move(notify);
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        snap_ = std::make_shared<const MSnap>();
        alive_ = std::make_shared<std::atomic<bool>>(true);
        thread_ = CreateThread(nullptr, 0, ThreadMain, this, 0, nullptr);
    }
    void Stop() {
        if (alive_) *alive_ = false;
        if (stop_) SetEvent(stop_);
        if (thread_) {
            WaitForSingleObject(thread_, 2000);
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        if (stop_) { CloseHandle(stop_); stop_ = nullptr; }
        if (wake_) { CloseHandle(wake_); wake_ = nullptr; }
    }
    std::shared_ptr<const MSnap> Snapshot() {
        std::lock_guard<std::mutex> g(mu_);
        return snap_;
    }
    void Command(MCmd c) {
        {
            std::lock_guard<std::mutex> g(mu_);
            if (c.kind == MCmdKind::Seek) {  // Only the newest seek matters.
                cmds_.erase(std::remove_if(cmds_.begin(), cmds_.end(), [](const MCmd& m) { return m.kind == MCmdKind::Seek; }), cmds_.end());
            }
            cmds_.push_back(std::move(c));
        }
        if (wake_) SetEvent(wake_);
    }
    // The panel is open: read the timeline every 1.5 s, like the old mod, so the jitter guard works unchanged.
    void SetPanelOpen(bool open) {
        if (panelOpen_.exchange(open) != open && wake_) SetEvent(wake_);
    }
    // The island shows media (pill or player). Only then does the 1-2 s polling fallback run.
    void SetShown(bool shown) {
        if (shown_.exchange(shown) != shown && wake_) SetEvent(wake_);
    }
    std::shared_ptr<std::atomic<bool>> Alive() const { return alive_; }

private:
    struct SessRec {
        wm::GlobalSystemMediaTransportControlsSession s{nullptr};
        std::wstring aumid;
        bool playing = false;
        wm::GlobalSystemMediaTransportControlsSession::PlaybackInfoChanged_revoker r1;
        wm::GlobalSystemMediaTransportControlsSession::MediaPropertiesChanged_revoker r2;
    };
    struct SrcState {
        bool wasPlaying = false;
        unsigned startedSeq = 0;
        std::shared_ptr<const MArt> icon;
        ULONGLONG iconTry = 0;
    };

    static DWORD WINAPI ThreadMain(void* self) {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        static_cast<MediaEngine*>(self)->Run();
        winrt::uninit_apartment();
        return 0;
    }

    void Run() {
        HANDLE h[2] = {stop_, wake_};
        DWORD timeout = 0;
        for (;;) {
            const DWORD w = WaitForMultipleObjects(2, h, FALSE, timeout);
            if (w == WAIT_OBJECT_0) break;
            if (w == WAIT_OBJECT_0 + 1 && WaitForSingleObject(stop_, 100) == WAIT_OBJECT_0) break;  // Coalesce bursts.
            // A timeout wake is a fallback poll: players whose events are missing or late get re-read here.
            pollTick_ = w == WAIT_TIMEOUT;
            try {
                RunCommands();
                Refresh();
            } catch (...) {
                Wh_Log(L"media: refresh failed");
                forceRebuild_ = true;
            }
            pollTick_ = false;
            timeout = NextTimeout();
        }
        recs_.clear();
        mgrSess_.revoke();
        mgrCur_.revoke();
        mgr_ = nullptr;
    }

    // No sessions: only events and a slow health check. Playing: events plus a fallback, faster with the panel open.
    // Polling fallback: while the island shows media, re-read every 1.5-2 s (playing, or paused under 30 s ago),
    // then 15 s for a long pause. Hidden or idle keeps the slow health checks, so nothing playing costs nothing.
    DWORD NextTimeout() {
        const ULONGLONG now = GetTickCount64();
        DWORD t = 120000;
        if (!recs_.empty()) t = 30000;
        if (cur_.playing) t = panelOpen_ ? 1500 : 5000;
        if (shown_ && cur_.has) {
            if (cur_.playing || now - lastPlayingTick_ < 30000) t = std::min<DWORD>(t, panelOpen_ ? 1500 : 2000);
            else t = std::min<DWORD>(t, 15000);
        }
        if ((forceRebuild_ || !mgr_) && cur_.has) t = std::min<DWORD>(t, 2000);  // Recovering a dropped session.
        if (artRetryAt_) t = std::min<DWORD>(t, artRetryAt_ > now ? (DWORD)(artRetryAt_ - now) + 5 : 5);
        return t;
    }

    void DropManager() {
        recs_.clear();
        mgrSess_.revoke();
        mgrCur_.revoke();
        mgr_ = nullptr;
        sessionsDirty_ = true;
    }

    // Clears the shown media, keeping the auto-expand sequence.
    void ClearSnapshot(std::vector<MSource> sources) {
        const unsigned seq = cur_.trackSeq;
        cur_ = MSnap();
        cur_.trackSeq = seq;
        cur_.sources = std::move(sources);
        firstRead_ = false;
        lastKey_.clear();
        artKey_.clear();
        artRetryAt_ = 0;
        Publish(false);
    }

    // The manager can drop and recreate sessions without raising SessionsChanged. On a poll, compare the list.
    void VerifySessions() {
        auto list = mgr_.GetSessions();
        if (list.Size() != recs_.size()) { sessionsDirty_ = true; return; }
        for (uint32_t i = 0; i < list.Size(); i++) {
            if (recs_[i].aumid != list.GetAt(i).SourceAppUserModelId().c_str()) { sessionsDirty_ = true; return; }
        }
    }

    bool EnsureManager() {
        if (mgr_) return true;
        try {
            auto op = wm::GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
            if (!AwaitOp(op, 3000)) return false;
            mgr_ = op.GetResults();
            mgrSess_ = mgr_.SessionsChanged(winrt::auto_revoke, [this](auto&&, auto&&) { sessionsDirty_ = true; SetEvent(wake_); });
            mgrCur_ = mgr_.CurrentSessionChanged(winrt::auto_revoke, [this](auto&&, auto&&) { sessionsDirty_ = true; SetEvent(wake_); });
            sessionsDirty_ = true;
            return true;
        } catch (...) {
            mgr_ = nullptr;  // Bug 4: a dead broker left the manager stale forever. Retry on the next health tick.
            return false;
        }
    }

    wm::GlobalSystemMediaTransportControlsSession* Chosen() {
        for (SessRec& r : recs_) if (r.aumid == chosenAumid_ && r.playing) return &r.s;
        for (SessRec& r : recs_) if (r.aumid == chosenAumid_) return &r.s;  // Bug 12: prefer the playing duplicate.
        return nullptr;
    }

    void RunCommands() {
        std::deque<MCmd> cmds;
        {
            std::lock_guard<std::mutex> g(mu_);
            cmds.swap(cmds_);
        }
        for (MCmd& c : cmds) {
            try {
                if (c.kind == MCmdKind::Pick) {
                    dockPick_ = c.s;
                    userSwitchUntil_ = GetTickCount64() + 4000;
                    forceRead_ = true;
                    continue;
                }
                if (c.kind == MCmdKind::Open) { OpenPlayer(); continue; }
                auto* s = Chosen();
                if (!s) continue;  // No media key fallback: it can double-skip.
                if (c.kind == MCmdKind::Toggle) AwaitOp(s->TryTogglePlayPauseAsync());
                else if (c.kind == MCmdKind::Next) AwaitOp(s->TrySkipNextAsync());
                else if (c.kind == MCmdKind::Prev) AwaitOp(s->TrySkipPreviousAsync());
                else if (c.kind == MCmdKind::Seek && cur_.durMs > 0) {
                    const long long target = std::max(0LL, std::min(c.arg, cur_.durMs));
                    AwaitOp(s->TryChangePlaybackPositionAsync((curStartMs_ + target) * 10000));
                    cur_.posMs = target;
                    cur_.baseTick = GetTickCount64();
                    ignoreTimelineUntil_ = cur_.baseTick + 1500;
                    backPending_ = false;
                    Publish(true);
                }
            } catch (...) {}
        }
        if (!cmds.empty()) forceRead_ = true;
    }

    void OpenPlayer() {
        const std::wstring al = LowerW(chosenAumid_);
        WinFind f{&al, &cur_.title};
        EnumWindows(FindWinProc, reinterpret_cast<LPARAM>(&f));
        HWND hw = f.byTitle ? f.byTitle : f.byProcess;
        if (hw) {
            if (IsIconic(hw)) ShowWindow(hw, SW_RESTORE);
            SetForegroundWindow(hw);
        } else if (chosenAumid_.find(L'!') != std::wstring::npos) {
            ShellExecuteW(nullptr, L"open", (L"shell:AppsFolder\\" + chosenAumid_).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    void RebuildSessions() {
        recs_.clear();
        sessionsDirty_ = false;
        auto list = mgr_.GetSessions();
        for (uint32_t i = 0; i < list.Size(); i++) {
            auto s = list.GetAt(i);
            recs_.emplace_back();
            SessRec& r = recs_.back();
            r.s = s;
            r.aumid = s.SourceAppUserModelId().c_str();
            r.r1 = s.PlaybackInfoChanged(winrt::auto_revoke, [this](auto&&, auto&&) { SetEvent(wake_); });
            r.r2 = s.MediaPropertiesChanged(winrt::auto_revoke, [this](auto&&, auto&&) { propsDirty_ = true; SetEvent(wake_); });
        }
    }

    // Spec section 6.2, kept: dock pick, then auto (playing beats paused, newest start wins).
    void ChooseSource(const std::wstring& systemCurrent) {
        std::wstring want;
        unsigned best = 0;
        bool newStart = false;
        for (auto& o : order_) if (srcs_[o].startedSeq > startedBefore_) newStart = true;
        if (newStart) dockPick_.clear();
        bool pickAlive = false;
        for (auto& o : order_) if (o == dockPick_) pickAlive = true;
        if (!pickAlive) dockPick_.clear();
        if (!dockPick_.empty()) { chosenAumid_ = dockPick_; return; }
        for (auto& o : order_) {
            bool playing = false;
            for (SessRec& r : recs_) if (r.aumid == o && r.playing) playing = true;
            const unsigned score = srcs_[o].startedSeq + (playing ? 1000000u : 0u);
            if (srcs_[o].startedSeq == 0 && !playing) continue;
            if (!want.size() || score > best) { want = o; best = score; }
        }
        if (want.empty()) want = !systemCurrent.empty() ? systemCurrent : (order_.empty() ? L"" : order_[0]);
        chosenAumid_ = want;
    }

    std::shared_ptr<const MArt> SourceIcon(const std::wstring& aumid, SrcState& st) {
        if (st.icon) return st.icon;
        const ULONGLONG now = GetTickCount64();
        if (st.iconTry && now - st.iconTry < 15000) return nullptr;  // Bug 6: throttle the window scan.
        st.iconTry = now;
        const std::wstring al = LowerW(aumid);
        const std::wstring none;
        WinFind f{&al, &none};
        EnumWindows(FindWinProc, reinterpret_cast<LPARAM>(&f));
        if (f.exePath.empty()) return nullptr;
        SHFILEINFOW info{};
        if (!SHGetFileInfoW(f.exePath.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) || !info.hIcon) return nullptr;
        std::unique_ptr<Pixmap> pm(PixmapFromIcon(info.hIcon));
        DestroyIcon(info.hIcon);
        if (!pm) return nullptr;
        auto art = std::make_shared<MArt>();
        art->w = pm->w; art->h = pm->h; art->px = std::move(pm->px);
        st.icon = art;
        return art;
    }

    bool ReadArt(const wm::GlobalSystemMediaTransportControlsSessionMediaProperties& props, std::shared_ptr<const MArt>& out, size_t& hash) {
        auto ref = props.Thumbnail();
        if (!ref) return false;
        auto op = ref.OpenReadAsync();
        if (!AwaitOp(op)) return false;
        auto stream = op.GetResults();
        const uint64_t size = stream.Size();
        if (!size || size > (24u << 20)) return false;
        ws::DataReader reader(stream);
        auto load = reader.LoadAsync((uint32_t)size);
        if (!AwaitOp(load)) return false;
        std::vector<uint8_t> bytes((size_t)size);
        reader.ReadBytes(bytes);
        const size_t hsh = std::hash<std::string_view>()(std::string_view((const char*)bytes.data(), bytes.size()));
        if (out && hash == hsh) return true;  // Same bytes: keep the decoded art.
        auto art = DecodeArt(bytes);
        if (!art) return false;
        art->hash = hsh;
        hash = hsh;
        out = art;
        return true;
    }

    void Refresh() {
        const ULONGLONG now = GetTickCount64();
        if (!EnsureManager()) {
            // The broker stays away: after a few tries, stop showing media that can no longer be confirmed.
            if (cur_.has && ++mgrFails_ >= 3) ClearSnapshot({});
            return;
        }
        mgrFails_ = 0;
        std::wstring systemCurrent;
        try {
            if (pollTick_ && !sessionsDirty_ && !forceRebuild_ && !recs_.empty()) VerifySessions();
            if (sessionsDirty_ || recs_.empty() || forceRebuild_) { RebuildSessions(); forceRebuild_ = false; }
            if (auto c = mgr_.GetCurrentSession()) systemCurrent = c.SourceAppUserModelId().c_str();
        } catch (...) {
            DropManager();
            return;  // Keep the last snapshot; the next tick asks for a new manager.
        }
        // Playback state of every session, so a start in another player is noticed.
        for (SessRec& r : recs_) {
            try {
                r.playing = r.s.GetPlaybackInfo().PlaybackStatus() ==
                            wm::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
            } catch (...) {
                r.playing = false;
                forceRebuild_ = true;  // A dead session object: rebuild the list on the next wake.
            }
        }
        // Merge by AUMID, stable order, history forgotten when the source goes away.
        std::vector<std::wstring> present;
        for (SessRec& r : recs_) if (std::find(present.begin(), present.end(), r.aumid) == present.end()) present.push_back(r.aumid);
        order_.erase(std::remove_if(order_.begin(), order_.end(), [&](const std::wstring& o) {
            if (std::find(present.begin(), present.end(), o) != present.end()) return false;
            srcs_.erase(o);
            return true;
        }), order_.end());
        for (auto& p : present) if (std::find(order_.begin(), order_.end(), p) == order_.end()) { order_.push_back(p); srcs_[p]; }
        startedBefore_ = startCounter_;
        for (int pass = 0; pass < 2; pass++) {  // The system's current session counts as the newest of several.
            for (auto& o : order_) {
                if ((o == systemCurrent) != (pass == 1)) continue;
                bool playing = false;
                for (SessRec& r : recs_) if (r.aumid == o && r.playing) playing = true;
                SrcState& st = srcs_[o];
                if (playing && !st.wasPlaying) st.startedSeq = ++startCounter_;
                st.wasPlaying = playing;
            }
        }
        const std::wstring prevAumid = chosenAumid_;
        ChooseSource(systemCurrent);

        MSnap next = cur_;
        next.sources.clear();
        for (auto& o : order_) {
            MSource ms;
            ms.aumid = o;
            ms.name = FriendlySourceName(o);
            for (SessRec& r : recs_) if (r.aumid == o && r.playing) ms.playing = true;
            ms.icon = SourceIcon(o, srcs_[o]);
            next.sources.push_back(ms);
        }
        next.active = -1;
        for (size_t i = 0; i < order_.size(); i++) if (order_[i] == chosenAumid_) next.active = (int)i;

        auto* sess = Chosen();
        if (!sess) {  // Session loss: only now drop the snapshot (bug 3).
            if (recs_.empty() || !chosenAumid_.empty()) ClearSnapshot(next.sources);
            return;
        }
        bool readOk = true;
        try {
            next.has = true;
            next.aumid = chosenAumid_;
            next.sourceName = FriendlySourceName(chosenAumid_);
            auto info = sess->GetPlaybackInfo();
            next.playing = info.PlaybackStatus() == wm::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
            auto ctl = info.Controls();
            next.canNext = ctl.IsNextEnabled();
            next.canPrev = ctl.IsPreviousEnabled();
            next.canSeek = ctl.IsPlaybackPositionEnabled();
            next.canToggle = ctl.IsPlayPauseToggleEnabled() || ctl.IsPlayEnabled() || ctl.IsPauseEnabled();
            double rate = 1.0;
            if (auto r = info.PlaybackRate()) rate = r.Value();
            next.rate = rate > 0.05 && rate < 8 ? rate : 1.0;

            const bool srcChanged = chosenAumid_ != prevAumid;
            // pollTick_: players that never raise MediaPropertiesChanged still get their new track read.
            if (propsDirty_ || forceRead_ || pollTick_ || srcChanged || lastKey_.empty() || cur_.title.empty() || artRetryAt_ ||
                next.playing != cur_.playing) {
                propsDirty_ = false;
                auto pop = sess->TryGetMediaPropertiesAsync();
                if (!AwaitOp(pop)) throw std::runtime_error("props");
                auto props = pop.GetResults();
                next.title = props.Title().c_str();
                next.artist = props.Artist().c_str();
                next.album = props.AlbumTitle().c_str();
                if (next.title.empty() && LowerW(chosenAumid_).find(L"vlc") != std::wstring::npos) next.title = VlcCaption();
                const std::wstring key = chosenAumid_ + L"\n" + next.title + L"\n" + next.artist + L"\n" + next.album;
                const bool newTrack = key != lastKey_;
                if (newTrack) {
                    next.art = nullptr;  // Never keep old art across a real track change.
                    artHash_ = 0;
                    artRetryAt_ = now + 1500;
                    artTries_ = 0;
                }
                // Art: replaced only when the bytes differ (bug 5). Retry at +1.5 s and +3 s for slow players.
                if (newTrack || (artRetryAt_ && now >= artRetryAt_)) {
                    std::shared_ptr<const MArt> a = next.art;
                    size_t hsh = artHash_;
                    bool got = false;
                    try { got = ReadArt(props, a, hsh); } catch (...) {}
                    if (got) { next.art = a; artHash_ = hsh; }
                    if (artTries_ < 2) { artRetryAt_ = now + 1500; artTries_++; } else artRetryAt_ = 0;
                }
                // Auto-expand trigger. Skipped on the first read, after a user switch, and for resume-from-pause (bug 2).
                const std::wstring tkey = next.title + L"\n" + next.artist;
                if (newTrack && !firstRead_ && now >= userSwitchUntil_ && !next.title.empty() && next.playing &&
                    tkey != lastTrackKey_) {
                    next.trackSeq++;
                }
                lastTrackKey_ = tkey;
                lastKey_ = key;
                forceRead_ = false;
            } else if (artRetryAt_ && now >= artRetryAt_) {
                artRetryAt_ = 0;
            }
            // Timeline.
            if (now >= ignoreTimelineUntil_) {
                auto tl = sess->GetTimelineProperties();
                using namespace std::chrono;
                curStartMs_ = duration_cast<milliseconds>(tl.StartTime()).count();
                const long long endMs = duration_cast<milliseconds>(tl.EndTime()).count();
                next.durMs = std::max(0LL, endMs - curStartMs_);  // Bug 13: duration is end - start.
                const long long rawPos = duration_cast<milliseconds>(tl.Position()).count();
                const long long updated = tl.LastUpdatedTime().time_since_epoch().count();
                long long pos = rawPos - curStartMs_;
                const long long age = duration_cast<milliseconds>(winrt::clock::now() - tl.LastUpdatedTime()).count();
                if (next.playing && age >= 0 && age < 10000) pos += (long long)(age * next.rate);
                pos = std::max(0LL, std::min(pos, next.durMs > 0 ? next.durMs : pos));
                // Spotify refreshes LastUpdatedTime without moving Position: hold a backward jump until confirmed.
                const long long expected = cur_.posMs + (cur_.playing ? (long long)((now - cur_.baseTick) * cur_.rate) : 0);
                const bool sameTrack = next.title == cur_.title && next.aumid == cur_.aumid;
                // Some players publish the timeline once and never again: neither Position nor LastUpdatedTime moves.
                // Re-basing on that stale reading would snap the thumb back each poll, so keep extrapolating locally.
                const bool frozen = sameTrack && next.playing && cur_.playing && cur_.baseTick &&
                                    rawPos == lastRawPos_ && updated == lastUpdated_;
                lastRawPos_ = rawPos;
                lastUpdated_ = updated;
                if (frozen) {
                    next.posMs = cur_.posMs;
                    next.baseTick = cur_.baseTick;
                } else if (sameTrack && next.playing && cur_.playing && pos < expected - 1500 && !backPending_) {
                    backPending_ = true;  // keep the old base
                    next.posMs = cur_.posMs;
                    next.baseTick = cur_.baseTick;
                } else {
                    backPending_ = false;
                    next.posMs = pos;
                    next.baseTick = now;
                }
            }
        } catch (...) {
            readOk = false;  // Bug 3: keep the last snapshot instead of publishing an empty one.
        }
        if (!readOk) {
            // The session may have been dropped and recreated under the same id: rebuild, and after
            // several failures in a row ask for a fresh manager.
            forceRebuild_ = true;
            if (++readFails_ >= 3) { readFails_ = 0; DropManager(); }
            cur_.sources = next.sources;
            cur_.active = next.active;
            Publish(false);
            return;
        }
        readFails_ = 0;
        if (next.playing) lastPlayingTick_ = now;
        firstRead_ = false;
        cur_ = std::move(next);
        Publish(false);
    }

    // Posts to the UI only when something visible changed, or when the panel is open (it shows the position).
    void Publish(bool force) {
        auto snap = std::make_shared<MSnap>(cur_);
        bool visible = force;
        {
            std::lock_guard<std::mutex> g(mu_);
            const MSnap& o = *snap_;
            visible = visible || o.has != snap->has || o.playing != snap->playing || o.title != snap->title ||
                      o.artist != snap->artist || o.album != snap->album || o.art != snap->art || o.aumid != snap->aumid ||
                      o.sources.size() != snap->sources.size() || o.active != snap->active || o.trackSeq != snap->trackSeq ||
                      o.durMs != snap->durMs || o.canNext != snap->canNext || o.canPrev != snap->canPrev ||
                      o.canSeek != snap->canSeek || o.rate != snap->rate;
            if (!visible) {
                for (size_t i = 0; i < o.sources.size(); i++)
                    if (o.sources[i].playing != snap->sources[i].playing || o.sources[i].icon != snap->sources[i].icon) visible = true;
            }
            snap_ = snap;
        }
        if (visible || panelOpen_) {
            auto alive = alive_;
            auto fn = notify_;
            g_services.Post([alive, fn] { if (*alive) fn(); });
        }
    }

    std::function<void()> notify_;
    HANDLE stop_ = nullptr, wake_ = nullptr, thread_ = nullptr;
    std::mutex mu_;
    std::shared_ptr<const MSnap> snap_;
    std::deque<MCmd> cmds_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::atomic<bool> panelOpen_{false}, shown_{false};
    std::atomic<bool> sessionsDirty_{true}, propsDirty_{false};

    // Media thread only.
    wm::GlobalSystemMediaTransportControlsSessionManager mgr_{nullptr};
    wm::GlobalSystemMediaTransportControlsSessionManager::SessionsChanged_revoker mgrSess_;
    wm::GlobalSystemMediaTransportControlsSessionManager::CurrentSessionChanged_revoker mgrCur_;
    std::vector<SessRec> recs_;
    std::unordered_map<std::wstring, SrcState> srcs_;
    std::vector<std::wstring> order_;
    std::wstring chosenAumid_, dockPick_, lastKey_, artKey_, lastTrackKey_;
    unsigned startCounter_ = 0, startedBefore_ = 0;
    MSnap cur_;
    bool firstRead_ = true, forceRead_ = false, forceRebuild_ = false, backPending_ = false, pollTick_ = false;
    size_t artHash_ = 0;
    int artTries_ = 0, readFails_ = 0, mgrFails_ = 0;
    ULONGLONG artRetryAt_ = 0, userSwitchUntil_ = 0, ignoreTimelineUntil_ = 0, lastPlayingTick_ = 0;
    long long curStartMs_ = 0, lastRawPos_ = -1, lastUpdated_ = 0;
};
