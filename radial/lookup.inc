// ============================================================================
// Lookup (tool process): three-finger tap -> macOS-style word popover
// ============================================================================
//
// A three-finger tap looks up the text under the pointer: the selection if
// the pointer is on it, otherwise the word. The word comes from UI Automation
// (the app's own text), or from OCR of the screen around the pointer when the
// app doesn't expose its text. The popover shows WordNet (offline), a
// Wikipedia summary, and what the word means in its sentence from the local
// model (ollama, started on demand, model unloaded after every answer).
//
// Work runs on worker threads that hold a reference to this DLL, so an unload
// in the middle of a lookup can't pull the code out from under them. Results
// come back to the UI thread as WM_RS_LOOKUP_RESULT; stale ones (an older
// generation) are dropped.

constexpr wchar_t kLookupWordNet[] = L"D:\\lookup\\wordnet.tsv";
constexpr wchar_t kLookupOllamaExe[] = L"D:\\ai\\bin\\ollama\\ollama.exe";
constexpr char kLookupModel[] = "qwen3:1.7b";
constexpr wchar_t kLookupClassName[] = L"RadialLookupPopover";

constexpr ULONGLONG kTapMaxMs = 280;   // all three fingers down and up within
constexpr double kTapMaxMove = 0.03;   // of the pad width, while three touch

static std::atomic<bool> g_lookupQuit{false};

// A short log at D:\lookup\lookup.log (also sent to the Windhawk log).
static void LkLog(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, ARRAYSIZE(buf) - 1, fmt, ap);
    va_end(ap);
    buf[ARRAYSIZE(buf) - 1] = 0;
    Wh_Log(L"%s", buf);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[1100];
    int n = _snwprintf(line, ARRAYSIZE(line) - 1, L"%02d:%02d:%02d.%03d %s\r\n", t.wHour,
                       t.wMinute, t.wSecond, t.wMilliseconds, buf);
    if (n < 0) return;
    std::string u;
    int m = WideCharToMultiByte(CP_UTF8, 0, line, n, nullptr, 0, nullptr, nullptr);
    u.resize(m);
    WideCharToMultiByte(CP_UTF8, 0, line, n, u.data(), m, nullptr, nullptr);
    static std::mutex lock;
    std::lock_guard<std::mutex> g(lock);
    HANDLE f = CreateFileW(L"D:\\lookup\\lookup.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz{};
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 512 * 1024) {
        CloseHandle(f);  // keep it small: start over
        f = CreateFileW(L"D:\\lookup\\lookup.log", GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
    }
    DWORD w = 0;
    WriteFile(f, u.data(), (DWORD)u.size(), &w, nullptr);
    CloseHandle(f);
}

static std::wstring LkUtf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static std::string LkWideToUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0,
                                nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n,
                        nullptr, nullptr);
    return s;
}

static bool LkIsDevanagari(wchar_t c) { return c >= 0x0900 && c <= 0x097F; }

static bool LkHasDevanagari(const std::wstring& s) {
    for (wchar_t c : s) {
        if (LkIsDevanagari(c)) return true;
    }
    return false;
}

// A letter outside the Latin script (punctuation, symbols and emoji don't count).
static bool LkIsNonLatinLetter(wchar_t c) {
    if (c < 0x0370) return false;
    if (c >= 0x2000 && c <= 0x2BFF) return false;  // punctuation, symbols, arrows
    if (c >= 0x3000 && c <= 0x303F) return false;  // CJK punctuation
    if (c >= 0xD800 && c <= 0xDFFF) return false;  // surrogates (emoji)
    if (c >= 0xFE00 && c <= 0xFE6F) return false;  // variation selectors, small forms
    if (c >= 0xFF00 && c <= 0xFF0F) return false;
    if (c == 0x0964 || c == 0x0965) return false;  // Devanagari danda
    return true;
}

static bool LkHasNonLatin(const std::wstring& s) {
    for (wchar_t c : s) {
        if (LkIsNonLatinLetter(c)) return true;
    }
    return false;
}

static int LkWordCount(const std::wstring& s) {
    int n = 0;
    bool in = false;
    for (wchar_t c : s) {
        bool sp = iswspace(c) || c == 0xA0;
        if (!sp && !in) n++;
        in = !sp;
    }
    return n;
}

// What the local model is asked, chosen from the text that was found:
// Word: an English (Latin-script) word or short phrase, "in this sentence".
// Native: a Hindi (Devanagari) word or short phrase, its English meaning plus
//         "in this sentence".
// Phrase: more than 4 words, or another script: an English translation and
//         what it means. No dictionary or Wikipedia for these.
enum class LkMode { Word, Native, Phrase };

static LkMode LkModeFor(const std::wstring& t) {
    if (LkWordCount(t) > 4) return LkMode::Phrase;
    if (!LkHasNonLatin(t)) return LkMode::Word;
    bool other = false;
    for (wchar_t c : t) {
        if (LkIsNonLatinLetter(c) && !LkIsDevanagari(c)) other = true;
    }
    return other ? LkMode::Phrase : LkMode::Native;
}

static bool LkWordChar(wchar_t c) {
    return iswalnum(c) || LkIsDevanagari(c) || c == L'\'' || c == L'-' ||
           c == 0x2019 || (c >= 0x00C0 && c <= 0x024F);
}

static std::wstring LkCollapseSpace(const std::wstring& s) {
    std::wstring o;
    bool sp = false;
    for (wchar_t c : s) {
        if (iswspace(c) || c == 0xA0) {
            sp = true;
            continue;
        }
        if (sp && !o.empty()) o += L' ';
        sp = false;
        o += c;
    }
    return o;
}

// Trims punctuation and quotes around a word or phrase, and a possessive 's.
static std::wstring LkCleanWord(std::wstring w) {
    w = LkCollapseSpace(w);
    size_t a = 0, b = w.size();
    auto keep = [](wchar_t c) {
        return iswalnum(c) || LkIsNonLatinLetter(c) || (c >= 0x00C0 && c <= 0x024F);
    };
    while (a < b && !keep(w[a])) a++;
    while (b > a && !keep(w[b - 1])) b--;
    w = w.substr(a, b - a);
    if (w.size() > 2) {
        size_t n = w.size();
        if ((w[n - 2] == L'\'' || w[n - 2] == 0x2019) &&
            (w[n - 1] == L's' || w[n - 1] == L'S')) {
            w.resize(n - 2);
        }
    }
    return w;
}

// The sentence around [off, off+len) in a paragraph.
static std::wstring LkSentenceAround(const std::wstring& p, size_t off,
                                     size_t len) {
    if (off > p.size()) off = p.size();
    auto isEnd = [](wchar_t c) {
        return c == L'.' || c == L'!' || c == L'?' || c == 0x0964 ||
               c == L'\n' || c == L'\r';
    };
    size_t a = off;
    while (a > 0 && !isEnd(p[a - 1]) && off - a < 300) a--;
    size_t b = std::min(p.size(), off + len);
    while (b < p.size() && !isEnd(p[b]) && b - off < 300) b++;
    if (b < p.size()) b++;
    return LkCollapseSpace(p.substr(a, b - a));
}

// ---------------------------------------------------------------------------
// Finding the word: UI Automation
// ---------------------------------------------------------------------------

struct LkHit {
    std::wstring word, context;
    RECT rect{};
    bool fromSelection = false;
};

