// ---- Shelf and Dropshelf (P6c): a page of dropped things, kept on D:, with OLE drop on every bubble ----
// Rows are addressed by item id, never by path or position: a drag blocks in DoDragDrop while the list can
// change, and the old tray removed every text row because they all shared an empty path.
// Everything here runs on the bar's thread.

unsigned IslandSurfaceGen();  // island.inc
void IslandOpenShelf();       // island.inc
void IslandRepaint();         // island.inc

enum class SK { File, Text, Link, Image, Stored };

struct ShelfItem {
    unsigned id = 0;
    SK kind = SK::File;
    std::wstring title;
    std::wstring path;  // File: the original. Text/Image/Stored: blob file name inside the blob folder ("" if inline).
    std::wstring text;  // Text: body when inline. Link: the URL.
    unsigned long long size = 0;
    unsigned long long addedAt = 0;
    bool isDir = false, missing = false;
    Pixmap* icon = nullptr;
    bool iconTried = false;
    std::shared_ptr<Pixmap> thumb;  // Small shell or WIC thumbnail, made off the bar thread.
    bool thumbAsked = false;
};

constexpr size_t kShelfMaxItems = 100;
constexpr unsigned long long kShelfBlobBudget = 2ULL << 30;
constexpr unsigned long long kShelfItemCap = 512ULL << 20;

bool g_dropActive = false;       // A drag is over the bar or the island.
ULONGLONG g_dropLeft = 0;        // When it last left.
bool g_shelfInternalDrag = false;  // Our own drag out: dropping it back on the bar does nothing.
bool g_shelfChoiceOpen = false;    // The keep-or-send card after a drop is up: the island stays open for it.

bool ShelfHoldsOpen() {
    return g_shelfChoiceOpen || g_dropActive || g_shelfInternalDrag || (g_dropLeft && GetTickCount64() - g_dropLeft < 300);
}

std::wstring ShelfDir() { return DataRoot() + L"\\shelf"; }
std::wstring BlobDir() { return ShelfDir() + L"\\blobs"; }

std::wstring EscapeField(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'\\') o += L"\\\\";
        else if (c == L'\t') o += L"\\t";
        else if (c == L'\n') o += L"\\n";
        else if (c == L'\r') o += L"\\r";
        else o += c;
    }
    return o;
}

std::wstring UnescapeField(const std::wstring& s) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            const wchar_t n = s[++i];
            o += n == L't' ? L'\t' : n == L'n' ? L'\n' : n == L'r' ? L'\r' : n;
        } else {
            o += s[i];
        }
    }
    return o;
}

bool SafeBlobName(const std::wstring& n) {
    return !n.empty() && n.find_first_of(L"\\/:") == std::wstring::npos && n.find(L"..") == std::wstring::npos;
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring FileNameOf(const std::wstring& p) {
    const size_t at = p.find_last_of(L"\\/");
    return at == std::wstring::npos ? p : p.substr(at + 1);
}

std::wstring ExtOf(const std::wstring& name) {
    const size_t at = name.find_last_of(L'.');
    return at == std::wstring::npos || name.size() - at > 8 ? L"" : Lower(name.substr(at));
}

bool IsImageExt(const std::wstring& e) {
    return e == L".png" || e == L".jpg" || e == L".jpeg" || e == L".gif" || e == L".bmp" || e == L".webp" || e == L".tif" || e == L".tiff";
}

bool LooksLikeLink(const std::wstring& t) {
    if (t.find_first_of(L" \t\r\n") != std::wstring::npos) return false;
    const std::wstring l = Lower(t);
    return l.rfind(L"http://", 0) == 0 || l.rfind(L"https://", 0) == 0 || l.rfind(L"mailto:", 0) == 0;
}

std::wstring OneLine(const std::wstring& t, size_t cap) {
    std::wstring o;
    bool space = false;
    for (wchar_t c : t) {
        if (c == L'\r' || c == L'\n' || c == L'\t' || c == L' ') { space = !o.empty(); continue; }
        if (space) { o += L' '; space = false; }
        o += c;
        if (o.size() >= cap) { o += L"…"; break; }
    }
    return o;
}

std::wstring FormatSize(unsigned long long b) {
    wchar_t buf[32];
    if (b >= (1ULL << 30)) swprintf_s(buf, L"%.1f GB", b / 1073741824.0);
    else if (b >= (1ULL << 20)) swprintf_s(buf, L"%.1f MB", b / 1048576.0);
    else if (b >= 1024) swprintf_s(buf, L"%.0f KB", b / 1024.0);
    else swprintf_s(buf, L"%llu B", b);
    return buf;
}

// ---- Send to phone: the PCTracker app's Explorer verb, read at run time ----
// The verb's command only drops a request in PCTracker's outbox and exits; the agent does the sending.
// No verb registered: no phone option anywhere.
void AlertsPhoneSent(int sent, int failed);  // alerts.inc
void SetExpanded(bool expanded);             // island.inc

struct PhoneVerb {
    std::wstring file, dir;
    ULONGLONG readAt = 0;
};
PhoneVerb g_phoneVerb;

std::wstring ReadVerbCommand(const wchar_t* key) {
    wchar_t buf[2048];
    DWORD size = sizeof(buf), type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key, nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type, buf, &size) != ERROR_SUCCESS) return L"";
    std::wstring cmd = buf;
    if (type == REG_EXPAND_SZ) {
        wchar_t out[4096];
        const DWORD n = ExpandEnvironmentStringsW(buf, out, 4096);
        if (n && n <= 4096) cmd = out;
    }
    while (!cmd.empty() && iswspace(cmd.back())) cmd.pop_back();
    return cmd;
}

// Cached for 5 s: rows ask while they draw.
const std::wstring& PhoneCommand(bool dir) {
    const ULONGLONG now = GetTickCount64();
    if (!g_phoneVerb.readAt || now - g_phoneVerb.readAt > 5000) {
        g_phoneVerb.readAt = now;
        g_phoneVerb.file = ReadVerbCommand(L"Software\\Classes\\*\\shell\\PCTrackerSendToPhone\\command");
        g_phoneVerb.dir = ReadVerbCommand(L"Software\\Classes\\Directory\\shell\\PCTrackerSendToPhone\\command");
    }
    return dir ? g_phoneVerb.dir : g_phoneVerb.file;
}

// The verb's command with "%1" replaced by the quoted path. No window, not waited for.
bool LaunchPhoneSend(const std::wstring& path, bool dir) {
    const std::wstring& cmd = PhoneCommand(dir);
    if (cmd.empty() || path.empty()) return false;
    const std::wstring q = L"\"" + path + L"\"";
    std::wstring line = cmd;
    size_t at = line.find(L"\"%1\"");
    if (at != std::wstring::npos) line.replace(at, 4, q);
    else if ((at = line.find(L"%1")) != std::wstring::npos) line.replace(at, 2, q);
    else line += L" " + q;
    std::wstring cwd;  // The helper's own folder, when the command names it in quotes.
    if (line[0] == L'"') {
        const size_t e = line.find(L'"', 1);
        if (e != std::wstring::npos) {
            cwd = line.substr(1, e - 1);
            const size_t s = cwd.find_last_of(L"\\/");
            cwd = s == std::wstring::npos ? L"" : cwd.substr(0, s);
        }
    }
    const DWORD ca = cwd.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(cwd.c_str());
    if (ca == INVALID_FILE_ATTRIBUTES || !(ca & FILE_ATTRIBUTE_DIRECTORY)) cwd.clear();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &line[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        Wh_Log(L"top-bar: send to phone failed to start, error %lu", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool CopyTextToClipboard(const std::wstring& t) {
    if (!OpenClipboard(g_hwnd)) return false;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
    bool ok = false;
    if (mem) {
        if (void* p = GlobalLock(mem)) {
            memcpy(p, t.c_str(), (t.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        }
        if (!ok) GlobalFree(mem);
    }
    CloseClipboard();
    return ok;
}

// DIB bytes (as on the clipboard) to a PNG file, through WIC.
bool DibToPng(const std::string& dib, const std::wstring& out) {
    if (dib.size() < sizeof(BITMAPINFOHEADER)) return false;
    const auto* bi = reinterpret_cast<const BITMAPINFOHEADER*>(dib.data());
    DWORD off = 14 + bi->biSize;
    if (bi->biBitCount <= 8) off += (bi->biClrUsed ? bi->biClrUsed : 1u << bi->biBitCount) * 4;
    else if (bi->biCompression == BI_BITFIELDS && bi->biSize == 40) off += 12;
    std::string bmp(14, '\0');
    bmp[0] = 'B';
    bmp[1] = 'M';
    const DWORD total = (DWORD)(dib.size() + 14);
    memcpy(&bmp[2], &total, 4);
    memcpy(&bmp[10], &off, 4);
    bmp += dib;
    Com<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), reinterpret_cast<void**>(wic.Put())))) return false;
    Com<IWICStream> in, file;
    Com<IWICBitmapDecoder> dec;
    Com<IWICBitmapFrameDecode> frame;
    Com<IWICBitmapEncoder> enc;
    Com<IWICBitmapFrameEncode> fr;
    Com<IPropertyBag2> props;
    if (FAILED(wic->CreateStream(in.Put())) || FAILED(in->InitializeFromMemory(reinterpret_cast<BYTE*>(&bmp[0]), (DWORD)bmp.size())) ||
        FAILED(wic->CreateDecoderFromStream(in.Get(), nullptr, WICDecodeMetadataCacheOnDemand, dec.Put())) || FAILED(dec->GetFrame(0, frame.Put())) ||
        FAILED(wic->CreateStream(file.Put())) || FAILED(file->InitializeFromFilename(out.c_str(), GENERIC_WRITE)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.Put())) || FAILED(enc->Initialize(file.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(fr.Put(), props.Put())) || FAILED(fr->Initialize(props.Get())) || FAILED(fr->WriteSource(frame.Get(), nullptr)) ||
        FAILED(fr->Commit()) || FAILED(enc->Commit())) {
        file.Reset();
        DeleteFileW(out.c_str());
        return false;
    }
    return true;
}

// ---- Reading a data object. Only called from Drop or paste; QueryGetData is all DragEnter uses. ----
CLIPFORMAT g_cfDesc, g_cfContents, g_cfUrlW, g_cfUrlA, g_cfPng;

void InitFormats() {
    if (g_cfDesc) return;
    g_cfDesc = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
    g_cfContents = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_FILECONTENTS);
    g_cfUrlW = (CLIPFORMAT)RegisterClipboardFormatW(L"UniformResourceLocatorW");
    g_cfUrlA = (CLIPFORMAT)RegisterClipboardFormatW(L"UniformResourceLocator");
    g_cfPng = (CLIPFORMAT)RegisterClipboardFormatW(L"PNG");
}

