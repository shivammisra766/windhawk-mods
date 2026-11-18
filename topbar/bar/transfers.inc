// ---- PCTracker transfers: files moving between the phone and the PC, in the pill and a page ----
// The PCTracker agent publishes every transfer it runs, whichever way it was started (Explorer's or this
// bar's "Send to phone", Browse from the phone, the phone's share sheet) and whichever way it travels
// (WebRTC or LAN), to %LOCALAPPDATA%\PCTracker\status\transfers.tsv. pc_agent/transfer_status.py has
// the format. A worker waits on a change notification for that folder (nothing polls) and hands each
// new snapshot to the UI thread.
// Collapsed, a running transfer owns the pill; its outcome stays a few seconds. Expanded, a page (first
// while any transfer shows) lists them with Pause/Resume and Cancel, as far as the agent allows ("caps").
// A button drops a small request in %LOCALAPPDATA%\PCTracker\control\, which the agent picks up.
// A one-second module timer runs only while something shows.

struct XferEntry {
    std::wstring id, name, error, caps;
    bool up = true;   // PC -> phone
    int state = 0;    // XS_*
    double done = 0, total = 0;
    int count = 1, index = 0;
    ULONGLONG updated = 0, started = 0;  // Wall clock, ms.
};
enum XferState { XS_Waiting, XS_Active, XS_Paused, XS_Done, XS_Failed, XS_Cancelled };
bool XferLive(int s) { return s == XS_Waiting || s == XS_Active || s == XS_Paused; }
struct XferSnap {
    ULONGLONG written = 0;  // Wall clock, ms; 0: no file (no agent, or not running).
    std::vector<XferEntry> items;
};

constexpr ULONGLONG kXferStaleMs = 10000;   // The agent rewrites every 2 s while a transfer is live.
constexpr ULONGLONG kXferOutcomeMs = 6000;  // How long a finished transfer stays (the agent keeps 8 s).
constexpr ULONGLONG kXferPendingMs = 4000;  // "Pausing…" after a click, until the agent's answer shows.
constexpr float kXferPillW = 250.0f;
constexpr int kXferRows = 3;

ULONGLONG WallMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ULL) / 10000ULL;
}

const std::wstring& XferBase() {
    static std::wstring dir;
    if (dir.empty()) {
        wchar_t buf[MAX_PATH] = {};
        GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        dir = std::wstring(buf) + L"\\PCTracker";
    }
    return dir;
}
std::wstring XferDir() { return XferBase() + L"\\status"; }

// The agent replaces the file with os.replace: share delete as well, or a read could block its write.
bool ReadXferFile(std::string& out) {
    out.clear();
    const std::wstring path = XferDir() + L"\\transfers.tsv";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(f, &sz) && sz.QuadPart <= (1 << 20);
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        DWORD n = 0;
        ok = ReadFile(f, out.empty() ? nullptr : &out[0], (DWORD)out.size(), &n, nullptr) && n == out.size();
    }
    CloseHandle(f);
    return ok;
}