static const CLSID kLkClsidCUIAutomation = {
    0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
static const IID kLkIidIUIAutomation = {
    0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};
static const IID kLkIidIUIAutomation2 = {
    0x34723aff, 0x0c9d, 0x49d0, {0x98, 0x96, 0x7a, 0xb5, 0x2d, 0xf8, 0xcd, 0x8a}};
static const IID kLkIidTextPattern = {
    0x32eba289, 0x3583, 0x42c9, {0x9c, 0x59, 0x3b, 0x6d, 0x9a, 0x1e, 0x9b, 0x6a}};
constexpr PATTERNID kLkTextPatternId = 10014;

static std::wstring LkBstr(BSTR b) {
    std::wstring s = b ? std::wstring(b, SysStringLen(b)) : std::wstring();
    if (b) SysFreeString(b);
    return s;
}

// True if a rectangle in the array (x, y, w, h groups) contains the point;
// `out` receives the union of all of them.
static bool LkRectsContain(SAFEARRAY* sa, POINT pt, int tol, RECT* out) {
    if (!sa) return false;
    LONG lb = 0, ub = -1;
    SafeArrayGetLBound(sa, 1, &lb);
    SafeArrayGetUBound(sa, 1, &ub);
    LONG n = ub - lb + 1;
    double* d = nullptr;
    bool hit = false;
    if (n >= 4 && SUCCEEDED(SafeArrayAccessData(sa, (void**)&d))) {
        RECT u{};
        bool any = false;
        for (LONG i = 0; i + 3 < n; i += 4) {
            RECT r{(LONG)d[i], (LONG)d[i + 1], (LONG)(d[i] + d[i + 2]),
                   (LONG)(d[i + 1] + d[i + 3])};
            if (r.right <= r.left || r.bottom <= r.top) continue;
            if (!any) {
                u = r;
                any = true;
            } else {
                UnionRect(&u, &u, &r);
            }
            if (pt.x >= r.left - tol && pt.x <= r.right + tol &&
                pt.y >= r.top - tol && pt.y <= r.bottom + tol) {
                hit = true;
            }
        }
        SafeArrayUnaccessData(sa);
        if (hit && out) *out = u;
    }
    SafeArrayDestroy(sa);
    return hit;
}

static std::wstring LkParagraphContext(IUIAutomationTextRange* r) {
    winrt::com_ptr<IUIAutomationTextRange> para, pre;
    if (FAILED(r->Clone(para.put())) || !para) return {};
    para->ExpandToEnclosingUnit(TextUnit_Paragraph);
    BSTR b = nullptr;
    para->GetText(4000, &b);
    std::wstring ptext = LkBstr(b);
    if (ptext.empty()) return {};
    size_t off = 0;
    if (SUCCEEDED(para->Clone(pre.put())) && pre &&
        SUCCEEDED(pre->MoveEndpointByRange(TextPatternRangeEndpoint_End, r,
                                           TextPatternRangeEndpoint_Start))) {
        BSTR pb = nullptr;
        pre->GetText(4000, &pb);
        off = LkBstr(pb).size();
    }
    BSTR wb = nullptr;
    r->GetText(400, &wb);
    size_t len = LkBstr(wb).size();
    return LkSentenceAround(ptext, off, len);
}

static bool LkUiaWordAt(POINT pt, LkHit& hit) {
    winrt::com_ptr<IUIAutomation> uia;
    if (FAILED(CoCreateInstance(kLkClsidCUIAutomation, nullptr,
                                CLSCTX_INPROC_SERVER, kLkIidIUIAutomation,
                                uia.put_void())) ||
        !uia) {
        return false;
    }
    winrt::com_ptr<IUIAutomation2> uia2;
    if (SUCCEEDED(uia->QueryInterface(kLkIidIUIAutomation2, uia2.put_void())) &&
        uia2) {
        uia2->put_ConnectionTimeout(1500);
        uia2->put_TransactionTimeout(2000);
    }
    winrt::com_ptr<IUIAutomationElement> el;
    if (FAILED(uia->ElementFromPoint(pt, el.put())) || !el) return false;
    int pid = 0;
    el->get_CurrentProcessId(&pid);
    if ((DWORD)pid == GetCurrentProcessId()) return false;

    winrt::com_ptr<IUIAutomationTreeWalker> walker;
    uia->get_ControlViewWalker(walker.put());
    winrt::com_ptr<IUIAutomationTextPattern> tp;
    for (int i = 0; i < 10 && el; i++) {
        if (SUCCEEDED(el->GetCurrentPatternAs(kLkTextPatternId, kLkIidTextPattern,
                                              tp.put_void())) &&
            tp) {
            break;
        }
        tp = nullptr;
        if (!walker) break;
        winrt::com_ptr<IUIAutomationElement> parent;
        if (FAILED(walker->GetParentElement(el.get(), parent.put()))) break;
        el = parent;
    }
    if (!tp) {
        BSTR cb = nullptr;
        std::wstring cls;
        if (el && SUCCEEDED(el->get_CurrentClassName(&cb))) cls = LkBstr(cb);
        LkLog(L"UIA: no text pattern (pid %d, class '%s')", pid, cls.c_str());
        return false;
    }

    // The selection, if the pointer is on it.
    winrt::com_ptr<IUIAutomationTextRangeArray> sel;
    if (SUCCEEDED(tp->GetSelection(sel.put())) && sel) {
        int n = 0;
        sel->get_Length(&n);
        for (int i = 0; i < n && i < 4; i++) {
            winrt::com_ptr<IUIAutomationTextRange> r;
            if (FAILED(sel->GetElement(i, r.put())) || !r) continue;
            BSTR b = nullptr;
            r->GetText(1200, &b);
            std::wstring t = LkCleanWord(LkBstr(b));
            if (t.empty() || t.size() > 1000) continue;  // phrases get a translation
            SAFEARRAY* sa = nullptr;
            RECT rc{};
            if (SUCCEEDED(r->GetBoundingRectangles(&sa)) &&
                LkRectsContain(sa, pt, 3, &rc)) {
                hit.word = t;
                hit.rect = rc;
                hit.fromSelection = true;
                hit.context = LkParagraphContext(r.get());
                return true;
            }
        }
    }

    winrt::com_ptr<IUIAutomationTextRange> r;
    if (FAILED(tp->RangeFromPoint(pt, r.put())) || !r) return false;
    if (FAILED(r->ExpandToEnclosingUnit(TextUnit_Word))) return false;
    BSTR b = nullptr;
    r->GetText(120, &b);
    std::wstring t = LkCleanWord(LkBstr(b));
    if (t.empty()) return false;
    SAFEARRAY* sa = nullptr;
    RECT rc{};
    // RangeFromPoint returns the nearest text even far from the pointer.
    if (FAILED(r->GetBoundingRectangles(&sa)) ||
        !LkRectsContain(sa, pt, 3, &rc)) {
        LkLog(L"UIA: nearest text '%s' is not under the pointer", t.substr(0, 40).c_str());
        return false;
    }
    hit.word = t;
    hit.rect = rc;
    hit.context = LkParagraphContext(r.get());
    return true;
}

// ---------------------------------------------------------------------------
// Finding the word: OCR of the screen around the pointer
// ---------------------------------------------------------------------------

namespace lkw {
using namespace winrt::Windows::Media::Ocr;
using namespace winrt::Windows::Graphics::Imaging;
using winrt::Windows::Globalization::Language;
using winrt::Windows::Storage::Streams::DataWriter;
}  // namespace lkw

static bool LkOcrPick(const lkw::OcrResult& res, float ix, float iy, float k,
                      LONG ox, LONG oy, LkHit& out, int* count = nullptr) {
    float best = 1e9f;
    bool found = false;
    for (const auto& line : res.Lines()) {
        for (const auto& w : line.Words()) {
            if (count) (*count)++;
            auto r = w.BoundingRect();
            float dx = std::max({r.X - ix, 0.f, ix - (r.X + r.Width)});
            float dy = std::max({r.Y - iy, 0.f, iy - (r.Y + r.Height)});
            float d = std::sqrt(dx * dx + dy * dy);
            if (d < best && d <= 6 * k) {
                best = d;
                found = true;
                out.word = std::wstring(w.Text());
                out.context = std::wstring(line.Text());
                out.rect = {ox + LONG(r.X / k), oy + LONG(r.Y / k),
                            ox + LONG((r.X + r.Width) / k),
                            oy + LONG((r.Y + r.Height) / k)};
            }
        }
    }
    return found;
}

static bool LkOcrWordAt(POINT pt, LkHit& hit) {
    RECT vs{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
            0, 0};
    vs.right = vs.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vs.bottom = vs.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    RECT cap{pt.x - 360, pt.y - 70, pt.x + 360, pt.y + 70};
    IntersectRect(&cap, &cap, &vs);
    int w = cap.right - cap.left, h = cap.bottom - cap.top;
    if (w < 16 || h < 16) return false;
    // Small UI text reads much better enlarged.
    float scale = GetMonitorScale(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST));
    int k = scale < 1.5f ? 2 : 1;
    int W = w * k, H = h * k;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) {
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return false;
    }
    HGDIOBJ old = SelectObject(mem, dib);
    SetStretchBltMode(mem, HALFTONE);
    StretchBlt(mem, 0, 0, W, H, screen, cap.left, cap.top, w, h,
               SRCCOPY | CAPTUREBLT);
    GdiFlush();
    auto* px = static_cast<uint8_t*>(bits);
    for (int i = 0; i < W * H; i++) px[i * 4 + 3] = 255;

    bool ok = false;
    try {
        lkw::SoftwareBitmap bmp(lkw::BitmapPixelFormat::Bgra8, W, H,
                                lkw::BitmapAlphaMode::Premultiplied);
        lkw::DataWriter dw;
        dw.WriteBytes(winrt::array_view<const uint8_t>(px, px + W * H * 4));
        bmp.CopyFromBuffer(dw.DetachBuffer());
        float ix = float(pt.x - cap.left) * k, iy = float(pt.y - cap.top) * k;

        LkHit en, hi;
        bool haveEn = false, haveHi = false;
        int nEn = -1;
        if (auto eng = lkw::OcrEngine::TryCreateFromLanguage(lkw::Language(L"en-US"))) {
            nEn = 0;
            haveEn = LkOcrPick(eng.RecognizeAsync(bmp).get(), ix, iy, (float)k,
                               cap.left, cap.top, en, &nEn);
        }
        if (auto eng = lkw::OcrEngine::TryCreateFromLanguage(lkw::Language(L"hi-IN"))) {
            haveHi = LkOcrPick(eng.RecognizeAsync(bmp).get(), ix, iy, (float)k,
                               cap.left, cap.top, hi) &&
                     LkHasDevanagari(hi.word);
        }
        if (haveHi) {
            hit = hi;
            ok = true;
        } else if (haveEn) {
            hit = en;
            ok = true;
        } else {
            LkLog(L"OCR: %d words read around the pointer, none under it%s", nEn,
                  nEn < 0 ? L" (no English OCR engine)" : L"");
        }
    } catch (...) {
        LkLog(L"OCR failed");
        ok = false;
    }
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (ok) hit.word = LkCleanWord(hit.word);
    return ok && !hit.word.empty();
}