constexpr DWORD kTymeds = TYMED_HGLOBAL | TYMED_ISTREAM;

bool HasFormat(IDataObject* d, CLIPFORMAT cf, DWORD tymed = kTymeds, LONG idx = -1) {
    FORMATETC f{cf, nullptr, DVASPECT_CONTENT, idx, tymed};
    return d->QueryGetData(&f) == S_OK;
}

// Reads a format into memory (cap bytes), or straight to a file when `file` is set.
bool ReadFormat(IDataObject* d, CLIPFORMAT cf, LONG idx, std::string* mem, const std::wstring* file, unsigned long long cap, unsigned long long* sizeOut = nullptr) {
    FORMATETC f{cf, nullptr, DVASPECT_CONTENT, idx, kTymeds};
    STGMEDIUM m{};
    if (FAILED(d->GetData(&f, &m))) return false;
    bool ok = false;
    HANDLE h = INVALID_HANDLE_VALUE;
    if (file) {
        h = CreateFileW(file->c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) { ReleaseStgMedium(&m); return false; }
    }
    unsigned long long total = 0;
    auto sink = [&](const void* p, size_t n) -> bool {
        total += n;
        if (total > cap) return false;
        if (file) { DWORD w = 0; return WriteFile(h, p, (DWORD)n, &w, nullptr) && w == n; }
        mem->append(static_cast<const char*>(p), n);
        return true;
    };
    if (m.tymed == TYMED_HGLOBAL && m.hGlobal) {
        if (void* p = GlobalLock(m.hGlobal)) {
            ok = sink(p, GlobalSize(m.hGlobal));
            GlobalUnlock(m.hGlobal);
        }
    } else if (m.tymed == TYMED_ISTREAM && m.pstm) {
        char buf[65536];
        ok = true;
        for (;;) {
            ULONG got = 0;
            const HRESULT hr = m.pstm->Read(buf, sizeof(buf), &got);
            if (got && !sink(buf, got)) { ok = false; break; }
            if (hr != S_OK || got == 0) break;
        }
    }
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        if (!ok) DeleteFileW(file->c_str());
    }
    ReleaseStgMedium(&m);
    if (ok && sizeOut) *sizeOut = total;
    return ok;
}

bool AcceptsData(IDataObject* d) {
    InitFormats();
    return HasFormat(d, CF_HDROP, TYMED_HGLOBAL) || (HasFormat(d, g_cfDesc, TYMED_HGLOBAL) && HasFormat(d, g_cfContents, kTymeds | TYMED_ISTORAGE, 0)) ||
           HasFormat(d, g_cfUrlW) || HasFormat(d, g_cfUrlA) || HasFormat(d, CF_UNICODETEXT) || HasFormat(d, CF_TEXT) || HasFormat(d, g_cfPng) ||
           HasFormat(d, CF_DIBV5) || HasFormat(d, CF_DIB);
}

DWORD PickDropEffect(DWORD allowed) {  // Never MOVE: the source keeps its data.
    if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
    return DROPEFFECT_NONE;
}

// ---- Thumbnails: one worker thread, started on demand, gone after 20 s idle ----
// IShellItemImageFactory gives thumbnails only (no icons: the row already has the shell icon). Images it
// cannot thumbnail are decoded with WIC. Results are small premultiplied pixmaps, posted to the bar thread.
void ShelfThumbReady(unsigned id, std::shared_ptr<Pixmap> pm);  // after the page

struct ThumbJob {
    unsigned id;
    std::wstring path;
    bool image;
    int px;
};

SRWLOCK g_thumbLock = SRWLOCK_INIT;
std::deque<ThumbJob> g_thumbQueue;
HANDLE g_thumbThread = nullptr;
HANDLE g_thumbEvent = nullptr;
bool g_thumbRunning = false, g_thumbStop = false;

// Scales any WIC source to fit px x px and copies it out as premultiplied BGRA.
std::shared_ptr<Pixmap> ThumbFromSource(IWICImagingFactory* wic, IWICBitmapSource* src, int px) {
    UINT w = 0, h = 0;
    if (FAILED(src->GetSize(&w, &h)) || !w || !h) return nullptr;
    const double f = std::min(1.0, std::min((double)px / w, (double)px / h));
    const UINT tw = std::max(1u, (UINT)(w * f + 0.5)), th = std::max(1u, (UINT)(h * f + 0.5));
    Com<IWICFormatConverter> conv;
    Com<IWICBitmapScaler> scaler;
    if (FAILED(wic->CreateFormatConverter(conv.Put())) ||
        FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
        FAILED(wic->CreateBitmapScaler(scaler.Put())) || FAILED(scaler->Initialize(conv.Get(), tw, th, WICBitmapInterpolationModeFant)))
        return nullptr;
    auto pm = std::make_shared<Pixmap>();
    pm->w = (int)tw;
    pm->h = (int)th;
    pm->px.resize((size_t)tw * th);
    if (FAILED(scaler->CopyPixels(nullptr, tw * 4, tw * th * 4, reinterpret_cast<BYTE*>(pm->px.data())))) return nullptr;
    return pm;
}

std::shared_ptr<Pixmap> ThumbFromShell(IWICImagingFactory* wic, const ThumbJob& j) {
    Com<IShellItemImageFactory> fac;
    if (FAILED(SHCreateItemFromParsingName(j.path.c_str(), nullptr, IID_PPV_ARGS(fac.Put())))) return nullptr;
    HBITMAP hbm = nullptr;
    if (FAILED(fac->GetImage(SIZE{j.px, j.px}, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &hbm)) || !hbm) return nullptr;
    std::shared_ptr<Pixmap> out;
    BITMAP bm{};
    if (GetObjectW(hbm, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0 && (long long)bm.bmWidth * bm.bmHeight <= 4096LL * 4096) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = bm.bmWidth;
        bi.bmiHeader.biHeight = -bm.bmHeight;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<DWORD> pix((size_t)bm.bmWidth * bm.bmHeight);
        HDC dc = GetDC(nullptr);
        const bool got = GetDIBits(dc, hbm, 0, bm.bmHeight, pix.data(), &bi, DIB_RGB_COLORS) != 0;
        ReleaseDC(nullptr, dc);
        if (got) {
            bool alpha = false;
            for (DWORD v : pix) if (v >> 24) { alpha = true; break; }
            for (DWORD& v : pix) {
                if (!alpha) { v |= 0xFF000000; continue; }  // Photos come back with alpha 0: opaque.
                const DWORD a = v >> 24;                    // Otherwise premultiplied; keep channels within alpha.
                const DWORD r = std::min((v >> 16) & 0xFF, a), g = std::min((v >> 8) & 0xFF, a), b = std::min(v & 0xFF, a);
                v = (a << 24) | (r << 16) | (g << 8) | b;
            }
            Com<IWICBitmap> wb;
            if (SUCCEEDED(wic->CreateBitmapFromMemory(bm.bmWidth, bm.bmHeight, GUID_WICPixelFormat32bppPBGRA, bm.bmWidth * 4,
                                                      (UINT)(pix.size() * 4), reinterpret_cast<BYTE*>(pix.data()), wb.Put())))
                out = ThumbFromSource(wic, wb.Get(), j.px);
        }
    }
    DeleteObject(hbm);
    return out;
}

std::shared_ptr<Pixmap> ThumbFromWic(IWICImagingFactory* wic, const ThumbJob& j) {
    Com<IWICBitmapDecoder> dec;
    Com<IWICBitmapFrameDecode> frame;
    if (FAILED(wic->CreateDecoderFromFilename(j.path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, dec.Put())) ||
        FAILED(dec->GetFrame(0, frame.Put())))
        return nullptr;
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || (unsigned long long)w * h > 64000000ULL) return nullptr;  // Skip huge images.
    return ThumbFromSource(wic, frame.Get(), j.px);
}