// Asks the agent to pause, resume or cancel transfer `id`: {"ts","id","action"} as a .json in control\,
// written to a temp name and renamed so the agent never reads half a file.
bool XferControl(const std::wstring& id, const char* action) {
    std::string safe;
    for (wchar_t ch : id) {
        if (!((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_')) return false;
        safe += (char)ch;
    }
    if (safe.empty() || safe.size() > 80) return false;
    const std::wstring dir = XferBase() + L"\\control";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    char json[256];
    sprintf_s(json, "{\"ts\": %llu, \"id\": \"%s\", \"action\": \"%s\"}", WallMs() / 1000, safe.c_str(), action);
    static unsigned seq = 0;
    const std::wstring path = dir + L"\\" + std::to_wstring(WallMs()) + L"_" + std::to_wstring(++seq) + L".json";
    return WriteFileAtomic(path, json);
}

std::vector<std::wstring> SplitOn(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t at = 0;
    for (;;) {
        const size_t next = s.find(sep, at);
        out.push_back(s.substr(at, next == std::wstring::npos ? std::wstring::npos : next - at));
        if (next == std::wstring::npos) break;
        at = next + 1;
    }
    return out;
}

XferSnap ParseXfer(const std::string& data) {
    XferSnap snap;
    const std::vector<std::wstring> lines = SplitOn(Wide(data), L'\n');
    if (lines.empty()) return snap;
    const std::vector<std::wstring> head = SplitOn(lines[0], L'\t');
    if (head.size() < 2 || head[0] != L"v1") return snap;  // A format this bar does not know: show nothing.
    snap.written = _wcstoui64(head[1].c_str(), nullptr, 10);
    for (size_t i = 1; i < lines.size(); i++) {
        const std::vector<std::wstring> c = SplitOn(lines[i], L'\t');
        if (c.size() < 11) continue;
        XferEntry e;
        e.id = c[0];
        e.up = c[1] == L"up";
        const std::wstring& st = c[2];
        e.state = st == L"active" ? XS_Active : st == L"paused" ? XS_Paused : st == L"done" ? XS_Done
                : st == L"failed" ? XS_Failed : st == L"cancelled" ? XS_Cancelled : XS_Waiting;
        e.done = _wtof(c[3].c_str());
        e.total = _wtof(c[4].c_str());
        e.count = std::max(1, _wtoi(c[5].c_str()));
        e.index = std::max(0, _wtoi(c[6].c_str()));
        e.updated = _wcstoui64(c[7].c_str(), nullptr, 10);
        e.started = _wcstoui64(c[8].c_str(), nullptr, 10);
        e.name = c[9];
        e.error = c[10];
        if (c.size() > 11) e.caps = c[11];  // Older agents: no controls.
        snap.items.push_back(std::move(e));
    }
    return snap;
}

std::wstring XferMB(double bytes) {
    wchar_t buf[32];
    if (bytes >= 1024.0 * 1024 * 1024) swprintf_s(buf, L"%.2f GB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024.0 * 1024) swprintf_s(buf, L"%.1f MB", bytes / (1024.0 * 1024));
    else swprintf_s(buf, L"%.0f KB", bytes / 1024.0);
    return buf;
}

const D2D1_COLOR_F kXferGreen = Rgb(48, 209, 88);

// The round badge: a progress ring around an arrow (up = to the phone), a tick, or a cross.
void DrawXferBadge(const Pg& p, const XferEntry& e, float cx, float cy, float r) {
    const D2D1_COLOR_F accent = e.up ? Rgb(10, 132, 255) : kXferGreen;
    const float frac = e.state == XS_Done ? 1.0f : e.total > 0 ? (float)std::min(e.done / e.total, 1.0) : 0.0f;
    const float k = r / 8.0f;  // Glyphs are drawn for r = 8.
    p.Ring(cx, cy, r, 2, kHairline);
    if (e.state == XS_Done) DrawArcFrac(p, cx, cy, r, 1.0f, 2, kXferGreen);
    else if (e.state == XS_Failed) DrawArcFrac(p, cx, cy, r, 1.0f, 2, kRed);
    else if (e.state == XS_Active) DrawArcFrac(p, cx, cy, r, std::max(frac, 0.02f), 2, accent);
    else if (e.state == XS_Paused) DrawArcFrac(p, cx, cy, r, std::max(frac, 0.02f), 2, kAmber);
    if (e.state == XS_Done) {
        p.Line(cx - 3.5f * k, cy + 0.2f * k, cx - 1.0f * k, cy + 2.8f * k, 1.6f, kFg);
        p.Line(cx - 1.0f * k, cy + 2.8f * k, cx + 3.8f * k, cy - 2.6f * k, 1.6f, kFg);
    } else if (e.state == XS_Failed || e.state == XS_Cancelled) {
        const float a = e.state == XS_Cancelled ? 0.6f : 1.0f;
        p.Line(cx - 2.8f * k, cy - 2.8f * k, cx + 2.8f * k, cy + 2.8f * k, 1.6f, kFg, a);
        p.Line(cx - 2.8f * k, cy + 2.8f * k, cx + 2.8f * k, cy - 2.8f * k, 1.6f, kFg, a);
    } else {
        const float dir = e.up ? -1.0f : 1.0f;  // Screen y grows downwards.
        const float a = e.state == XS_Active ? 1.0f : 0.6f;
        p.Line(cx, cy - 3.8f * k * dir, cx, cy + 3.8f * k * dir, 1.5f, kFg, a);
        p.Line(cx, cy + 3.8f * k * dir, cx - 3.0f * k, cy + 0.8f * k * dir, 1.5f, kFg, a);
        p.Line(cx, cy + 3.8f * k * dir, cx + 3.0f * k, cy + 0.8f * k * dir, 1.5f, kFg, a);
    }
}

HANDLE g_xfStop = nullptr;
DWORD WINAPI XferThread(void*);

class TransferModule : public PageBase {
public:
    const wchar_t* Id() const override { return L"pctracker-transfers"; }
    // Above the timer (50) and media (10): a file on the move is what the user is waiting on.
    int PillPriority() const override { return shown_ ? 60 : 0; }
    // A page only while there is something to show, and then the first one the island opens on.
    bool HasDashboard() const override { return shown_; }
    int DashboardOrder() const override { return 5; }
    Size2 Measure(View v) override { return v == View::Pill ? Size2{kXferPillW, kNotchH} : Size2{kPageW, kPageH}; }

    void Start(Services& s) override {
        sv_ = &s;
        g_xfStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        thread_ = CreateThread(nullptr, 0, XferThread, nullptr, 0, nullptr);
    }
    void Stop() override {
        if (sv_) sv_->KillTimer(this);
        if (g_xfStop) SetEvent(g_xfStop);
        if (thread_) {
            WaitForSingleObject(thread_, 3000);
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        if (g_xfStop) CloseHandle(g_xfStop);
        g_xfStop = nullptr;
    }
    bool OnPointer(const PointerEvent& e, View v) override {
        if (v != View::Dashboard) return false;  // The pill has no buttons; its hit list belongs to the page.
        return PageBase::OnPointer(e, v);
    }

    // UI thread: a new snapshot from the worker.
    void Apply(XferSnap snap) {
        snap_ = std::move(snap);
        Update();
    }

    void Draw(const DrawCtx& c) override {
        if (!shown_) return;
        if (c.view == View::Pill) DrawPill(c);
        else DrawPage(c);
    }

protected:
    void Click(int id) override {
        const int row = (id - 100) / 10, what = (id - 100) % 10;
        if (row < 0 || row >= (int)rows_.size()) return;
        const XferEntry& e = rows_[row];
        const char* action = what == 2 ? "cancel" : e.state == XS_Paused ? "resume" : "pause";
        if (XferControl(e.id, action)) {
            pendingId_ = e.id;
            pendingAction_ = what == 2 ? 2 : e.state == XS_Paused ? 1 : 0;
            pendingState_ = e.state;
            pendingAt_ = GetTickCount64();
            sv_->Invalidate();
        }
    }

private:
    // Status text for an entry: what it is doing, its progress, or how it ended.
    std::wstring Detail(const XferEntry& e, bool pill) const {
        wchar_t buf[96];
        if (Pending(e)) {
            return pendingAction_ == 2 ? Loc(L"xfer.cancelling") : pendingAction_ == 1 ? Loc(L"xfer.resuming") : Loc(L"xfer.pausing");
        }
        const int pct = e.total > 0 ? (int)floor(std::min(e.done / e.total, 1.0) * 100.0) : 0;
        switch (e.state) {
            case XS_Active:
                if (pill) {
                    if (e.count > 1) swprintf_s(buf, L"%d/%d  %d%%", std::min(e.index + 1, e.count), e.count, pct);
                    else swprintf_s(buf, L"%d%%", pct);
                    return buf;
                }
                swprintf_s(buf, L"%s of %s  ·  %d%%", XferMB(e.done).c_str(), XferMB(e.total).c_str(), pct);
                return (e.count > 1 ? std::to_wstring(std::min(e.index + 1, e.count)) + L"/" + std::to_wstring(e.count) + L"  ·  " : L"") + buf;
            case XS_Paused:
                if (pill) return Loc(L"xfer.paused");
                swprintf_s(buf, L"%s  ·  %s of %s", Loc(L"xfer.paused"), XferMB(e.done).c_str(), XferMB(e.total).c_str());
                return buf;
            case XS_Waiting: {
                // `updated` only moves on a change of state or bytes: for a waiting entry, when the wait began.
                const ULONGLONG now = WallMs();
                const ULONGLONG secs = now > e.updated ? (now - e.updated) / 1000 : 0;
                swprintf_s(buf, L"%llu:%02llu", secs / 60, secs % 60);
                if (pill) return buf;
                return std::wstring(WaitText(e)) + L"  ·  " + buf;
            }
            case XS_Done: return e.up ? Loc(L"xfer.sent") : Loc(L"xfer.received");
            case XS_Cancelled: return Loc(L"xfer.cancelled");
            default: return pill || e.error.empty() ? Loc(L"xfer.failed") : std::wstring(Loc(L"xfer.failed")) + L": " + e.error;
        }
    }
    const wchar_t* WaitText(const XferEntry& e) const {
        return e.done > 0 ? Loc(L"xfer.reconnecting") : e.up ? Loc(L"xfer.waitphone") : Loc(L"xfer.waiting");
    }
    bool Pending(const XferEntry& e) const {
        return e.id == pendingId_ && e.state == pendingState_ && GetTickCount64() < pendingAt_ + kXferPendingMs;
    }
    D2D1_COLOR_F DetailColor(const XferEntry& e) const {
        if (Pending(e)) return kMuted;
        return e.state == XS_Done ? kXferGreen : e.state == XS_Failed ? kRed : e.state == XS_Paused ? kAmber : kMuted;
    }

    void DrawPill(const DrawCtx& c) {
        const XferEntry& e = rows_.front();
        Pg p(c);
        const float h = (c.rect.bottom - c.rect.top) / c.scale;
        DrawXferBadge(p, e, 20.0f, h / 2, 8.0f);
        std::wstring right = Detail(e, true), main = e.name;
        if (e.state == XS_Waiting && !Pending(e)) main = WaitText(e);
        if (e.state == XS_Failed && !e.error.empty()) main = e.error;
        const int others = live_ - (XferLive(e.state) ? 1 : 0);
        if (others > 0 && XferLive(e.state)) right += L"  +" + std::to_wstring(others);
        const float w = (c.rect.right - c.rect.left) / c.scale;
        const float rw = TextWidth(right, 12, 600) + 2.0f;
        p.Text(main, 36, 0, w - 14 - rw - 8, h, 13, kFg, TF_Ellipsis, 600);
        p.Text(right, w - 14 - rw, 0, w - 12, h, 12, DetailColor(e), TF_Right, 600);
    }

    void DrawPage(const DrawCtx& c) {
        Pg p(c);
        hits_.clear();
        std::wstring title = Loc(L"xfer.title");
        if ((int)snapLive() > kXferRows) title += L"  ·  " + std::to_wstring(snapLive());
        p.Text(title, 20, 12, 300, 32, 13.5f, kFg, TF_Ellipsis, 600);
        const float rowH = 44.0f, gap = 4.0f, left = 12.0f, right = kPageW - 24.0f;
        for (int i = 0; i < (int)rows_.size() && i < kXferRows; i++) {
            const XferEntry& e = rows_[i];
            const float t = 38.0f + i * (rowH + gap), b = t + rowH;
            p.Fill(left, t, right, b, 10, kTile);
            DrawXferBadge(p, e, left + 22, (t + b) / 2, 11.0f);

            // Buttons, right to left: cancel, then pause/resume.
            const bool live = XferLive(e.state) && !Pending(e);
            float textRight = right - 10;
            const float br = 12.0f, cy = (t + b) / 2;
            auto button = [&](float cx, int id, auto glyph) {
                const bool hot = hover_ == id;
                p.Dot(cx, cy, br, hot ? kTileRaised : kTileHover);
                glyph(cx);
                Hit(cx - br, cy - br, cx + br, cy + br, id);
            };
            float bx = right - 10 - br;
            if (live && e.caps.find(L'c') != std::wstring::npos) {
                button(bx, 100 + i * 10 + 2, [&](float x) {
                    p.Line(x - 3.6f, cy - 3.6f, x + 3.6f, cy + 3.6f, 1.6f, kFg);
                    p.Line(x - 3.6f, cy + 3.6f, x + 3.6f, cy - 3.6f, 1.6f, kFg);
                });
                textRight = bx - br - 8;
                bx -= 2 * br + 8;
            }
            if (live && e.caps.find(L'p') != std::wstring::npos) {
                const bool paused = e.state == XS_Paused;
                button(bx, 100 + i * 10 + 1, [&](float x) {
                    if (paused) {  // Play: a small triangle, outlined.
                        p.Line(x - 2.5f, cy - 4.2f, x - 2.5f, cy + 4.2f, 1.6f, kFg);
                        p.Line(x - 2.5f, cy - 4.2f, x + 4.2f, cy, 1.6f, kFg);
                        p.Line(x - 2.5f, cy + 4.2f, x + 4.2f, cy, 1.6f, kFg);
                    } else {       // Pause: two bars.
                        p.Line(x - 2.4f, cy - 4.2f, x - 2.4f, cy + 4.2f, 2.0f, kFg);
                        p.Line(x + 2.4f, cy - 4.2f, x + 2.4f, cy + 4.2f, 2.0f, kFg);
                    }
                });
                textRight = bx - br - 8;
            }

            const float tx = left + 42;
            p.Text(e.name, tx, t + 5, textRight, t + 23, 13, kFg, TF_Ellipsis, 600);
            p.Text(Detail(e, false), tx, t + 23, textRight, t + 39, 11, DetailColor(e), TF_Ellipsis);
            if (XferLive(e.state) && e.total > 0) {  // A hairline progress track along the row's foot.
                const float frac = (float)std::min(e.done / e.total, 1.0);
                p.Line(tx, b - 4, textRight, b - 4, 2.0f, kHairline);
                if (frac > 0.002f) p.Line(tx, b - 4, tx + (textRight - tx) * frac, b - 4, 2.0f,
                                          e.state == XS_Paused ? kAmber : e.up ? Rgb(10, 132, 255) : kXferGreen);
            }
        }
    }

    size_t snapLive() const { return (size_t)live_ + outcomes_; }

    // What shows: live transfers (oldest first, so rows keep their places) then outcomes still within
    // kXferOutcomeMs (newest first). The pill shows the most recently moving live one, else the newest
    // outcome. Live entries in a stale file are ignored: the agent stopped (or crashed) mid-transfer.
    void Update() {
        const ULONGLONG now = WallMs();
        const bool fresh = snap_.written && now < snap_.written + kXferStaleMs;
        std::vector<XferEntry> live, done;
        for (const XferEntry& e : snap_.items) {
            if (XferLive(e.state)) { if (fresh) live.push_back(e); }
            else if (now < e.updated + kXferOutcomeMs) done.push_back(e);
        }
        std::stable_sort(live.begin(), live.end(), [](const XferEntry& a, const XferEntry& b) { return a.started < b.started; });
        std::stable_sort(done.begin(), done.end(), [](const XferEntry& a, const XferEntry& b) { return a.updated > b.updated; });
        // The pill's entry goes first: the live one that moved last, or (none live) the newest outcome.
        std::vector<XferEntry> rows = live;
        rows.insert(rows.end(), done.begin(), done.end());
        if (!live.empty()) {
            auto mover = std::max_element(rows.begin(), rows.begin() + live.size(), [](const XferEntry& a, const XferEntry& b) {
                const int ra = a.state == XS_Active ? 1 : 0, rb = b.state == XS_Active ? 1 : 0;
                return ra != rb ? ra < rb : a.updated < b.updated;
            });
            std::rotate(rows.begin(), mover, mover + 1);
        }
        const bool wasShown = shown_;
        const int wasState = rows_.empty() ? -1 : rows_.front().state;
        const size_t wasCount = rows_.size();
        rows_ = std::move(rows);
        live_ = (int)live.size();
        outcomes_ = (int)done.size();
        shown_ = !rows_.empty();
        if (!sv_) return;
        if (shown_) sv_->SetTimer(this, 1000, [this] { Update(); });  // Elapsed wait, staleness, outcome expiry.
        else sv_->KillTimer(this);
        if (shown_ != wasShown || (shown_ && (rows_.front().state != wasState || rows_.size() != wasCount))) sv_->Refresh();
        else if (shown_) sv_->Invalidate();
    }

    HANDLE thread_ = nullptr;
    XferSnap snap_;
    std::vector<XferEntry> rows_;  // rows_[0] is the pill's.
    bool shown_ = false;
    int live_ = 0, outcomes_ = 0;
    std::wstring pendingId_;
    int pendingAction_ = 0, pendingState_ = -1;  // 0 pause, 1 resume, 2 cancel
    ULONGLONG pendingAt_ = 0;
};

TransferModule g_transferModule;
IModule* TransfersModuleInstance() { return &g_transferModule; }

// Worker: re-reads the status file whenever its folder changes, and hands changed contents to the UI
// thread. Until the folder exists (no PCTracker yet) it looks again every 30 s.
DWORD WINAPI XferThread(void*) {
    std::string last = "\x01";  // Never equal to a real read, so the first one always posts.
    auto readAndPost = [&] {
        std::string data;
        ReadXferFile(data);  // A missing file reads as empty: nothing to show.
        if (data == last) return;
        last = data;
        XferSnap snap = ParseXfer(data);
        g_services.Post([snap = std::move(snap)]() mutable { g_transferModule.Apply(std::move(snap)); });
    };
    for (;;) {
        HANDLE watch = FindFirstChangeNotificationW(XferDir().c_str(), FALSE,
                                                    FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
        if (watch == INVALID_HANDLE_VALUE) {
            if (WaitForSingleObject(g_xfStop, 30000) == WAIT_OBJECT_0) return 0;
            continue;
        }
        readAndPost();
        HANDLE h[2] = {g_xfStop, watch};
        bool stop = false, lost = false;
        while (!stop && !lost) {
            const DWORD w = WaitForMultipleObjects(2, h, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0 || w == WAIT_FAILED) stop = true;
            else {
                // os.replace is a delete plus a rename: let it land before reading.
                if (WaitForSingleObject(g_xfStop, 30) == WAIT_OBJECT_0) stop = true;
                else {
                    readAndPost();
                    lost = !FindNextChangeNotification(watch);  // The folder was deleted: watch it again.
                }
            }
        }
        FindCloseChangeNotification(watch);
        if (stop) return 0;
    }
}