// ---------------------------------------------------------------------------
// WordNet (D:\lookup\wordnet.tsv, sorted by key bytes; see build-wordnet.py)
// ---------------------------------------------------------------------------

struct LkSense {
    std::wstring lemma, pos, def, example, syns;
};

static std::mutex g_wnLock;
static HANDLE g_wnFile = INVALID_HANDLE_VALUE, g_wnMap = nullptr;
static const char* g_wnData = nullptr;
static size_t g_wnSize = 0;

static bool LkWnOpen() {
    if (g_wnData) return true;
    g_wnFile = CreateFileW(kLookupWordNet, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_wnFile == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(g_wnFile, &sz);
    g_wnMap = CreateFileMappingW(g_wnFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (g_wnMap) g_wnData = (const char*)MapViewOfFile(g_wnMap, FILE_MAP_READ, 0, 0, 0);
    if (!g_wnData) {
        if (g_wnMap) CloseHandle(g_wnMap);
        CloseHandle(g_wnFile);
        g_wnMap = nullptr;
        g_wnFile = INVALID_HANDLE_VALUE;
        return false;
    }
    g_wnSize = (size_t)sz.QuadPart;
    return true;
}

static void LkWnClose() {
    std::lock_guard<std::mutex> lock(g_wnLock);
    if (g_wnData) UnmapViewOfFile(g_wnData);
    if (g_wnMap) CloseHandle(g_wnMap);
    if (g_wnFile != INVALID_HANDLE_VALUE) CloseHandle(g_wnFile);
    g_wnData = nullptr;
    g_wnMap = nullptr;
    g_wnFile = INVALID_HANDLE_VALUE;
}

static size_t LkWnLineEnd(size_t s) {
    const void* e = memchr(g_wnData + s, '\n', g_wnSize - s);
    return e ? (size_t)((const char*)e - g_wnData) : g_wnSize;
}

static std::string_view LkWnKey(size_t s) {
    size_t e = LkWnLineEnd(s);
    const void* t = memchr(g_wnData + s, '\t', e - s);
    size_t k = t ? (size_t)((const char*)t - g_wnData) : e;
    return std::string_view(g_wnData + s, k - s);
}

// All lines whose key is exactly `key`.
static std::vector<std::string_view> LkWnLines(std::string_view key) {
    std::vector<std::string_view> out;
    size_t lo = 0, hi = g_wnSize;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        size_t s = mid;
        while (s > lo && g_wnData[s - 1] != '\n') s--;
        if (LkWnKey(s) < key) {
            lo = LkWnLineEnd(s) + 1;
        } else {
            hi = s;
        }
    }
    for (size_t s = lo; s < g_wnSize;) {
        if (LkWnKey(s) != key) break;
        size_t e = LkWnLineEnd(s);
        out.emplace_back(g_wnData + s, e - s);
        s = e + 1;
    }
    return out;
}

static std::vector<std::string_view> LkSplitTabs(std::string_view l) {
    std::vector<std::string_view> f;
    size_t a = 0;
    for (size_t i = 0; i <= l.size(); i++) {
        if (i == l.size() || l[i] == '\t') {
            f.push_back(l.substr(a, i - a));
            a = i + 1;
        }
    }
    return f;
}

static std::vector<LkSense> LkDictionary(const std::wstring& word) {
    std::vector<LkSense> out;
    std::wstring lw = word;
    for (auto& c : lw) c = towlower(c);
    std::lock_guard<std::mutex> lock(g_wnLock);
    if (!LkWnOpen()) return out;

    std::vector<std::string> cands{LkWideToUtf8(lw)};
    for (auto l : LkWnLines(cands[0])) {
        auto f = LkSplitTabs(l);
        if (f.size() >= 3 && f[1] == "=") cands.emplace_back(f[2]);
    }
    // WordNet's morphy rules, plus doubled consonants (running, stopped).
    static const char* rules[][2] = {
        {"s", ""},     {"ses", "s"}, {"xes", "x"}, {"zes", "z"}, {"ches", "ch"},
        {"shes", "sh"}, {"men", "man"}, {"ies", "y"}, {"es", "e"}, {"es", ""},
        {"ed", "e"},   {"ed", ""},   {"ing", "e"}, {"ing", ""},  {"er", ""},
        {"est", ""},   {"er", "e"},  {"est", "e"}, {"ly", ""},   {"ily", "y"}};
    std::string w8 = cands[0];
    for (auto& r : rules) {
        size_t n = strlen(r[0]);
        if (w8.size() > n + 1 && w8.compare(w8.size() - n, n, r[0]) == 0) {
            std::string base = w8.substr(0, w8.size() - n) + r[1];
            cands.push_back(base);
            size_t b = base.size();
            if (!*r[1] && b >= 3 && base[b - 1] == base[b - 2]) {
                cands.push_back(base.substr(0, b - 1));
            }
        }
    }

    std::vector<std::string> seen;
    std::vector<std::string> posTaken;
    for (const auto& c : cands) {
        if (std::find(seen.begin(), seen.end(), c) != seen.end()) continue;
        seen.push_back(c);
        for (auto l : LkWnLines(c)) {
            auto f = LkSplitTabs(l);
            if (f.size() < 5 || f[1] == "=") continue;
            std::string pos(f[1]);
            // A part of speech comes from the closest form only.
            bool taken = std::find(posTaken.begin(), posTaken.end(), pos + "|" + c) ==
                             posTaken.end() &&
                         std::any_of(posTaken.begin(), posTaken.end(),
                                     [&](const std::string& p) {
                                         return p.compare(0, pos.size() + 1, pos + "|") == 0;
                                     });
            if (taken) continue;
            posTaken.push_back(pos + "|" + c);
            LkSense s;
            s.lemma = LkUtf8ToWide(f[0]);
            s.pos = pos == "n" ? L"noun" : pos == "v" ? L"verb" : pos == "a" ? L"adjective" : L"adverb";
            s.def = LkUtf8ToWide(f[2]);
            s.example = LkUtf8ToWide(f[3]);
            s.syns = LkUtf8ToWide(f[4]);
            out.push_back(std::move(s));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// HTTP (WinHTTP) and a minimal JSON string reader
// ---------------------------------------------------------------------------

static bool LkHttp(const std::wstring& host, INTERNET_PORT port, bool https,
                   const wchar_t* verb, const std::wstring& path,
                   const std::string& body, DWORD timeoutMs, std::string& out,
                   DWORD& status) {
    out.clear();
    status = 0;
    HINTERNET ses = WinHttpOpen(L"WindowsLookup/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        ses = WinHttpOpen(L"WindowsLookup/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!ses) return false;
    WinHttpSetTimeouts(ses, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    bool ok = false;
    HINTERNET con = WinHttpConnect(ses, host.c_str(), port, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, verb, path.c_str(), nullptr,
                                             WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             https ? WINHTTP_FLAG_SECURE : 0)
                        : nullptr;
    if (req) {
        const wchar_t* hdr = body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                          : L"Content-Type: application/json\r\n";
        if (WinHttpSendRequest(req, hdr, body.empty() ? 0 : (DWORD)-1L,
                               (LPVOID)(body.empty() ? nullptr : body.data()),
                               (DWORD)body.size(), (DWORD)body.size(), 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD sz = sizeof(status);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                                WINHTTP_NO_HEADER_INDEX);
            ok = true;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail) || !avail) break;
                size_t at = out.size();
                out.resize(at + avail);
                DWORD got = 0;
                if (!WinHttpReadData(req, out.data() + at, avail, &got)) {
                    ok = false;
                    break;
                }
                out.resize(at + got);
                if (out.size() > 8 * 1024 * 1024) break;
            }
        }
        WinHttpCloseHandle(req);
    }
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

static bool LkHttpUrl(const std::wstring& url, DWORD timeoutMs, std::string& out) {
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    DWORD st = 0;
    return LkHttp(host, uc.nPort, uc.nScheme == INTERNET_SCHEME_HTTPS, L"GET", path,
                  "", timeoutMs, out, st) &&
           st == 200;
}

static void LkAppendUtf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) {
        s += (char)cp;
    } else if (cp < 0x800) {
        s += (char)(0xC0 | (cp >> 6));
        s += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += (char)(0xE0 | (cp >> 12));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    } else {
        s += (char)(0xF0 | (cp >> 18));
        s += (char)(0x80 | ((cp >> 12) & 0x3F));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    }
}

// The JSON string literal whose opening quote is at j[q].
static bool LkJsonStrAt(const std::string& j, size_t q, std::wstring& out,
                        size_t* endPos = nullptr) {
    if (q >= j.size() || j[q] != '"') return false;
    q++;
    std::string s;
    while (q < j.size() && j[q] != '"') {
        char c = j[q++];
        if (c != '\\') {
            s += c;
            continue;
        }
        if (q >= j.size()) break;
        char e = j[q++];
        switch (e) {
            case 'n': s += '\n'; break;
            case 't': s += '\t'; break;
            case 'r': break;
            case 'b': case 'f': break;
            case 'u': {
                if (q + 4 > j.size()) break;
                uint32_t cp = strtoul(j.substr(q, 4).c_str(), nullptr, 16);
                q += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && q + 6 <= j.size() &&
                    j[q] == '\\' && j[q + 1] == 'u') {
                    uint32_t lo = strtoul(j.substr(q + 2, 4).c_str(), nullptr, 16);
                    q += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                LkAppendUtf8(s, cp);
                break;
            }
            default: s += e; break;
        }
    }
    out = LkUtf8ToWide(s);
    if (endPos) *endPos = q;
    return true;
}

// The string value of the first "key" at or after `from`. Not a full parser:
// enough for the flat fields read from Wikipedia and ollama.
static bool LkJsonStr(const std::string& j, const char* key, std::wstring& out,
                      size_t from = 0, size_t* endPos = nullptr) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k, from);
    while (p != std::string::npos) {
        size_t q = p + k.size();
        while (q < j.size() && isspace((unsigned char)j[q])) q++;
        if (q < j.size() && j[q] == ':') {
            q++;
            while (q < j.size() && isspace((unsigned char)j[q])) q++;
            return LkJsonStrAt(j, q, out, endPos);
        }
        p = j.find(k, p + 1);
    }
    return false;
}