DWORD WINAPI ThumbThread(void*) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Com<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), reinterpret_cast<void**>(wic.Put()));
    bool idle = false;
    for (;;) {
        AcquireSRWLockExclusive(&g_thumbLock);
        if (g_thumbStop || (g_thumbQueue.empty() && idle) || !wic) {
            g_thumbRunning = false;
            g_thumbQueue.clear();
            ReleaseSRWLockExclusive(&g_thumbLock);
            break;
        }
        if (g_thumbQueue.empty()) {
            ReleaseSRWLockExclusive(&g_thumbLock);
            idle = WaitForSingleObject(g_thumbEvent, 20000) == WAIT_TIMEOUT;
            continue;
        }
        ThumbJob j = std::move(g_thumbQueue.front());
        g_thumbQueue.pop_front();
        ReleaseSRWLockExclusive(&g_thumbLock);
        idle = false;
        std::shared_ptr<Pixmap> pm = ThumbFromShell(wic.Get(), j);
        if (!pm && j.image) pm = ThumbFromWic(wic.Get(), j);
        // Posted even on failure, so the row stops waiting. The heap lambda owns the pixmap until it runs.
        auto* fn = new std::function<void()>([id = j.id, pm] { ShelfThumbReady(id, pm); });
        HWND hwnd = g_hwnd;
        if (!hwnd || !PostMessageW(hwnd, WM_APP_POST, 0, reinterpret_cast<LPARAM>(fn))) delete fn;
    }
    wic.Reset();
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}

bool ThumbRequest(ThumbJob j) {
    if (!g_thumbEvent) g_thumbEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_thumbEvent) return false;
    bool ok = true;
    AcquireSRWLockExclusive(&g_thumbLock);
    if (g_thumbStop) ok = false;
    else {
        g_thumbQueue.push_back(std::move(j));
        if (!g_thumbRunning) {
            if (g_thumbThread) CloseHandle(g_thumbThread);  // A worker that went idle has exited.
            g_thumbThread = CreateThread(nullptr, 0, ThumbThread, nullptr, 0, nullptr);
            g_thumbRunning = g_thumbThread != nullptr;
            if (!g_thumbRunning) { g_thumbQueue.clear(); ok = false; }
        }
    }
    ReleaseSRWLockExclusive(&g_thumbLock);
    if (ok) SetEvent(g_thumbEvent);
    return ok;
}

// Bar thread, before the window's resources go: the worker runs code from this DLL.
void ShelfShutdown() {
    AcquireSRWLockExclusive(&g_thumbLock);
    g_thumbStop = true;
    g_thumbQueue.clear();
    HANDLE t = g_thumbThread;
    g_thumbThread = nullptr;
    ReleaseSRWLockExclusive(&g_thumbLock);
    if (g_thumbEvent) SetEvent(g_thumbEvent);
    if (t) {
        WaitForSingleObject(t, 3000);
        CloseHandle(t);
    }
    if (g_thumbEvent) { CloseHandle(g_thumbEvent); g_thumbEvent = nullptr; }
}

// Uploads a pixmap for the current render target when needed.
ID2D1Bitmap* PixmapBitmap(const Pg& p, Pixmap& px) {
    const unsigned gen = IslandSurfaceGen();
    if (!px.bmp || px.gen != gen) {
        px.bmp.Reset();
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (SUCCEEDED(p.c.rt->CreateBitmap(D2D1::SizeU(px.w, px.h), px.px.data(), px.w * 4, &bp, px.bmp.Put()))) px.gen = gen;
    }
    return px.bmp.Get();
}

// ---- Drag-out data helpers ----
HGLOBAL MakeHGlobal(const void* data, size_t bytes) {
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes ? bytes : 1);
    if (!mem) return nullptr;
    if (void* p = GlobalLock(mem)) {
        if (bytes) memcpy(p, data, bytes);
        GlobalUnlock(mem);
        return mem;
    }
    GlobalFree(mem);
    return nullptr;
}

bool SetHGlobal(IDataObject* d, CLIPFORMAT cf, const void* data, size_t bytes, LONG index = -1) {
    HGLOBAL mem = MakeHGlobal(data, bytes);
    if (!mem) return false;
    FORMATETC f{cf, nullptr, DVASPECT_CONTENT, index, TYMED_HGLOBAL};
    STGMEDIUM m{};
    m.tymed = TYMED_HGLOBAL;
    m.hGlobal = mem;
    if (FAILED(d->SetData(&f, &m, TRUE))) { GlobalFree(mem); return false; }  // Owned by the object on success.
    return true;
}

bool SetText(IDataObject* d, const std::wstring& t) {
    bool ok = SetHGlobal(d, CF_UNICODETEXT, t.c_str(), (t.size() + 1) * sizeof(wchar_t));
    const int n = WideCharToMultiByte(CP_ACP, 0, t.c_str(), (int)t.size() + 1, nullptr, 0, nullptr, nullptr);
    if (n > 0) {
        std::string a((size_t)n, '\0');
        WideCharToMultiByte(CP_ACP, 0, t.c_str(), (int)t.size() + 1, &a[0], n, nullptr, nullptr);
        SetHGlobal(d, CF_TEXT, a.data(), a.size());
    }
    return ok;
}

// One virtual file (descriptor + contents at index 0): Explorer and upload targets get a real file.
bool SetVirtualFile(IDataObject* d, const std::wstring& name, const std::string& bytes) {
    InitFormats();
    FILEGROUPDESCRIPTORW g{};
    g.cItems = 1;
    FILEDESCRIPTORW& fd = g.fgd[0];
    fd.dwFlags = FD_FILESIZE | FD_PROGRESSUI | FD_ATTRIBUTES;
    fd.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    fd.nFileSizeLow = (DWORD)bytes.size();
    fd.nFileSizeHigh = (DWORD)((unsigned long long)bytes.size() >> 32);
    wcsncpy_s(fd.cFileName, name.c_str(), _TRUNCATE);
    if (!SetHGlobal(d, g_cfDesc, &g, sizeof(g))) return false;
    return SetHGlobal(d, g_cfContents, bytes.data(), bytes.size(), 0);
}

bool SetHDrop(IDataObject* d, const std::wstring& path) {
    std::vector<BYTE> buf(sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t), 0);
    auto* df = reinterpret_cast<DROPFILES*>(buf.data());
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    memcpy(buf.data() + sizeof(DROPFILES), path.c_str(), path.size() * sizeof(wchar_t));
    return SetHGlobal(d, CF_HDROP, buf.data(), buf.size());
}

// A file name made from a row title: no separators or reserved characters, short.
std::wstring SafeFileName(const std::wstring& title, const wchar_t* fallback, const wchar_t* ext) {
    std::wstring o;
    for (wchar_t c : OneLine(title, 60)) o += (c < 32 || wcschr(L"\\/:*?\"<>|", c)) ? L'_' : c;
    while (!o.empty() && (o.back() == L' ' || o.back() == L'.' || o.back() == L'…')) o.pop_back();
    while (!o.empty() && o.front() == L' ') o.erase(0, 1);
    return (o.empty() ? std::wstring(fallback) : o) + ext;
}

// The picture as CF_DIB (BITMAPINFOHEADER + bottom-up 32 bpp rows), for apps that paste pictures. Capped at 4K.
bool ImageDib(const std::wstring& path, std::string& out) {
    Com<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), reinterpret_cast<void**>(wic.Put())))) return false;
    Com<IWICBitmapDecoder> dec;
    Com<IWICBitmapFrameDecode> frame;
    Com<IWICFormatConverter> conv;
    UINT w = 0, h = 0;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, dec.Put())) ||
        FAILED(dec->GetFrame(0, frame.Put())) || FAILED(frame->GetSize(&w, &h)) || !w || !h || (unsigned long long)w * h > 8400000ULL ||
        FAILED(wic->CreateFormatConverter(conv.Put())) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;
    const size_t stride = (size_t)w * 4;
    out.assign(sizeof(BITMAPINFOHEADER) + stride * h, '\0');
    auto* bi = reinterpret_cast<BITMAPINFOHEADER*>(&out[0]);
    bi->biSize = sizeof(BITMAPINFOHEADER);
    bi->biWidth = (LONG)w;
    bi->biHeight = (LONG)h;
    bi->biPlanes = 1;
    bi->biBitCount = 32;
    bi->biCompression = BI_RGB;
    bi->biSizeImage = (DWORD)(stride * h);
    BYTE* px = reinterpret_cast<BYTE*>(&out[sizeof(BITMAPINFOHEADER)]);
    if (FAILED(conv->CopyPixels(nullptr, (UINT)stride, (UINT)(stride * h), px))) { out.clear(); return false; }
    std::vector<BYTE> row(stride);
    for (UINT y = 0; y < h / 2; y++) {  // Top-down from WIC, bottom-up for CF_DIB
        BYTE* a = px + y * stride;
        BYTE* b = px + (h - 1 - y) * stride;
        memcpy(row.data(), a, stride);
        memcpy(a, b, stride);
        memcpy(b, row.data(), stride);
    }
    return true;
}

// ---- The page ----
class ShelfPage : public PageBase {
public:
    const wchar_t* Id() const override { return L"page-shelf"; }
    int DashboardOrder() const override { return 60; }
    int Count() { EnsureLoaded(); return (int)items_.size(); }