static std::string LkJsonEscape(const std::wstring& w) {
    std::string s = LkWideToUtf8(w), o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += (char)c;
        } else if (c == '\n') {
            o += "\\n";
        } else if (c < 0x20) {
            o += ' ';
        } else {
            o += (char)c;
        }
    }
    return o;
}

static std::wstring LkUrlEncode(const std::wstring& w) {
    std::string s = LkWideToUtf8(w);
    std::wstring o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            o += (wchar_t)c;
        } else {
            wchar_t b[4];
            swprintf(b, 4, L"%%%02X", c);
            o += b;
        }
    }
    return o;
}

// ---------------------------------------------------------------------------
// Wikipedia
// ---------------------------------------------------------------------------

struct LkWiki {
    std::wstring title, extract, url, thumbUrl;
};

static bool LkWikiSummary(const std::wstring& host, std::wstring title, LkWiki& w) {
    for (auto& c : title) {
        if (c == L' ') c = L'_';
    }
    std::string body;
    DWORD st = 0;
    if (!LkHttp(host, 443, true, L"GET",
                L"/api/rest_v1/page/summary/" + LkUrlEncode(title), "", 5000, body,
                st) ||
        st != 200) {
        return false;
    }
    std::wstring type;
    LkJsonStr(body, "type", type);
    if (type == L"disambiguation") return false;
    LkJsonStr(body, "title", w.title);
    LkJsonStr(body, "extract", w.extract);
    size_t t = body.find("\"thumbnail\"");
    if (t != std::string::npos) LkJsonStr(body, "source", w.thumbUrl, t);
    size_t d = body.find("\"desktop\"");
    if (d != std::string::npos) LkJsonStr(body, "page", w.url, d);
    return !w.extract.empty();
}

static bool LkWikipedia(const std::wstring& word, LkWiki& w) {
    std::wstring host = LkHasDevanagari(word) ? L"hi.wikipedia.org" : L"en.wikipedia.org";
    if (LkWikiSummary(host, word, w)) return true;
    // Not an exact title: take the best search match.
    std::string body;
    DWORD st = 0;
    if (!LkHttp(host, 443, true, L"GET",
                L"/w/api.php?action=opensearch&limit=1&namespace=0&format=json&search=" +
                    LkUrlEncode(word),
                "", 5000, body, st) ||
        st != 200) {
        return false;
    }
    // ["query",["Title"],...]: skip the query string (which may hold commas,
    // brackets or escaped quotes), then read the first title.
    std::wstring query, title;
    size_t end = 0;
    size_t q0 = body.find('"');
    if (q0 == std::string::npos || !LkJsonStrAt(body, q0, query, &end)) return false;
    size_t a = body.find('[', end);
    if (a == std::string::npos) return false;
    a++;
    while (a < body.size() && isspace((unsigned char)body[a])) a++;
    if (!LkJsonStrAt(body, a, title)) return false;  // [] = no match
    return !title.empty() && LkWikiSummary(host, title, w);
}

// ---------------------------------------------------------------------------
// Local model (ollama)
// ---------------------------------------------------------------------------

static bool LkOllamaUp() {
    std::string b;
    DWORD st = 0;
    return LkHttp(L"127.0.0.1", 11434, false, L"GET", L"/api/version", "", 500, b,
                  st) &&
           st == 200;
}

// Starts `ollama serve` with the user's OLLAMA_* settings (models on D:).
static bool LkOllamaStart() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        for (DWORD i = 0;; i++) {
            wchar_t name[128];
            DWORD nameLen = ARRAYSIZE(name), type = 0;
            BYTE data[2048];
            DWORD dataLen = sizeof(data) - 2;
            LONG r = RegEnumValueW(key, i, name, &nameLen, nullptr, &type, data, &dataLen);
            if (r == ERROR_NO_MORE_ITEMS) break;
            if (r != ERROR_SUCCESS || _wcsnicmp(name, L"OLLAMA_", 7) != 0) continue;
            if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
            data[dataLen] = data[dataLen + 1] = 0;
            wchar_t expanded[2048];
            ExpandEnvironmentStringsW((wchar_t*)data, expanded, ARRAYSIZE(expanded));
            SetEnvironmentVariableW(name, expanded);
        }
        RegCloseKey(key);
    }
    std::wstring cmd = std::wstring(L"\"") + kLookupOllamaExe + L"\" serve";
    std::wstring dir = kLookupOllamaExe;
    dir.resize(dir.rfind(L'\\'));
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(kLookupOllamaExe, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr,
                        dir.c_str(), &si, &pi)) {
        LkLog(L"starting ollama failed: %u", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    for (int i = 0; i < 80 && !g_lookupQuit; i++) {
        Sleep(250);
        if (LkOllamaUp()) return true;
    }
    return false;
}

// What the model said. Word: note ("in this sentence"). Native: meaning and
// note. Phrase: translation (empty when the text is already English) and meaning.
struct LkAnswer {
    std::wstring note, meaning, translation;
};

static bool LkHasContext(const std::wstring& word, const std::wstring& context) {
    return !context.empty() && context != word;
}

// Letters and digits only, lower case: to tell a translation from an echo.
static std::wstring LkFold(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (iswalnum(c) || LkIsNonLatinLetter(c)) o += towlower(c);
    }
    return o;
}

// One model call per lookup. Native and Phrase ask for a small JSON object
// (ollama's "format":"json" keeps a 1.7B model to it).
static bool LkOllamaAsk(LkMode mode, const std::wstring& word, const std::wstring& context,
                        LkAnswer& ans) {
    std::wstring prompt;
    bool json = mode != LkMode::Word;
    int predict = 110;
    if (mode == LkMode::Word) {
        if (LkHasContext(word, context)) {
            prompt = L"Sentence: \"" + context + L"\"\nIn one or two short sentences, explain what \"" +
                     word + L"\" means in this sentence. Answer in English, plain text, no preamble.";
        } else {
            prompt = L"In one or two short sentences, explain what \"" + word +
                     L"\" means. Answer in English, plain text, no preamble.";
        }
    } else if (mode == LkMode::Native) {
        if (LkHasContext(word, context)) {
            prompt = L"Hindi word: \"" + word + L"\"\nSentence: \"" + context +
                     L"\"\nReply with JSON only, in English: {\"meaning\": \"<the word's English "
                     L"meaning, a few words>\", \"context\": \"<one short sentence: what the word "
                     L"means in this sentence>\"}";
            predict = 120;
        } else {
            prompt = L"Hindi word: \"" + word +
                     L"\"\nReply with JSON only, in English: {\"meaning\": \"<the word's English "
                     L"meaning, a few words>\"}";
            predict = 60;
        }
    } else {
        prompt = L"Text: \"" + word +
                 L"\"\nReply with JSON only: {\"translation\": \"<the English translation of the "
                 L"text>\", \"meaning\": \"<one short sentence on what it means>\"}";
        predict = std::clamp(80 + (int)word.size(), 80, 420);
    }
    std::string body = std::string("{\"model\":\"") + kLookupModel + "\",\"prompt\":\"" +
                       LkJsonEscape(prompt) + "\",\"stream\":false,\"think\":false,\"keep_alive\":0," +
                       (json ? "\"format\":\"json\"," : "") +
                       "\"options\":{\"num_thread\":8,\"num_predict\":" + std::to_string(predict) +
                       ",\"temperature\":0.3}}";
    std::string out;
    std::wstring answer;
    DWORD st = 0;
    if (!LkHttp(L"127.0.0.1", 11434, false, L"POST", L"/api/generate", body, 60000, out,
                st) ||
        st != 200 || !LkJsonStr(out, "response", answer)) {
        return false;
    }
    size_t t = answer.find(L"</think>");
    if (t != std::wstring::npos) answer = answer.substr(t + 8);
    if (!json) {
        ans.note = LkCollapseSpace(answer);
        return !ans.note.empty();
    }
    std::string a8 = LkWideToUtf8(answer);
    if (mode == LkMode::Native) {
        LkJsonStr(a8, "meaning", ans.meaning);
        LkJsonStr(a8, "context", ans.note);
    } else {
        LkJsonStr(a8, "translation", ans.translation);
        LkJsonStr(a8, "meaning", ans.meaning);
        // An English text comes back as itself: not a translation.
        if (LkFold(ans.translation) == LkFold(word)) ans.translation.clear();
    }
    ans.note = LkCollapseSpace(ans.note);
    ans.meaning = LkCollapseSpace(ans.meaning);
    ans.translation = LkCollapseSpace(ans.translation);
    if (ans.note.empty() && ans.meaning.empty() && ans.translation.empty() &&
        answer.find(L'{') == std::wstring::npos) {
        ans.meaning = LkCollapseSpace(answer);  // not JSON after all: show it as it is
    }
    return !(ans.note.empty() && ans.meaning.empty() && ans.translation.empty());
}

// ---------------------------------------------------------------------------
// Workers
// ---------------------------------------------------------------------------

enum class LkKind { NotFound, Word, Dict, Wiki, AiStatus, Ai };

struct LkMsg {
    UINT gen = 0;
    LkKind kind = LkKind::NotFound;
    std::wstring a, b, c;
    RECT rect{};
    std::vector<LkSense> senses;
    std::string bytes;
    bool ok = false;
    LkMode mode = LkMode::Word;
};

static void LkPost(LkMsg* m) {
    HWND h = g_hwnd;
    if (!h || g_lookupQuit || !PostMessageW(h, WM_RS_LOOKUP_RESULT, 0, (LPARAM)m)) {
        delete m;
    }
}

static bool LkCurrent(UINT gen) { return !g_lookupQuit && gen == g_lookupGen; }

struct LkJob {
    UINT gen = 0;
    POINT pt{};
    std::wstring word, context;
    HMODULE self = nullptr;
    LkMode mode = LkMode::Word;
};

static bool LkStartThread(LPTHREAD_START_ROUTINE fn, LkJob* job) {
    // The thread keeps the DLL loaded until it ends (FreeLibraryAndExitThread).
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&LkStartThread), &job->self);
    HANDLE t = CreateThread(nullptr, 0, fn, job, 0, nullptr);
    if (!t) {
        if (job->self) FreeLibrary(job->self);
        delete job;
        return false;
    }
    CloseHandle(t);
    return true;
}

static void LkRunAi(const LkJob& job) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
    if (!LkOllamaUp()) {
        auto* m = new LkMsg{job.gen, LkKind::AiStatus};
        m->a = L"Starting the local model\x2026";
        LkPost(m);
        if (!LkOllamaStart()) {
            auto* f = new LkMsg{job.gen, LkKind::Ai};
            f->a = L"Couldn't start the local model.";
            LkPost(f);
            return;
        }
    }
    if (!LkCurrent(job.gen)) return;
    LkAnswer ans;
    auto* m = new LkMsg{job.gen, LkKind::Ai};
    m->ok = LkOllamaAsk(job.mode, job.word, job.context, ans);
    if (m->ok) {
        m->a = ans.note;
        m->b = ans.meaning;
        m->c = ans.translation;
    } else {
        m->a = L"The local model didn't answer.";
    }
    LkLog(L"model (%s): %s; note %u, meaning %u, translation %u chars",
          job.mode == LkMode::Word ? L"word" : job.mode == LkMode::Native ? L"hindi" : L"phrase",
          m->ok ? L"answered" : L"no answer", (unsigned)ans.note.size(),
          (unsigned)ans.meaning.size(), (unsigned)ans.translation.size());
    LkPost(m);
}

static DWORD WINAPI LkAiThread(LPVOID p) {
    auto* job = static_cast<LkJob*>(p);
    HMODULE self = job->self;
    LkRunAi(*job);
    delete job;
    FreeLibraryAndExitThread(self, 0);
}

static void LkRunLookup(const LkJob& job) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    LkHit hit;
    bool ok = false;
    try {
        ok = LkUiaWordAt(job.pt, hit);
    } catch (...) {
        ok = false;
    }
    if (ok) {
        LkLog(L"UIA word '%s'", hit.word.c_str());
    } else if (LkCurrent(job.gen)) {
        hit = {};
        ok = LkOcrWordAt(job.pt, hit);
        if (ok) LkLog(L"OCR word '%s'", hit.word.c_str());
    }
    if (!LkCurrent(job.gen)) {
        LkLog(L"lookup %u superseded", job.gen);
        return;
    }
    if (!ok || hit.word.empty()) {
        LkLog(L"no word at %d,%d", job.pt.x, job.pt.y);
        LkPost(new LkMsg{job.gen, LkKind::NotFound});
        return;
    }
    if (hit.context.size() > 400) hit.context.resize(400);

    const LkMode mode = LkModeFor(hit.word);
    auto* wm = new LkMsg{job.gen, LkKind::Word};
    wm->a = hit.word;
    wm->b = hit.context;
    wm->rect = hit.rect;
    wm->mode = mode;
    LkPost(wm);

    auto* ai = new LkJob{job.gen, job.pt, hit.word, hit.context};
    ai->mode = mode;
    LkStartThread(LkAiThread, ai);

    // The dictionary is English only; a phrase gets neither it nor Wikipedia.
    if (mode == LkMode::Word) {
        auto* dm = new LkMsg{job.gen, LkKind::Dict};
        dm->senses = LkDictionary(hit.word);
        LkLog(L"dictionary: %u senses", (unsigned)dm->senses.size());
        LkPost(dm);
    }
    if (mode == LkMode::Phrase || !LkCurrent(job.gen)) return;
    LkWiki w;
    auto* km = new LkMsg{job.gen, LkKind::Wiki};
    km->ok = LkWikipedia(hit.word, w);
    if (km->ok) {
        km->a = w.title;
        km->b = w.extract;
        km->c = w.url;
        if (!w.thumbUrl.empty() && LkCurrent(job.gen)) {
            LkHttpUrl(w.thumbUrl, 5000, km->bytes);
        }
    }
    LkLog(L"wikipedia: %s '%s', thumb %u bytes", km->ok ? L"found" : L"none", km->a.c_str(),
          (unsigned)km->bytes.size());
    LkPost(km);
}

static DWORD WINAPI LkLookupThread(LPVOID p) {
    auto* job = static_cast<LkJob*>(p);
    HMODULE self = job->self;
    LkRunLookup(*job);
    delete job;
    FreeLibraryAndExitThread(self, 0);
}

// ---------------------------------------------------------------------------
// Popover (UI thread)
// ---------------------------------------------------------------------------

enum class LkState { Pending, None, Done };

static struct {
    UINT gen = 0;
    std::wstring word, context;
    RECT anchor{};
    bool above = false;
    float scale = 1;
    LkState dict = LkState::Pending;
    std::vector<LkSense> senses;
    LkState wiki = LkState::Pending;
    std::wstring wikiTitle, wikiText, wikiUrl;
    std::unique_ptr<gp::Bitmap> thumb;
    LkState ai = LkState::Pending;
    std::wstring aiText = L"Thinking\x2026";   // status, then the note ("in this sentence")
    std::wstring aiMeaning, aiTranslation;     // Native and Phrase answers
    LkMode mode = LkMode::Word;
    RECT wikiHit{};  // window-local
} g_pop;

static HWND g_popHwnd = nullptr;
static HDC g_popMem = nullptr;
static HBITMAP g_popDib = nullptr;
static HGDIOBJ g_popOld = nullptr;
static int g_popW = 0, g_popH = 0;
static POINT g_lookupPt{};
static HWINEVENTHOOK g_popFgHook = nullptr;
static ULONGLONG g_popShownAt = 0;
static std::atomic<LONG> g_popL{0}, g_popT{0}, g_popR{0}, g_popB{0};
static HHOOK g_lookupMouseHook = nullptr;  // hook thread

struct LkDpiScope {
    DPI_AWARENESS_CONTEXT old;
    LkDpiScope() { old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); }
    ~LkDpiScope() {
        if (old) SetThreadDpiAwarenessContext(old);
    }
};

static void LookupMouseHookCtl(bool on);