    // A thumbnail arrived from the worker. The row may be gone by now; it is found by id.
    void SetThumb(unsigned id, std::shared_ptr<Pixmap> pm) {
        ShelfItem* it = Find(id);
        if (!it || !pm) return;
        it->thumb = std::move(pm);
        if (sv_) sv_->Invalidate();
    }

    // Adds what a data object offers. Reads finish before this returns; items are built in locals and committed at the end.
    int Ingest(IDataObject* d) {
        EnsureLoaded();
        InitFormats();
        std::vector<ShelfItem> fresh;
        std::wstring refuse;
        if (HasFormat(d, CF_HDROP, TYMED_HGLOBAL)) {
            FORMATETC f{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM m{};
            if (SUCCEEDED(d->GetData(&f, &m))) {
                HDROP drop = (HDROP)m.hGlobal;
                const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                wchar_t tmp[MAX_PATH + 1] = {};
                GetTempPathW(MAX_PATH, tmp);
                const std::wstring tempDir = Lower(tmp);
                for (UINT i = 0; i < n && i < 64; i++) {
                    wchar_t path[MAX_PATH * 2] = {};
                    if (!DragQueryFileW(drop, i, path, MAX_PATH * 2)) continue;
                    ShelfItem it;
                    it.kind = SK::File;
                    it.path = path;
                    it.title = FileNameOf(it.path);
                    const DWORD a = GetFileAttributesW(path);
                    it.isDir = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
                    WIN32_FILE_ATTRIBUTE_DATA info{};
                    if (!it.isDir && GetFileAttributesExW(path, GetFileExInfoStandard, &info)) it.size = ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
                    if (!it.isDir && Lower(it.path).rfind(tempDir, 0) == 0) {  // Apps delete temp files soon: keep a copy.
                        if (it.size > kShelfItemCap) { refuse = L"Too large to keep"; continue; }
                        it.kind = IsImageExt(ExtOf(it.title)) ? SK::Image : SK::Stored;
                        std::wstring blob = NewBlobName(ExtOf(it.title));
                        if (CopyFileW(path, (BlobDir() + L"\\" + blob).c_str(), FALSE)) { it.path = blob; } else { continue; }
                    }
                    fresh.push_back(std::move(it));
                }
                ReleaseStgMedium(&m);
            }
        } else if (HasFormat(d, g_cfDesc, TYMED_HGLOBAL) && HasFormat(d, g_cfContents, kTymeds, 0)) {
            std::string desc;
            if (ReadFormat(d, g_cfDesc, -1, &desc, nullptr, 1 << 20) && desc.size() >= sizeof(UINT)) {
                const auto* g = reinterpret_cast<const FILEGROUPDESCRIPTORW*>(desc.data());
                const UINT n = std::min<UINT>(g->cItems, (UINT)((desc.size() - sizeof(UINT)) / sizeof(FILEDESCRIPTORW)));
                const bool isUrl = n >= 1 && ExtOf(g->fgd[0].cFileName) == L".url";
                for (UINT i = 0; !isUrl && i < n && i < 32; i++) {
                    ShelfItem it;
                    it.title = g->fgd[i].cFileName;
                    it.kind = IsImageExt(ExtOf(it.title)) ? SK::Image : SK::Stored;
                    const std::wstring blob = NewBlobName(ExtOf(it.title));
                    const std::wstring full = BlobDir() + L"\\" + blob;
                    unsigned long long sz = 0;
                    if (ReadFormat(d, g_cfContents, (LONG)i, nullptr, &full, kShelfItemCap, &sz)) {
                        it.path = blob;
                        it.size = sz;
                        fresh.push_back(std::move(it));
                    }
                }
            }
        }
        if (fresh.empty() && refuse.empty()) {
            std::wstring url;
            std::string raw;
            if (ReadFormat(d, g_cfUrlW, -1, &raw, nullptr, 65536)) {
                url.assign(reinterpret_cast<const wchar_t*>(raw.data()), raw.size() / 2);
                url = url.c_str();
            } else if (raw.clear(), ReadFormat(d, g_cfUrlA, -1, &raw, nullptr, 65536)) {
                url = Wide(std::string(raw.c_str()));
            }
            if (!url.empty()) {
                ShelfItem it;
                it.kind = SK::Link;
                it.text = url;
                it.title = OneLine(url, 60);
                fresh.push_back(std::move(it));
            }
        }
        if (fresh.empty() && refuse.empty()) {
            std::wstring text;
            std::string raw;
            if (ReadFormat(d, CF_UNICODETEXT, -1, &raw, nullptr, 4u << 20)) {
                text.assign(reinterpret_cast<const wchar_t*>(raw.data()), raw.size() / 2);
                text = text.c_str();
            } else if (raw.clear(), ReadFormat(d, CF_TEXT, -1, &raw, nullptr, 4u << 20)) {
                text = Wide(std::string(raw.c_str()));
            }
            while (!text.empty() && iswspace(text.back())) text.pop_back();
            if (!text.empty()) {
                ShelfItem it;
                it.kind = LooksLikeLink(text) ? SK::Link : SK::Text;
                it.title = OneLine(text, 60);
                it.size = text.size();
                if (it.kind == SK::Text && Utf8(text).size() > 4096) {  // Long text lives in a blob, so the index stays small.
                    const std::wstring blob = NewBlobName(L".txt");
                    if (WriteFileAtomic(BlobDir() + L"\\" + blob, Utf8(text))) it.path = blob;
                    else { it.text = text.substr(0, 4000); }
                } else {
                    it.text = text;
                }
                fresh.push_back(std::move(it));
            }
        }
        if (fresh.empty() && refuse.empty()) {
            const std::wstring blob = NewBlobName(L".png");
            const std::wstring full = BlobDir() + L"\\" + blob;
            std::string raw;
            bool ok = false;
            if (HasFormat(d, g_cfPng) && ReadFormat(d, g_cfPng, -1, nullptr, &full, kShelfItemCap)) ok = true;
            else if ((HasFormat(d, CF_DIBV5) && ReadFormat(d, CF_DIBV5, -1, &raw, nullptr, 256u << 20) && DibToPng(raw, full)) ||
                     (raw.clear(), HasFormat(d, CF_DIB) && ReadFormat(d, CF_DIB, -1, &raw, nullptr, 256u << 20) && DibToPng(raw, full))) ok = true;
            if (ok) {
                ShelfItem it;
                it.kind = SK::Image;
                it.path = blob;
                it.title = L"Image";
                WIN32_FILE_ATTRIBUTE_DATA info{};
                if (GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &info)) it.size = ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
                fresh.push_back(std::move(it));
            }
        }
        int added = 0;
        lastIngest_.clear();
        for (ShelfItem& it : fresh) {
            bool isNew = false;
            if (Commit(std::move(it), &isNew)) { added++; lastIngest_.push_back({items_.back().id, isNew}); }
            else refuse = L"Shelf is full";
        }
        if (added) { Save(); scroll_ = 0; }
        if (!added) Flash(refuse.empty() ? L"Nothing to keep" : refuse);
        if (sv_) sv_->Invalidate();
        return added;
    }