static LRESULT CALLBACK LookupMouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_lookupVisible &&
        (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN)) {
        auto* m = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        RECT r{g_popL, g_popT, g_popR, g_popB};
        if (!PtInRect(&r, m->pt)) {
            if (HWND h = g_hwnd) PostMessageW(h, WM_RS_LOOKUP_CLOSE, 0, 0);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// Runs on the hook thread (its message loop is what keeps a LL hook alive).
static void LookupMouseHookCtl(bool on) {
    if (on && !g_lookupMouseHook) {
        g_lookupMouseHook = SetWindowsHookExW(WH_MOUSE_LL, LookupMouseProc, g_hInst, 0);
    } else if (!on && g_lookupMouseHook) {
        UnhookWindowsHookEx(g_lookupMouseHook);
        g_lookupMouseHook = nullptr;
    }
}

static void LookupClose(const wchar_t* why = L"") {
    if (g_lookupVisible) LkLog(L"popover closed: %s", why);
    if (g_lookupVisible) {
        g_lookupVisible = false;
        if (g_hookThreadId) PostThreadMessageW(g_hookThreadId, WM_RS_LOOKUP_MHOOK, 0, 0);
    }
    g_lookupGen++;  // workers stop, late results are dropped
    if (g_popFgHook) {
        UnhookWinEvent(g_popFgHook);
        g_popFgHook = nullptr;
    }
    if (g_popHwnd) ShowWindow(g_popHwnd, SW_HIDE);
    g_pop.thumb.reset();
    g_pop.senses.clear();
}

static void CALLBACK LkForegroundProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD,
                                      DWORD) {
    if (g_lookupVisible && GetTickCount64() - g_popShownAt > 200) {
        LookupClose(L"app switch");
    }
}

// The look of the other mods (top bar, island, panels): Start's theme. Black
// at 77% with no blur, #1F1F1F tiles for cards, white text with secondary text
// at lower alpha, a faint white hairline, Segoe UI Variable Text, 14 px panels.
static const gp::Color kLkSurface(0xC4, 0, 0, 0);
static const gp::Color kLkTile(0xFF, 0x1F, 0x1F, 0x1F);
static const gp::Color kLkHairline(26, 255, 255, 255);
static const gp::Color kLkFg(255, 255, 255, 255);
static const gp::Color kLkMuted(153, 228, 228, 228);
static const gp::Color kLkFaint(110, 228, 228, 228);

static const wchar_t* LkFamily(bool semibold) {
    static std::wstring regular, semi;
    if (regular.empty()) {
        auto has = [](const wchar_t* name) {
            gp::FontFamily f(name);
            return f.GetLastStatus() == gp::Ok && f.IsAvailable();
        };
        regular = has(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
        semi = has(L"Segoe UI Variable Text Semibold") ? L"Segoe UI Variable Text Semibold"
                                                       : L"Segoe UI Semibold";
    }
    return semibold ? semi.c_str() : regular.c_str();
}

static gp::Font* LkFont(const std::wstring& text, float px, int style, bool semibold = false) {
    const wchar_t* fam = LkHasDevanagari(text) ? L"Nirmala UI" : LkFamily(semibold);
    return new gp::Font(fam, px, style, gp::UnitPixel);
}

// How much the popover keeps when space runs short: 0 everything, 1 fewer
// senses and shorter Wikipedia text, 2 the minimum. The local model's
// section is always kept.
struct LkLevel {
    int perPos, total, wikiLines, aiLines;
    bool examples, syns;
};
static const LkLevel kLkLevels[] = {
    {2, 5, 6, 6, true, true},
    {1, 3, 4, 5, true, false},
    {1, 2, 3, 4, false, false},
};

// Cards: 0 dictionary, 1 Wikipedia, 2 and 3 the local model.
static float g_lkCardTop[4], g_lkCardBottom[4];  // from the measuring pass

// Lays the popover out, and draws it when `draw`. Returns the content height.
static float LkLayout(gp::Graphics& g, bool draw, float W, float H, int level) {
    const LkLevel& L = kLkLevels[level];
    const float s = g_pop.scale;
    const float outer = 10 * s;       // surface edge to card
    const float pad = 12 * s;         // card edge to text
    const float x0 = outer + pad;
    const float cw = W - 2 * x0;
    float y = 14 * s;

    if (draw) {
        gp::GraphicsPath path;
        AddRoundRect(path, gp::RectF(0.5f, 0.5f, W - 1, H - 1), 14 * s);
        gp::SolidBrush bg(kLkSurface);
        g.FillPath(&bg, &path);
        gp::Pen pen(kLkHairline, 1);
        g.DrawPath(&pen, &path);
    }

    gp::StringFormat fmt(gp::StringFormat::GenericTypographic());
    fmt.SetFormatFlags(fmt.GetFormatFlags() | gp::StringFormatFlagsLineLimit);
    fmt.SetTrimming(gp::StringTrimmingEllipsisWord);

    auto text = [&](const std::wstring& t, gp::Font* f, const gp::Color& c, float x,
                    float w, int maxLines) -> float {
        if (t.empty()) {
            delete f;
            return 0;
        }
        float lh = f->GetHeight(&g);
        gp::RectF lay(x, y, w, maxLines > 0 ? lh * maxLines + 0.5f : 100000.f);
        gp::RectF box;
        g.MeasureString(t.c_str(), (INT)t.size(), f, lay, &fmt, &box);
        float h = std::min(box.Height, lay.Height);
        if (draw) {
            gp::SolidBrush br(c);
            gp::RectF dr(x, y, w, h + 1);
            g.DrawString(t.c_str(), (INT)t.size(), f, dr, &fmt, &br);
        }
        delete f;
        return h;
    };
    auto cardBegin = [&](int i) {
        y += 8 * s;
        if (draw && g_lkCardBottom[i] > g_lkCardTop[i]) {
            gp::GraphicsPath path;
            AddRoundRect(path,
                         gp::RectF(outer, g_lkCardTop[i], W - 2 * outer,
                                   g_lkCardBottom[i] - g_lkCardTop[i]),
                         10 * s);
            gp::SolidBrush tile(kLkTile);
            g.FillPath(&tile, &path);
        }
        if (!draw) g_lkCardTop[i] = y;
        y += pad - 1 * s;
    };
    auto cardEnd = [&](int i) {
        y += pad;
        if (!draw) g_lkCardBottom[i] = y;
    };
    auto label = [&](const wchar_t* t) {
        y += text(t, LkFont(t, 11 * s, gp::FontStyleRegular, true), kLkMuted, x0, cw, 1) +
             6 * s;
    };

    // Header: the word, on the surface itself (a phrase smaller, on more lines).
    const bool phrase = g_pop.mode == LkMode::Phrase;
    y += text(g_pop.word, LkFont(g_pop.word, (phrase ? 15 : 20) * s, gp::FontStyleRegular, true),
              kLkFg, outer + 4 * s, W - 2 * outer - 8 * s, phrase ? 3 : 2) +
         2 * s;

    // Dictionary (English words only).
    if (g_pop.mode == LkMode::Word) {
        cardBegin(0);
        label(L"Dictionary");
        if (g_pop.dict == LkState::Pending) {
            y += text(L"Looking up\x2026", LkFont(L"", 13 * s, gp::FontStyleRegular), kLkMuted,
                      x0, cw, 1);
        } else if (g_pop.senses.empty()) {
            y += text(L"No entry in the dictionary.", LkFont(L"", 13 * s, gp::FontStyleRegular),
                      kLkMuted, x0, cw, 1);
        } else {
            std::wstring lastHead;
            int shown = 0, perPos = 0;
            std::wstring lw = g_pop.word;
            for (auto& c : lw) c = towlower(c);
            for (const auto& sn : g_pop.senses) {
                std::wstring head = sn.lemma != lw ? sn.lemma + L"  \x00B7  " + sn.pos : sn.pos;
                if (head != lastHead) {
                    if (shown >= L.total) break;
                    if (!lastHead.empty()) y += 6 * s;
                    y += text(head, LkFont(head, 12.5f * s, gp::FontStyleItalic), kLkMuted, x0, cw,
                              1) +
                         3 * s;
                    lastHead = head;
                    perPos = 0;
                }
                if (perPos >= L.perPos || shown >= L.total) continue;
                perPos++;
                shown++;
                std::wstring num = std::to_wstring(perPos);
                float ind = 16 * s;
                text(num, LkFont(num, 13.5f * s, gp::FontStyleRegular), kLkMuted, x0, ind, 1);
                y += text(sn.def, LkFont(sn.def, 13.5f * s, gp::FontStyleRegular), kLkFg, x0 + ind,
                          cw - ind, 3) +
                     2 * s;
                if (L.examples && !sn.example.empty()) {
                    std::wstring ex = L"\x201C" + sn.example + L"\x201D";
                    y += text(ex, LkFont(ex, 12.5f * s, gp::FontStyleItalic), kLkMuted, x0 + ind,
                              cw - ind, 2) +
                         2 * s;
                }
                if (L.syns && perPos == 1 && !sn.syns.empty()) {
                    std::wstring sy = L"Synonyms: " + sn.syns;
                    y += text(sy, LkFont(sy, 12 * s, gp::FontStyleRegular), kLkFaint, x0 + ind,
                              cw - ind, 1) +
                         2 * s;
                }
                y += 3 * s;
            }
            y -= 3 * s;
        }
        cardEnd(0);
    }

    // Wikipedia (the whole card opens the article).
    g_pop.wikiHit = {};
    if (g_pop.wiki != LkState::None) {
        cardBegin(1);
        float top = y;
        label(L"Wikipedia");
        if (g_pop.wiki == LkState::Pending) {
            y += text(L"Searching\x2026", LkFont(L"", 13 * s, gp::FontStyleRegular), kLkMuted, x0,
                      cw, 1);
        } else {
            float th = g_pop.thumb ? 60 * s : 0;
            float tw = g_pop.thumb ? cw - th - 12 * s : cw;
            float ty = y;
            y += text(g_pop.wikiTitle,
                      LkFont(g_pop.wikiTitle, 14.5f * s, gp::FontStyleRegular, true), kLkFg, x0,
                      tw, 2) +
                 3 * s;
            y += text(g_pop.wikiText, LkFont(g_pop.wikiText, 13 * s, gp::FontStyleRegular),
                      kLkMuted, x0, tw, L.wikiLines);
            if (g_pop.thumb) {
                gp::RectF r(W - x0 - th, ty, th, th);
                if (draw) {
                    gp::GraphicsPath path;
                    AddRoundRect(path, r, 8 * s);
                    gp::TextureBrush tb(g_pop.thumb.get());
                    gp::Matrix m;
                    m.Translate(r.X, r.Y);
                    m.Scale(th / g_pop.thumb->GetWidth(), th / g_pop.thumb->GetHeight());
                    tb.SetTransform(&m);
                    g.FillPath(&tb, &path);
                }
                y = std::max(y, ty + th);
            }
            g_pop.wikiHit = {(LONG)outer, (LONG)(top - pad), (LONG)(W - outer), (LONG)(y + pad)};
        }
        cardEnd(1);
    }

    // The local model: one card while it works, then one or two with its answer.
    const bool hasContext = LkHasContext(g_pop.word, g_pop.context);
    const wchar_t* kNoteLabel = hasContext ? L"In this sentence \x00B7 local model"
                                           : L"Meaning \x00B7 local model";
    auto aiCard = [&](int i, const wchar_t* lbl, const std::wstring& t, bool done, int lines) {
        cardBegin(i);
        label(lbl);
        y += text(t, LkFont(t, 13.5f * s, gp::FontStyleRegular), done ? kLkFg : kLkMuted, x0, cw,
                  lines);
        cardEnd(i);
    };
    if (g_pop.ai != LkState::Done) {
        const wchar_t* lbl = g_pop.mode == LkMode::Word     ? kNoteLabel
                             : g_pop.mode == LkMode::Native ? L"Meaning \x00B7 local model"
                                                            : L"Translation \x00B7 local model";
        aiCard(2, lbl, g_pop.aiText, false, L.aiLines);
    } else if (g_pop.mode == LkMode::Word) {
        aiCard(2, kNoteLabel, g_pop.aiText, true, L.aiLines);
    } else if (g_pop.mode == LkMode::Native) {
        int i = 2;
        if (!g_pop.aiMeaning.empty()) {
            aiCard(i++, L"Meaning \x00B7 local model", g_pop.aiMeaning, true, 3);
        }
        if (!g_pop.aiText.empty() && hasContext) {
            aiCard(i++, L"In this sentence \x00B7 local model", g_pop.aiText, true, L.aiLines);
        }
    } else {
        int i = 2;
        if (!g_pop.aiTranslation.empty()) {
            aiCard(i++, L"Translation \x00B7 local model", g_pop.aiTranslation, true,
                   L.aiLines * 2);
        }
        if (!g_pop.aiMeaning.empty()) {
            aiCard(i++, L"Meaning \x00B7 local model", g_pop.aiMeaning, true, L.aiLines);
        }
    }

    return y + outer;
}

static void LkFreeSurface() {
    if (g_popMem) {
        SelectObject(g_popMem, g_popOld);
        DeleteDC(g_popMem);
        g_popMem = nullptr;
    }
    if (g_popDib) {
        DeleteObject(g_popDib);
        g_popDib = nullptr;
    }
}

// msedge.exe from App Paths (64-bit view first: this process may be 32-bit),
// else the usual install folders. Empty if Edge isn't there.
static std::wstring LkEdgePath() {
    const REGSAM views[] = {KEY_WOW64_64KEY, KEY_WOW64_32KEY};
    const HKEY hives[] = {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER};
    for (HKEY hive : hives) {
        for (REGSAM v : views) {
            HKEY k;
            if (RegOpenKeyExW(hive,
                              L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\msedge.exe",
                              0, KEY_READ | v, &k) != ERROR_SUCCESS) {
                continue;
            }
            wchar_t buf[MAX_PATH * 2] = {};
            DWORD sz = sizeof(buf) - sizeof(wchar_t), type = 0;
            LONG r = RegQueryValueExW(k, nullptr, nullptr, &type, (BYTE*)buf, &sz);
            RegCloseKey(k);
            if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) continue;
            std::wstring p = buf;
            if (p.size() > 1 && p.front() == L'"') p = p.substr(1, p.find(L'"', 1) - 1);
            wchar_t ex[MAX_PATH * 2];
            if (ExpandEnvironmentStringsW(p.c_str(), ex, ARRAYSIZE(ex))) p = ex;
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
        }
    }
    const wchar_t* guesses[] = {
        L"%ProgramFiles(x86)%\\Microsoft\\Edge\\Application\\msedge.exe",
        L"%ProgramW6432%\\Microsoft\\Edge\\Application\\msedge.exe",
    };
    for (const wchar_t* gs : guesses) {
        wchar_t ex[MAX_PATH * 2];
        if (ExpandEnvironmentStringsW(gs, ex, ARRAYSIZE(ex)) &&
            GetFileAttributesW(ex) != INVALID_FILE_ATTRIBUTES) {
            return ex;
        }
    }
    return {};
}

// Opens a Wikipedia article in a compact Edge app window where the popover
// was (no tabs or toolbar), or in the default browser without Edge.
static void LkOpenArticle(const std::wstring& url, RECT pop, float scale) {
    // Only a plain https link: it goes on a command line inside quotes.
    if (url.rfind(L"https://", 0) != 0 || url.find_first_of(L"\" \t\r\n") != std::wstring::npos) {
        LkLog(L"not opening odd link");
        return;
    }
    std::wstring edge = LkEdgePath();
    if (!edge.empty()) {
        if (scale <= 0) scale = 1;
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromRect(&pop, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT& wa = mi.rcWork;
        int w = (int)std::lround(480 * scale), h = (int)std::lround(680 * scale);
        w = std::min(w, (int)(wa.right - wa.left));
        h = std::min(h, (int)(wa.bottom - wa.top));
        int x = std::clamp((int)pop.left, (int)wa.left, std::max((int)wa.left, (int)wa.right - w));
        int y = std::clamp((int)pop.top, (int)wa.top, std::max((int)wa.top, (int)wa.bottom - h));
        // Chromium reads these switches in DIPs.
        wchar_t args[2400];
        _snwprintf(args, ARRAYSIZE(args) - 1, L"--app=\"%s\" --window-size=%d,%d --window-position=%d,%d",
                   url.c_str(), (int)std::lround(w / scale), (int)std::lround(h / scale),
                   (int)std::lround(x / scale), (int)std::lround(y / scale));
        args[ARRAYSIZE(args) - 1] = 0;
        if ((INT_PTR)ShellExecuteW(nullptr, L"open", edge.c_str(), args, nullptr, SW_SHOWNORMAL) > 32) {
            LkLog(L"opened in an Edge app window: %s", url.c_str());
            return;
        }
        LkLog(L"Edge app window failed (%u), using the default browser", GetLastError());
    }
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static LRESULT CALLBACK LkPopProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_SETCURSOR: {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            SetCursor(LoadCursorW(nullptr, PtInRect(&g_pop.wikiHit, p) && !g_pop.wikiUrl.empty()
                                               ? IDC_HAND
                                               : IDC_ARROW));
            return TRUE;
        }
        case WM_LBUTTONUP: {
            POINT p{(short)LOWORD(lp), (short)HIWORD(lp)};
            if (PtInRect(&g_pop.wikiHit, p) && !g_pop.wikiUrl.empty()) {
                std::wstring url = g_pop.wikiUrl;
                RECT pop{g_popL, g_popT, g_popR, g_popB};
                float scale = g_pop.scale;
                LookupClose(L"opened Wikipedia");
                LkOpenArticle(url, pop, scale);
            }
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static bool LkEnsureWindow() {
    if (g_popHwnd) return true;
    LkDpiScope dpi;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = LkPopProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kLookupClassName;
    RegisterClassExW(&wc);
    // Layered and drawn by us with per-pixel alpha, like the bar's panels.
    g_popHwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                kLookupClassName, L"Lookup", WS_POPUP, 0, 0, 1, 1,
                                nullptr, nullptr, g_hInst, nullptr);
    return g_popHwnd != nullptr;
}

// Redraws the popover and fits the window to it.
static void LkRender(bool firstShow) {
    if (!LkEnsureWindow()) return;
    LkDpiScope dpi;
    const float s = g_pop.scale;
    const int W = (int)std::lround(372 * s);

    // The side of the word with more room, chosen once per lookup so the
    // popover doesn't jump while results arrive.
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&g_pop.anchor, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& wa = mi.rcWork;
    const int gap = (int)std::lround(8 * s);
    const int below = wa.bottom - gap - (g_pop.anchor.bottom + gap);
    const int aboveRoom = (g_pop.anchor.top - gap) - (wa.top + gap);
    if (firstShow) g_pop.above = below < (int)std::lround(460 * s) && aboveRoom > below;
    const int maxH = std::min((int)std::lround(680 * s), std::max(g_pop.above ? aboveRoom : below,
                                                                   (int)std::lround(160 * s)));

    float h = 0;
    int level = 0;
    {
        gp::Bitmap probe(1, 1, PixelFormat32bppPARGB);
        gp::Graphics g(&probe);
        g.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
        for (level = 0; level < 3; level++) {
            h = LkLayout(g, false, (float)W, 0, level);
            if (h <= maxH) break;
        }
        if (level == 3) level = 2;
    }
    // Still too tall at the smallest level: rather than cut the last card
    // off, let it grow over the word, up to the work area.
    const int H = std::min((int)std::ceil(h),
                           std::max(maxH, (int)(wa.bottom - wa.top) - 2 * gap));

    LkFreeSurface();
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    g_popDib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!g_popDib) {
        ReleaseDC(nullptr, screen);
        return;
    }
    g_popMem = CreateCompatibleDC(screen);
    g_popOld = SelectObject(g_popMem, g_popDib);
    g_popW = W;
    g_popH = H;
    {
        gp::Bitmap surf(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        gp::Graphics g(&surf);
        g.Clear(gp::Color(0, 0, 0, 0));
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
        g.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic);
        LkLayout(g, true, (float)W, (float)H, level);
    }
    GdiFlush();

    int x = g_pop.anchor.left - (int)std::lround(22 * s);
    int y = g_pop.above ? g_pop.anchor.top - gap - H : g_pop.anchor.bottom + gap;
    x = std::clamp(x, (int)wa.left + gap, std::max((int)wa.left + gap, (int)wa.right - gap - W));
    y = std::clamp(y, (int)wa.top + gap, std::max((int)wa.top + gap, (int)wa.bottom - gap - H));
    g_popL = x, g_popT = y, g_popR = x + W, g_popB = y + H;

    POINT pos{x, y}, src{0, 0};
    SIZE size{W, H};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_popHwnd, screen, &pos, &size, g_popMem, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
    SetWindowPos(g_popHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
}

static std::unique_ptr<gp::Bitmap> LkDecodeThumb(const std::string& bytes, int px) {
    if (bytes.empty()) return nullptr;
    IStream* st = SHCreateMemStream((const BYTE*)bytes.data(), (UINT)bytes.size());
    if (!st) return nullptr;
    std::unique_ptr<gp::Bitmap> out;
    {
        gp::Bitmap src(st);
        if (src.GetLastStatus() == gp::Ok && src.GetWidth() && src.GetHeight()) {
            out = std::make_unique<gp::Bitmap>(px, px, PixelFormat32bppPARGB);
            gp::Graphics g(out.get());
            g.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic);
            float sw = (float)src.GetWidth(), sh = (float)src.GetHeight();
            float side = std::min(sw, sh);  // centre square crop
            g.DrawImage(&src, gp::RectF(0, 0, (float)px, (float)px), (sw - side) / 2,
                        (sh - side) / 4, side, side, gp::UnitPixel);
        }
    }
    st->Release();
    return out;
}

static void OnLookupResult(LkMsg* m) {
    std::unique_ptr<LkMsg> own(m);
    if (m->gen != g_lookupGen) return;
    switch (m->kind) {
        case LkKind::NotFound:
            return;
        case LkKind::Word: {
            g_pop.gen = m->gen;
            g_pop.word = m->a;
            g_pop.context = m->b;
            g_pop.anchor = m->rect;
            if (IsRectEmpty(&g_pop.anchor)) {
                g_pop.anchor = {g_lookupPt.x, g_lookupPt.y, g_lookupPt.x + 1, g_lookupPt.y + 12};
            }
            g_pop.scale = GetMonitorScale(MonitorFromRect(&g_pop.anchor, MONITOR_DEFAULTTONEAREST));
            g_pop.dict = LkState::Pending;
            g_pop.senses.clear();
            g_pop.wiki = LkState::Pending;
            g_pop.wikiTitle.clear();
            g_pop.wikiText.clear();
            g_pop.wikiUrl.clear();
            g_pop.thumb.reset();
            g_pop.ai = LkState::Pending;
            g_pop.aiText = L"Thinking\x2026";
            g_pop.aiMeaning.clear();
            g_pop.aiTranslation.clear();
            g_pop.mode = m->mode;
            if (g_pop.mode == LkMode::Phrase) g_pop.wiki = LkState::None;  // not looked up
            LkRender(true);
            LkLog(L"popover shown for '%s' (context %u chars)", g_pop.word.c_str(),
                  (unsigned)g_pop.context.size());
            g_popShownAt = GetTickCount64();
            if (!g_lookupVisible) {
                g_lookupVisible = true;
                if (g_hookThreadId) PostThreadMessageW(g_hookThreadId, WM_RS_LOOKUP_MHOOK, 1, 0);
            }
            if (!g_popFgHook) {
                g_popFgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                              nullptr, LkForegroundProc, 0, 0,
                                              WINEVENT_OUTOFCONTEXT);
            }
            return;
        }
        case LkKind::Dict:
            g_pop.senses = std::move(m->senses);
            g_pop.dict = LkState::Done;
            break;
        case LkKind::Wiki:
            if (m->ok) {
                g_pop.wiki = LkState::Done;
                g_pop.wikiTitle = m->a;
                g_pop.wikiText = m->b;
                g_pop.wikiUrl = m->c;
                g_pop.thumb = LkDecodeThumb(m->bytes, (int)std::lround(128 * g_pop.scale));
            } else {
                g_pop.wiki = LkState::None;
            }
            break;
        case LkKind::AiStatus:
            if (g_pop.ai == LkState::Pending) g_pop.aiText = m->a;
            break;
        case LkKind::Ai:
            g_pop.ai = m->ok ? LkState::Done : LkState::None;
            g_pop.aiText = m->a;
            g_pop.aiMeaning = m->b;
            g_pop.aiTranslation = m->c;
            break;
    }
    if (g_lookupVisible) LkRender(false);
}

// A tap, or a test request with a point (wp = 1, lp = x, y).
static void OnLookupTap(WPARAM wp, LPARAM lp) {
    if (g_visible || g_pending) return;  // the ring is open
    POINT pt{};
    if (wp == 1) {
        pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
    } else {
        GetPhysicalCursorPos(&pt);
    }
    if (g_lookupVisible) {
        bool sameSpot = std::abs(pt.x - g_lookupPt.x) < 24 && std::abs(pt.y - g_lookupPt.y) < 24;
        LookupClose(L"tap");
        if (sameSpot) return;  // the same tap again closes it
    }
    g_lookupPt = pt;
    LkLog(L"lookup %u at %d,%d (%s)", g_lookupGen + 1, pt.x, pt.y, wp == 1 ? L"test" : L"tap");
    auto* job = new LkJob{++g_lookupGen, pt};
    LkStartThread(LkLookupThread, job);
}

// Tap detection, fed with every touchpad frame (see OnTouchFrame).
static struct {
    bool down = false, candidate = false, seen3 = false;
    int maxFingers = 0;
    ULONGLONG start = 0;
    double x3 = 0, y3 = 0;
} g_tap;

static void LookupTapFrame(int fingers, double x, double y) {
    auto& t = g_tap;
    if (!g_settings.lookupTap) return;
    if (fingers > 0) {
        if (!t.down) {
            t = {};
            t.down = true;
            t.candidate = true;
            t.start = GetTickCount64();
        }
        t.maxFingers = std::max(t.maxFingers, fingers);
        if (fingers > 3 || GetTickCount64() - t.start > kTapMaxMs) t.candidate = false;
        if (fingers == 3) {
            if (!t.seen3) {
                t.seen3 = true;
                t.x3 = x, t.y3 = y;
            } else if (std::fabs(x - t.x3) > kTapMaxMove || std::fabs(y - t.y3) > kTapMaxMove) {
                t.candidate = false;
            }
        }
        return;
    }
    if (t.down && t.candidate && t.maxFingers == 3 && t.seen3 &&
        GetTickCount64() - t.start <= kTapMaxMs) {
        if (HWND h = g_hwnd) PostMessageW(h, WM_RS_LOOKUP_TAP, 0, 0);
    }
    t = {};
}

static void LookupShutdownUi() {
    g_lookupQuit = true;
    LookupClose();
    LkFreeSurface();
    if (g_popHwnd) {
        DestroyWindow(g_popHwnd);
        g_popHwnd = nullptr;
    }
    UnregisterClassW(kLookupClassName, g_hInst);
    g_pop.thumb.reset();
    LkWnClose();
}