    void Draw(const DrawCtx& c) override {
        EnsureLoaded();
        Pg p(c);
        hits_.clear();
        p.Text(L"Shelf", 26, 14, 120, 34, 12, kFg, 0, 700);
        const int n = (int)items_.size();
        wchar_t cnt[32];
        swprintf_s(cnt, n == 1 ? L"%d item" : L"%d items", n);
        p.Text(status_.empty() ? std::wstring(cnt) : status_, 110, 14, 254, 34, 11, status_.empty() ? kMuted : kAccent, TF_Right | TF_Ellipsis);
        auto chip = [&](int id, float l, const wchar_t* text, D2D1_COLOR_F tint) {
            p.Fill(l, 14, l + 44, 34, 7, hover_ == id ? kCardHover : kCard);
            p.Text(text, l, 14, l + 44, 34, 11, tint, TF_Center, 600);
            Hit(l, 14, l + 44, 34, id);
        };
        chip(1, 262, L"Paste", kFg);
        chip(2, 310, confirmUntil_ > GetTickCount64() ? L"Sure?" : L"Clear", confirmUntil_ > GetTickCount64() ? kRed : kFg);

        if (n == 0) {
            static Com<ID2D1StrokeStyle> dash;
            if (!dash) {
                D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10.0f, D2D1_DASH_STYLE_DASH, 0.0f);
                g_factory->CreateStrokeStyle(sp, nullptr, 0, dash.Put());
            }
            p.Outline(26, 46, 354, 150, 10, 1, kFg, 0.3f, dash.Get());
            p.Text(L"Drop anything here", 26, 46, 354, 150, 12, kMuted, TF_Center);
        }
        const int cap = kRows;
        scroll_ = std::max(0, std::min(scroll_, n - cap));
        for (int r = 0; r < cap; r++) {
            const int d = scroll_ + r;
            if (d >= n) break;
            ShelfItem& it = items_[n - 1 - d];
            const float top = 46 + r * (kRowH + kGap), bot = top + kRowH;
            const int rowId = 1000 + (int)it.id, rmId = 100000 + (int)it.id;
            const bool hot = hover_ == rowId || hover_ == rmId || hover_ == kPhoneIdBase + (int)it.id;
            if (hot) p.Fill(26, top, 354, bot, 9, kCard);
            Hit(26, top, 354, bot, rowId);
            const float a = it.missing ? 0.45f : 1.0f;
            DrawKindIcon(p, it, 34, top + 6, a);
            p.Col(kFg, a);
            const bool phone = hot && CanPhone(it);
            p.Text(it.title, 58, top, 258, bot, 11, it.missing ? kMuted : kFg, TF_Ellipsis);
            p.Text(SizeLabel(it), 258, top, phone ? 302 : 322, bot, 11, kMuted, TF_Right | TF_Ellipsis);
            if (hot) {
                const float cx = 337, cy = top + kRowH / 2;
                p.Dot(cx, cy, 7, hover_ == rmId ? kCardHover : kCard);
                p.Line(cx - 2.5f, cy - 2.5f, cx + 2.5f, cy + 2.5f, 1.3f, kFg);
                p.Line(cx - 2.5f, cy + 2.5f, cx + 2.5f, cy - 2.5f, 1.3f, kFg);
                Hit(cx - 10, cy - 10, cx + 10, cy + 10, rmId);
            }
            if (phone) {  // Send to phone; the row stays.
                const int phId = kPhoneIdBase + (int)it.id;
                const float cx = 315, cy = top + kRowH / 2;
                p.Dot(cx, cy, 7, hover_ == phId ? kCardHover : kCard);
                PhoneGlyph(p, cx, cy, kFg);
                Hit(cx - 9, cy - 10, cx + 9, cy + 10, phId);
            }
        }
        if (n > cap) {  // A thin scroll mark
            const float trackT = 46, trackB = 46 + cap * (kRowH + kGap) - kGap;
            const float th = (trackB - trackT) * cap / n, ty = trackT + (trackB - trackT - th) * scroll_ / std::max(1, n - cap);
            p.Fill(358, ty, 360, ty + th, 1, kFg, 0.35f);
        }
        DrawChoice(c);
        if (g_dropActive) p.Outline(3, 3, 377, 181, 14, 1.5f, kAccent, 0.8f);
    }

    bool Animating(View v) const override { return v == View::Dashboard && !choice_.Settled(); }

    // After a drop: the card asks keep or send, for the dropped rows the phone can take. Keep is the default.
    void OfferPhone() {
        offer_.clear();
        for (const Offer& o : lastIngest_) {
            if (ShelfItem* it = Find(o.id)) if (CanPhone(*it)) offer_.push_back(o);
        }
        lastIngest_.clear();
        if (offer_.empty()) return;
        g_shelfChoiceOpen = true;
        choiceUntil_ = GetTickCount64() + kChoiceMs;
        ArmTimer();
        ShowChoice(true);
    }

    bool OnWheel(int steps) override {
        const int n = Count();
        if (n <= kRows) return false;
        const int next = std::max(0, std::min(scroll_ - steps, n - kRows));
        if (next == scroll_) return false;  // At an end: the next notch changes the page.
        scroll_ = next;
        sv_->Invalidate();
        return true;
    }

    bool OnPointer(const PointerEvent& e, View v) override {
        const float x = e.pt.x / Scale(), y = e.pt.y / Scale();
        if (e.kind == PointerEvent::Down) {
            press_ = 0;
            for (const HitRect& h : hits_) if (x >= h.l && x < h.r && y >= h.t && y < h.b && h.id >= 1000 && h.id < 100000) press_ = h.id - 1000;
            for (const HitRect& h : hits_) if (x >= h.l - 3 && x < h.r + 3 && y >= h.t - 3 && y < h.b + 3 && h.id >= 100000) press_ = 0;  // The row buttons never start a drag
            for (const HitRect& h : hits_) if (x >= h.l && x < h.r && y >= h.t && y < h.b && h.id == kChoiceCard) press_ = 0;  // Nor does the card over the rows
            pressPt_ = e.pt;
        } else if (e.kind == PointerEvent::Move && press_ && (GetKeyState(VK_LBUTTON) & 0x8000)) {
            if (fabsf(e.pt.x - pressPt_.x) > GetSystemMetrics(SM_CXDRAG) || fabsf(e.pt.y - pressPt_.y) > GetSystemMetrics(SM_CYDRAG)) {
                const unsigned id = (unsigned)press_;
                press_ = 0;
                down_ = 0;
                DragOut(id);
                return true;
            }
        } else if (e.kind == PointerEvent::Up || e.kind == PointerEvent::Leave) {
            press_ = 0;
        }
        return PageBase::OnPointer(e, v);
    }

protected:
    void Shown() override { EnsureLoaded(); }
    void Hidden() override {
        scroll_ = 0;
        offer_.clear();  // Closing the island over the card keeps what was dropped.
        g_shelfChoiceOpen = false;
        choiceUntil_ = 0;
        choice_.Snap(0);
        ArmTimer();
        for (ShelfItem& it : items_) {  // Keep the small CPU pixels as the cache; drop the GPU copies.
            if (it.icon) it.icon->bmp.Reset();
            if (it.thumb) it.thumb->bmp.Reset();
        }
    }
    void Click(int id) override {
        if (id == 1) {
            Com<IDataObject> obj;
            if (SUCCEEDED(OleGetClipboard(obj.Put())) && obj) Ingest(obj.Get()); else Flash(L"Clipboard is empty");
        } else if (id == 2) {
            if (items_.empty()) return;
            bool destroys = false;
            for (const ShelfItem& it : items_) if (it.kind != SK::File && it.kind != SK::Link) destroys = true;
            const ULONGLONG now = GetTickCount64();
            if (destroys && confirmUntil_ <= now) {  // This destroys stored content: ask once.
                confirmUntil_ = now + 3000;
                ArmStatusTimer(3000);
            } else {
                for (ShelfItem& it : items_) Free(it);
                items_.clear();
                confirmUntil_ = 0;
                Save();
            }
        } else if (id == kChoiceKeep) {
            ResolveChoice(false);
        } else if (id == kChoiceSend) {
            ResolveChoice(true);
        } else if (id == kChoiceCard) {
            return;
        } else if (id >= kPhoneIdBase) {
            bool removed = false;
            Flash(SendItem((unsigned)(id - kPhoneIdBase), false, &removed) ? L"Sent to phone" : L"Could not send to phone");
        } else if (id >= 100000) {
            Remove((unsigned)(id - 100000));
        } else if (id >= 1000) {
            OpenItem((unsigned)(id - 1000));
        }
        sv_->Invalidate();
    }

private:
    static constexpr int kRows = 3;
    static constexpr float kRowH = 30, kGap = 6;
    static constexpr int kChoiceKeep = 3, kChoiceSend = 4, kChoiceCard = 5;  // Hit ids of the drop card
    static constexpr int kPhoneIdBase = 200000;                              // + item id: a row's phone button
    static constexpr ULONGLONG kChoiceMs = 4000;                            // Then it stays in the bar.

    struct Offer {
        unsigned id;
        bool fresh;  // Added by this drop, not an existing row that the drop promoted.
    };

    // Whether a row can go to the phone: a file or folder on disk, or the file the shelf stored.
    bool CanPhone(const ShelfItem& it) {
        if (it.missing) return false;
        if (it.kind == SK::File) return !PhoneCommand(it.isDir).empty();
        if (it.kind == SK::Link || it.path.empty() || !SafeBlobName(it.path)) return false;  // Links and short text are not files.
        return !PhoneCommand(false).empty();
    }

    static void PhoneGlyph(const Pg& p, float cx, float cy, D2D1_COLOR_F k) {
        p.Outline(cx - 3.2f, cy - 5, cx + 3.2f, cy + 5, 1.6f, 1.1f, k);
        p.Line(cx - 1, cy + 3, cx + 1, cy + 3, 1.1f, k);
    }

    std::wstring SentDir() { return ShelfDir() + L"\\sent"; }

    // Stored files go to the phone from their own folder under sent\, named after the row: the agent may send
    // after the row is gone. A hard link costs no space; sent\ is swept after a week.
    std::wstring SentLink(const ShelfItem& it, bool move) {
        const std::wstring blob = BlobPath(it);
        std::wstring name = SafeFileName(it.title, it.kind == SK::Text ? L"Text" : L"File", L"");
        const std::wstring ext = ExtOf(it.path);
        if (!ext.empty() && ExtOf(name) != ext) name += ext;
        const std::wstring dir = SentDir() + L"\\" + std::to_wstring(WallSeconds()) + L"_" + std::to_wstring(it.id);
        if (SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr) != ERROR_SUCCESS && GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) return L"";
        const std::wstring target = dir + L"\\" + name;
        if (CreateHardLinkW(target.c_str(), blob.c_str(), nullptr)) return target;
        if (move && MoveFileExW(blob.c_str(), target.c_str(), 0)) return target;
        RemoveDirectoryW(dir.c_str());
        return L"";
    }

    void SweepSent() {
        const ULONGLONG now = WallSeconds();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((SentDir() + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            const ULONGLONG at = wcstoull(fd.cFileName, nullptr, 10);
            if (!at || now < at || now - at < 7 * 86400) continue;
            const std::wstring dir = SentDir() + L"\\" + fd.cFileName;
            WIN32_FIND_DATAW f2;
            HANDLE h2 = FindFirstFileW((dir + L"\\*").c_str(), &f2);
            if (h2 != INVALID_HANDLE_VALUE) {
                do {
                    if (!(f2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + f2.cFileName).c_str());
                } while (FindNextFileW(h2, &f2));
                FindClose(h2);
            }
            RemoveDirectoryW(dir.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // Hands one row to the phone. remove: drop the row once handed over (only when the file survives that).
    bool SendItem(unsigned id, bool remove, bool* removed) {
        *removed = false;
        ShelfItem* it = Find(id);
        if (!it || !CanPhone(*it)) return false;
        bool ok = false, canRemove = true;
        if (it->kind == SK::File) {
            if (GetFileAttributesW(it->path.c_str()) == INVALID_FILE_ATTRIBUTES) { it->missing = true; return false; }
            ok = LaunchPhoneSend(it->path, it->isDir);
        } else {
            std::wstring path = SentLink(*it, remove);
            if (path.empty()) { path = BlobPath(*it); canRemove = false; }  // Only the stored copy: keep the row.
            ok = LaunchPhoneSend(path, false);
        }
        if (ok && remove && canRemove) { Remove(id); *removed = true; }
        return ok;
    }

    void ShowChoice(bool on) {
        choice_.Aim(on ? 1.0f : 0.0f, on ? kSpringExpandK : kSpringCollapseK, on ? kSpringExpandC : kSpringCollapseC);
        choiceT_ = QpcSeconds();
        if (sv_) { sv_->WakeAnimation(); sv_->Invalidate(); }
    }

    void ResolveChoice(bool send) {
        if (offer_.empty()) return;
        const std::vector<Offer> o = std::move(offer_);
        offer_.clear();
        g_shelfChoiceOpen = false;
        choiceUntil_ = 0;
        ArmTimer();
        ShowChoice(false);
        if (!send) return;
        int sent = 0, failed = 0;
        for (const Offer& f : o) {
            bool removed = false;  // Sent from the drop: not kept, unless the drop only promoted a row already there.
            if (SendItem(f.id, f.fresh, &removed)) sent++; else failed++;
        }
        Flash(sent ? L"Sent to phone" : L"Could not send to phone");
        AlertsPhoneSent(sent, failed);
        // The confirmation is an alert, and alerts wait while the island is open: let it close.
        if (sv_) sv_->Post([] { if (!g_pinned) SetExpanded(false); });
    }

    // The drop card: rises over the bottom rows, dark and flat like the island.
    void DrawChoice(const DrawCtx& c) {
        if (!choice_.Settled()) {
            double dt = c.now - choiceT_;
            if (dt > 0.05) dt = 0.05;
            if (dt > 0) choice_.Step(dt);
        }
        choiceT_ = c.now;
        const float a = std::clamp(choice_.x, 0.0f, 1.0f);
        if (a <= 0.01f) return;
        DrawCtx cc = c;
        cc.alpha *= a;
        Pg q(cc);
        const float dy = (1.0f - choice_.x) * 22.0f;  // Overshoot shows as a small lift
        const float t = 116 + dy, b = 174 + dy;
        q.Fill(22, t, 358, b, 13, kCentreBg);
        q.Fill(22, t, 358, b, 13, kCard);
        q.Outline(22.5f, t + 0.5f, 357.5f, b - 0.5f, 13, 1, kHairline);
        const bool open = !offer_.empty();
        if (open) Hit(22, t, 358, b, kChoiceCard);
        std::wstring what;
        if (offer_.size() == 1) { if (ShelfItem* it = Find(offer_[0].id)) what = it->title; }
        else if (offer_.size() > 1) what = std::to_wstring(offer_.size()) + L" items";
        q.Text(L"Dropped", 36, t + 9, 160, t + 27, 10.5f, kMuted, TF_Ellipsis);
        q.Text(what, 36, t + 27, 160, t + 47, 11.5f, kFg, TF_Ellipsis, 600);
        auto button = [&](int id, float l, float r, const wchar_t* text, bool primary) {
            const float bt = t + 14, bb = b - 14;
            const bool hot = open && hover_ == id;
            if (primary) q.Fill(l, bt, r, bb, 9, kAccent, hot ? 1.0f : 0.9f);
            else q.Fill(l, bt, r, bb, 9, hot ? kCardHover : kCard);
            if (id == kChoiceSend) {
                PhoneGlyph(q, l + 14, (bt + bb) / 2, primary ? kInk : kFg);
                q.Text(text, l + 22, bt, r - 6, bb, 11, primary ? kInk : kFg, TF_Center | TF_Ellipsis, 600);
            } else {
                q.Text(text, l + 4, bt, r - 4, bb, 11, primary ? kInk : kFg, TF_Center | TF_Ellipsis, 600);
            }
            if (open) Hit(l, bt, r, bb, id);
        };
        button(kChoiceKeep, 166, 250, L"Keep in bar", false);
        button(kChoiceSend, 256, 346, L"Send to phone", true);
    }

    ShelfItem* Find(unsigned id) {
        for (ShelfItem& it : items_) if (it.id == id) return &it;
        return nullptr;
    }
    std::wstring BlobPath(const ShelfItem& it) { return it.kind == SK::File ? it.path : BlobDir() + L"\\" + it.path; }
    std::wstring NewBlobName(const std::wstring& ext) {
        EnsureLoaded();
        return std::to_wstring(nextId_++) + L"b" + (ext.empty() ? L".bin" : ext);
    }
    void Free(ShelfItem& it) {
        if (it.kind != SK::File && !it.path.empty() && SafeBlobName(it.path)) DeleteFileW((BlobDir() + L"\\" + it.path).c_str());
        delete it.icon;
        it.icon = nullptr;
        it.thumb.reset();
    }
    void Remove(unsigned id) {
        for (size_t i = 0; i < items_.size(); i++) {
            if (items_[i].id == id) { Free(items_[i]); items_.erase(items_.begin() + i); break; }
        }
        Save();
    }
    // Dedup promotes the existing row. Text and links compare exactly, paths without case.
    bool Commit(ShelfItem it, bool* isNew) {
        *isNew = false;
        for (size_t i = 0; i < items_.size(); i++) {
            ShelfItem& o = items_[i];
            const bool same = o.kind == it.kind && ((it.kind == SK::File && Lower(o.path) == Lower(it.path)) ||
                                                     (it.kind == SK::Link && o.text == it.text) ||
                                                     (it.kind == SK::Text && o.title == it.title && o.size == it.size && o.text == it.text && o.path.empty() && it.path.empty()));
            if (same) {
                ShelfItem keep = std::move(o);
                items_.erase(items_.begin() + i);
                keep.addedAt = WallSeconds();
                items_.push_back(std::move(keep));
                if (it.kind != SK::File && !it.path.empty()) DeleteFileW((BlobDir() + L"\\" + it.path).c_str());
                return true;
            }
        }
        unsigned long long blobs = 0;
        for (const ShelfItem& o : items_) if (o.kind == SK::Stored || o.kind == SK::Image) blobs += o.size;
        if (items_.size() >= kShelfMaxItems || blobs + it.size > kShelfBlobBudget) {
            if (it.kind != SK::File && !it.path.empty()) DeleteFileW((BlobDir() + L"\\" + it.path).c_str());
            return false;
        }
        it.id = nextId_++;
        it.addedAt = WallSeconds();
        items_.push_back(std::move(it));
        *isNew = true;
        return true;
    }

    void Flash(const std::wstring& text) {
        status_ = text;
        ArmStatusTimer(1800);
    }
    void ArmStatusTimer(UINT ms) {
        statusUntil_ = GetTickCount64() + ms;
        ArmTimer();
    }
    // The module has one timer: it serves the status line and the drop card, whichever is due first.
    void ArmTimer() {
        if (!sv_) return;
        const ULONGLONG now = GetTickCount64();
        ULONGLONG next = statusUntil_;
        if (choiceUntil_ && (!next || choiceUntil_ < next)) next = choiceUntil_;
        if (!next) { sv_->KillTimer(this); return; }
        sv_->SetTimer(this, (UINT)std::max<ULONGLONG>(next > now ? next - now : 0, 10), [this] { OnTimer(); });
    }
    void OnTimer() {
        const ULONGLONG now = GetTickCount64();
        if (statusUntil_ && statusUntil_ <= now) {
            statusUntil_ = 0;
            status_.clear();
            confirmUntil_ = 0;
            sv_->Invalidate();
        }
        if (choiceUntil_ && choiceUntil_ <= now) ResolveChoice(false);  // Nothing chosen: it stays in the bar.
        ArmTimer();
    }

    std::wstring SizeLabel(const ShelfItem& it) {
        switch (it.kind) {
            case SK::File: return it.isDir ? L"—" : FormatSize(it.size);
            case SK::Text: { wchar_t b[32]; swprintf_s(b, L"%llu ch", it.size); return b; }
            case SK::Link: {
                const size_t s = it.text.find(L"://");
                if (s == std::wstring::npos) return it.text.substr(it.text.find(L':') + 1, 20);
                const size_t e = it.text.find_first_of(L"/?#", s + 3);
                return it.text.substr(s + 3, e == std::wstring::npos ? std::wstring::npos : e - s - 3);
            }
            default: return FormatSize(it.size);
        }
    }

    void DrawKindIcon(const Pg& p, ShelfItem& it, float x, float y, float alpha) {
        const bool chip = it.kind == SK::Text || it.kind == SK::Link;
        if (chip) {
            p.Fill(x, y, x + 18, y + 18, 5, kCardHover, alpha);
            p.Text(it.kind == SK::Text ? L"T" : L"↗", x, y, x + 18, y + 18, 11, kFg, TF_Center, 700);
            return;
        }
        if (!it.iconTried) {
            it.iconTried = true;
            SHFILEINFOW sfi{};
            const std::wstring path = it.kind == SK::File ? it.path : it.title;
            DWORD flags = SHGFI_ICON | SHGFI_SMALLICON;
            if (it.missing || it.kind != SK::File || it.path.rfind(L"\\\\", 0) == 0) flags |= SHGFI_USEFILEATTRIBUTES;
            if (SHGetFileInfoW(path.c_str(), it.isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), flags) && sfi.hIcon) {
                it.icon = PixmapFromIcon(sfi.hIcon);
                DestroyIcon(sfi.hIcon);
            }
        }
        // Thumbnails for files, stored files and images; asked once, for rows that are drawn.
        if (!it.thumbAsked && !it.missing && !it.isDir && (it.kind == SK::File || SafeBlobName(it.path))) {
            it.thumbAsked = true;
            const int px = std::max(16, std::min(64, (int)ceilf(18 * p.s)));
            ThumbRequest(ThumbJob{it.id, BlobPath(it), it.kind == SK::Image || IsImageExt(ExtOf(it.title)), px});
        }
        if (it.thumb && it.thumb->w > 0 && it.thumb->h > 0) {
            Pixmap& px = *it.thumb;
            if (ID2D1Bitmap* b = PixmapBitmap(p, px)) {
                const float f = 18.0f / std::max(px.w, px.h), w = px.w * f, h = px.h * f;  // Fit, keep aspect
                const float l = x + (18 - w) / 2, t = y + (18 - h) / 2;
                p.c.rt->DrawBitmap(b, p.R(l, t, l + w, t + h), p.c.alpha * alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                return;
            }
        }
        if (it.icon) {
            if (ID2D1Bitmap* b = PixmapBitmap(p, *it.icon)) p.c.rt->DrawBitmap(b, p.R(x, y, x + 18, y + 18), p.c.alpha * alpha);
        } else {
            p.Fill(x + 3, y + 1, x + 15, y + 17, 2, kMuted, alpha);
        }
    }

    // ---- Open ----
    void OpenItem(unsigned id) {
        ShelfItem* it = Find(id);
        if (!it) return;
        if (it->kind == SK::Text) {
            std::wstring body = it->text;
            if (!it->path.empty()) {
                std::string raw;
                if (SafeBlobName(it->path) && ReadWholeFile(BlobPath(*it), raw, 8u << 20)) body = Wide(raw);
            }
            Flash(CopyTextToClipboard(body) ? L"Copied" : L"Copy failed");
            return;
        }
        std::wstring target;
        if (it->kind == SK::Link) {
            if (!LooksLikeLink(it->text)) { Flash(CopyTextToClipboard(it->text) ? L"Copied" : L"Copy failed"); return; }  // Never launch arbitrary strings.
            target = it->text;
        } else {
            if (it->kind != SK::File && !SafeBlobName(it->path)) return;
            target = BlobPath(*it);
            if (it->kind == SK::File && GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) { it->missing = true; return; }
        }
        AllowSetForegroundWindow(ASFW_ANY);
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.fMask = SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = L"open";
        sei.lpFile = target.c_str();
        sei.nShow = SW_SHOWNORMAL;
        ShellExecuteExW(&sei);
    }

    // ---- Drag out ----
    class Source : public IDropSource {
    public:
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override {
            if (r == IID_IUnknown || r == IID_IDropSource) { *o = this; return S_OK; }
            *o = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
        ULONG STDMETHODCALLTYPE Release() override { return 1; }
        HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL esc, DWORD keys) override {
            if (esc) return DRAGDROP_S_CANCEL;
            return (keys & MK_LBUTTON) ? S_OK : DRAGDROP_S_DROP;
        }
        HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
    };

    // An empty shell container that accepts SetData of any format (measured; see the spec).
    static bool EmptyDataObject(Com<IDataObject>& obj) {
        return SUCCEEDED(SHCreateDataObject(nullptr, 0, nullptr, nullptr, IID_IDataObject, reinterpret_cast<void**>(obj.Put()))) && obj;
    }

    // Files: the shell's own data object (CF_HDROP, the ID list, names), else CF_HDROP by hand.
    static bool FileDataObject(const std::wstring& path, Com<IDataObject>& obj) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) {
            if (PIDLIST_ABSOLUTE parent = ILClone(pidl)) {
                ILRemoveLastID(parent);
                PCUITEMID_CHILD child = ILFindLastID(pidl);
                if (FAILED(SHCreateDataObject(parent, 1, &child, nullptr, IID_IDataObject, reinterpret_cast<void**>(obj.Put())))) obj.Reset();
                ILFree(parent);
            }
            ILFree(pidl);
        }
        if (obj) return true;
        return EmptyDataObject(obj) && SetHDrop(obj.Get(), path);
    }

    void DragOut(unsigned id) {
        ShelfItem* it = Find(id);
        if (!it) return;
        const ShelfItem copy = *it;  // The list can change while the drag blocks.
        InitFormats();
        Com<IDataObject> obj;
        if (copy.kind == SK::File || copy.kind == SK::Stored || copy.kind == SK::Image) {
            if (copy.kind != SK::File && !SafeBlobName(copy.path)) return;
            const std::wstring path = BlobPath(copy);
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                if (ShelfItem* now = Find(copy.id)) now->missing = true;
                Flash(L"File not found");
                sv_->Invalidate();
                return;
            }
            if (!FileDataObject(path, obj)) return;
            if (copy.kind == SK::Image || (copy.kind == SK::File && IsImageExt(ExtOf(copy.title)))) {
                // The picture itself too, for apps that take pasted images rather than files.
                std::string png, dib;
                bool set = true;
                if (ExtOf(path) == L".png" && ReadWholeFile(path, png, 32u << 20)) set = SetHGlobal(obj.Get(), g_cfPng, png.data(), png.size());
                png = std::string();
                if (set && ImageDib(path, dib)) set = SetHGlobal(obj.Get(), CF_DIB, dib.data(), dib.size());
                if (!set) {  // The shell object refused extra formats: a plain container with CF_HDROP.
                    Com<IDataObject> plain;
                    if (EmptyDataObject(plain) && SetHDrop(plain.Get(), path)) {
                        obj = plain;
                        if (!dib.empty() || ImageDib(path, dib)) SetHGlobal(obj.Get(), CF_DIB, dib.data(), dib.size());
                    }
                }
            }
        } else {
            if (!EmptyDataObject(obj)) return;
            std::wstring body = copy.text;
            if (copy.kind == SK::Text && !copy.path.empty() && SafeBlobName(copy.path)) {
                std::string raw;
                if (ReadWholeFile(BlobPath(copy), raw, 8u << 20)) body = Wide(raw);
            }
            SetText(obj.Get(), body);
            // Never CF_HDROP for text rows: editors would open a file instead of inserting text.
            if (copy.kind == SK::Link) {
                SetHGlobal(obj.Get(), g_cfUrlW, copy.text.c_str(), (copy.text.size() + 1) * sizeof(wchar_t));
                const std::string a = Utf8(copy.text);
                SetHGlobal(obj.Get(), g_cfUrlA, a.c_str(), a.size() + 1);
                if (LooksLikeLink(copy.text)) SetVirtualFile(obj.Get(), SafeFileName(copy.title, L"Link", L".url"), "[InternetShortcut]\r\nURL=" + a + "\r\n");
            } else {
                SetVirtualFile(obj.Get(), SafeFileName(copy.title, L"Text", L".txt"), "\xEF\xBB\xBF" + Utf8(body));
            }
        }
        if (!obj) return;
        static const CLIPFORMAT cfPreferred = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
        const DWORD copyEffect = DROPEFFECT_COPY;
        SetHGlobal(obj.Get(), cfPreferred, &copyEffect, sizeof(copyEffect));
        static Source source;
        DWORD effect = 0;
        g_shelfInternalDrag = true;
        using ShDo = HRESULT(WINAPI*)(HWND, IDataObject*, IDropSource*, DWORD, DWORD*);
        static ShDo shDo = reinterpret_cast<ShDo>(GetProcAddress(GetModuleHandleW(L"shell32.dll"), "SHDoDragDrop"));
        if (shDo) shDo(g_isHwnd, obj.Get(), &source, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
        else DoDragDrop(obj.Get(), &source, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
        g_shelfInternalDrag = false;
        g_dropLeft = GetTickCount64();
        hover_ = down_ = 0;  // No WM_MOUSELEAVE arrives after a drag.
        if (copy.kind == SK::File && GetFileAttributesW(copy.path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (ShelfItem* now = Find(copy.id)) now->missing = true;  // Addressed by id, never by path.
        }
        sv_->Invalidate();
    }

    // ---- Persistence ----
    void EnsureLoaded() {
        if (loaded_) return;
        loaded_ = true;
        SHCreateDirectoryExW(nullptr, BlobDir().c_str(), nullptr);
        SweepSent();
        std::string raw;
        const std::wstring index = ShelfDir() + L"\\index.txt";
        if (!ReadWholeFile(index, raw)) {
            if (GetFileAttributesW(index.c_str()) != INVALID_FILE_ATTRIBUTES) MoveFileExW(index.c_str(), (index + L".bad").c_str(), MOVEFILE_REPLACE_EXISTING);
            return;
        }
        const std::wstring text = Wide(raw);
        size_t pos = 0;
        bool header = false, bad = false;
        std::vector<ShelfItem> loaded;
        while (pos < text.size()) {
            size_t e = text.find(L'\n', pos);
            if (e == std::wstring::npos) e = text.size();
            std::wstring line = text.substr(pos, e - pos);
            pos = e + 1;
            while (!line.empty() && line.back() == L'\r') line.pop_back();
            if (line.empty()) continue;
            if (!header) {
                if (line != L"TBSHELF 2") { bad = true; break; }
                header = true;
                continue;
            }
            std::vector<std::wstring> f;
            size_t s = 0;
            for (;;) {
                const size_t t = line.find(L'\t', s);
                f.push_back(UnescapeField(line.substr(s, t == std::wstring::npos ? std::wstring::npos : t - s)));
                if (t == std::wstring::npos) break;
                s = t + 1;
            }
            if (f.size() < 4) continue;  // Unknown or short lines are skipped
            ShelfItem it;
            it.id = (unsigned)wcstoul(f[1].c_str(), nullptr, 10);
            it.addedAt = wcstoull(f[2].c_str(), nullptr, 10);
            const wchar_t k = f[0].empty() ? L'?' : f[0][0];
            if (k == L'F') { it.kind = SK::File; it.path = f[3]; it.title = FileNameOf(it.path); }
            else if (k == L'T' && f.size() >= 6) { it.kind = SK::Text; it.title = f[3]; it.path = f[4]; it.text = f[5]; if (!it.path.empty() && !SafeBlobName(it.path)) continue; it.size = it.text.size(); }
            else if (k == L'U') { it.kind = SK::Link; it.title = f[3]; it.text = f.size() > 4 ? f[4] : L""; }
            else if ((k == L'I' || k == L'S') && f.size() >= 5) { it.kind = k == L'I' ? SK::Image : SK::Stored; it.title = f[3]; it.path = f[4]; if (!SafeBlobName(it.path)) continue; }
            else continue;
            if (!it.id) continue;
            // Missing entries stay, shown dim. Network paths are not touched here: they can stall for seconds.
            WIN32_FILE_ATTRIBUTE_DATA info{};
            if (it.kind == SK::File) {
                if (it.path.rfind(L"\\\\", 0) != 0) {
                    if (GetFileAttributesExW(it.path.c_str(), GetFileExInfoStandard, &info)) {
                        it.isDir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                        if (!it.isDir) it.size = ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
                    } else {
                        it.missing = true;
                    }
                }
            } else if (!it.path.empty()) {
                if (GetFileAttributesExW((BlobDir() + L"\\" + it.path).c_str(), GetFileExInfoStandard, &info)) {
                    it.size = ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
                    if (it.kind == SK::Text) it.size = std::max<unsigned long long>(it.size / 2, 4096);
                } else {
                    it.missing = true;
                }
            }
            nextId_ = std::max(nextId_, it.id + 1);
            loaded.push_back(std::move(it));
        }
        if (bad || (!header && !raw.empty())) {  // Keep the unreadable file: it may hold the only list.
            MoveFileExW(index.c_str(), (index + L".bad").c_str(), MOVEFILE_REPLACE_EXISTING);
            return;
        }
        items_ = std::move(loaded);
        // Orphan blobs are removed only after a good parse.
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((BlobDir() + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                bool used = false;
                for (const ShelfItem& it : items_) if (it.kind != SK::File && it.path == fd.cFileName) used = true;
                if (!used) DeleteFileW((BlobDir() + L"\\" + fd.cFileName).c_str());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    void Save() {
        const std::wstring index = ShelfDir() + L"\\index.txt";
        if (items_.empty()) {
            DeleteFileW(index.c_str());
            return;
        }
        std::wstring out = L"TBSHELF 2\n";
        for (const ShelfItem& it : items_) {
            const std::wstring head = std::to_wstring(it.id) + L"\t" + std::to_wstring(it.addedAt) + L"\t";
            switch (it.kind) {
                case SK::File: out += L"F\t" + head + EscapeField(it.path); break;
                case SK::Text: out += L"T\t" + head + EscapeField(it.title) + L"\t" + EscapeField(it.path) + L"\t" + EscapeField(it.text); break;
                case SK::Link: out += L"U\t" + head + EscapeField(it.title) + L"\t" + EscapeField(it.text); break;
                case SK::Image: out += L"I\t" + head + EscapeField(it.title) + L"\t" + EscapeField(it.path); break;
                case SK::Stored: out += L"S\t" + head + EscapeField(it.title) + L"\t" + EscapeField(it.path); break;
            }
            out += L"\n";
        }
        SHCreateDirectoryExW(nullptr, ShelfDir().c_str(), nullptr);
        if (!WriteFileAtomic(index, Utf8(out))) Flash(L"Could not save the shelf");
    }

    std::vector<ShelfItem> items_;
    bool loaded_ = false;
    unsigned nextId_ = 1;
    int scroll_ = 0;
    int press_ = 0;
    D2D1_POINT_2F pressPt_{};
    std::wstring status_;
    ULONGLONG confirmUntil_ = 0, statusUntil_ = 0;
    std::vector<Offer> lastIngest_;  // Rows the last Ingest added or promoted.
    std::vector<Offer> offer_;       // Rows the drop card is asking about.
    ULONGLONG choiceUntil_ = 0;
    Spring choice_;                  // The card: 0 gone, 1 up.
    double choiceT_ = 0;
};

ShelfPage g_shelf;
IModule* ShelfModule() { return &g_shelf; }
void ShelfThumbReady(unsigned id, std::shared_ptr<Pixmap> pm) { g_shelf.SetThumb(id, std::move(pm)); }

// ---- Drop target: one object for the bar window and the island ----
class ShelfDropTarget : public IDropTarget {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override {
        if (r == IID_IUnknown || r == IID_IDropTarget) { *o = this; return S_OK; }
        *o = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* d, DWORD, POINTL pt, DWORD* eff) override {
        accept_ = !g_shelfInternalDrag && AcceptsData(d);
        *eff = accept_ ? PickDropEffect(*eff) : DROPEFFECT_NONE;
        if (!helper_) CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_IDropTargetHelper, reinterpret_cast<void**>(helper_.Put()));
        POINT p{pt.x, pt.y};
        HWND at = WindowFromPoint(p);
        if (at != g_hwnd && at != g_isHwnd) at = g_hwnd;
        if (helper_) helper_->DragEnter(at, d, &p, *eff);
        if (accept_) {
            g_dropActive = true;
            g_dropLeft = 0;
            SetTimer(g_hwnd, kDropTimer, 120, nullptr);  // A passing drag does not flash the island open
            IslandRepaint();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* eff) override {
        // Allowed effects are not repeated here; COPY is what the source was told on enter.
        *eff = accept_ ? (*eff & DROPEFFECT_COPY ? DROPEFFECT_COPY : PickDropEffect(*eff)) : DROPEFFECT_NONE;
        POINT p{pt.x, pt.y};
        if (helper_) helper_->DragOver(&p, *eff);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (helper_) helper_->DragLeave();
        Left();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* d, DWORD, POINTL pt, DWORD* eff) override {
        POINT p{pt.x, pt.y};
        *eff = accept_ ? PickDropEffect(*eff ? *eff : (DROPEFFECT_COPY | DROPEFFECT_LINK)) : DROPEFFECT_NONE;
        if (helper_) helper_->Drop(d, &p, *eff);
        if (accept_) {
            IslandOpenShelf();  // Stay on the shelf after the drop
            if (g_shelf.Ingest(d) == 0) *eff = DROPEFFECT_NONE;
            else g_shelf.OfferPhone();  // Keep in bar, or send to the phone
        }
        Left();
        return S_OK;
    }
private:
    void Left() {
        g_dropActive = false;
        g_dropLeft = GetTickCount64();
        KillTimer(g_hwnd, kDropTimer);
        IslandRepaint();
    }
    Com<IDropTargetHelper> helper_;
    bool accept_ = false;
};

ShelfDropTarget g_dropTarget;
bool g_integrityLogged = false;

void ShelfDropRegister(HWND hwnd) {
    if (!hwnd) return;
    if (!g_integrityLogged) {
        g_integrityLogged = true;
        HANDLE tok = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
            DWORD need = 0;
            GetTokenInformation(tok, TokenIntegrityLevel, nullptr, 0, &need);
            std::vector<BYTE> buf(need + 8);
            if (need && GetTokenInformation(tok, TokenIntegrityLevel, buf.data(), (DWORD)buf.size(), &need)) {
                auto* l = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data());
                const DWORD rid = *GetSidSubAuthority(l->Label.Sid, *GetSidSubAuthorityCount(l->Label.Sid) - 1);
                if (rid > SECURITY_MANDATORY_MEDIUM_RID) Wh_Log(L"top-bar: running above medium integrity, drops from normal apps will be refused");
            }
            CloseHandle(tok);
        }
    }
    RegisterDragDrop(hwnd, &g_dropTarget);
}

void ShelfDropRevoke(HWND hwnd) {
    if (hwnd) RevokeDragDrop(hwnd);
}

void ShelfDropDwell() {
    KillTimer(g_hwnd, kDropTimer);
    if (g_dropActive) IslandOpenShelf();
}
