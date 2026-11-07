
// ---- Text: DirectWrite layouts, cached ----

struct TextOpts {
    const wchar_t* family = nullptr;  // null: the bar's font
    float size = 0;                   // px, 0: the bar's size
    float maxW = 1e6f;
    float maxH = 1e6f;
    bool ellipsis = false;
    bool center = false;              // centred both ways inside maxW x maxH
    int weight = 400;                 // DWRITE_FONT_WEIGHT: 600 semibold, 700 bold
    bool right = false;               // right-aligned inside maxW
    bool wrap = false;                // wrap words inside maxW
};

class TextService {
public:
    bool Init() {
        return SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                             reinterpret_cast<IUnknown**>(factory_.Put())));
    }
    // The first installed family of: the user's choice, Segoe UI Variable Text, Segoe UI.
    void SetFont(const std::wstring& preferred, float sizePx) {
        family_ = PickFamily(preferred);
        size_ = sizePx;
        cache_.clear();
    }
    float DefaultSize() const { return size_; }

    Com<IDWriteTextLayout> Layout(const std::wstring& text, const TextOpts& o = {}) {
        if (!factory_ || text.empty()) return {};
        const wchar_t* family = o.family ? o.family : family_.c_str();
        const float size = o.size > 0 ? o.size : size_;
        wchar_t head[160];
        swprintf_s(head, L"%ls|%.2f|%.1f|%.1f|%d%d%d%d|%d|", family, size, o.maxW, o.maxH, o.ellipsis, o.center, o.right, o.wrap, o.weight);
        const std::wstring key = std::wstring(head) + text;
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;
        if (cache_.size() > 200) cache_.clear();
        Com<IDWriteTextFormat> fmt;
        if (FAILED(factory_->CreateTextFormat(family, nullptr, (DWRITE_FONT_WEIGHT)o.weight, DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, size, L"", fmt.Put()))) {
            return {};
        }
        Com<IDWriteTextLayout> layout;
        if (FAILED(factory_->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt.Get(), o.maxW, o.maxH,
                                              layout.Put()))) {
            return {};
        }
        layout->SetWordWrapping(o.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (o.right) layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        if (o.center) {
            layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        } else if (o.maxH < 1e5f) {
            layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (o.ellipsis) {
            Com<IDWriteInlineObject> sign;
            if (SUCCEEDED(factory_->CreateEllipsisTrimmingSign(layout.Get(), sign.Put()))) {
                DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                layout->SetTrimming(&trimming, sign.Get());
            }
        }
        cache_[key] = layout;
        return layout;
    }

    // Width including trailing spaces, as the GDI+ build measured it.
    float Measure(const std::wstring& text, float size = 0) {
        TextOpts o;
        o.size = size;
        Com<IDWriteTextLayout> layout = Layout(text, o);
        if (!layout) return 0;
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
    }

    void Draw(ID2D1RenderTarget* rt, const std::wstring& text, float x, float y, ID2D1Brush* brush,
              const TextOpts& o = {}) {
        Com<IDWriteTextLayout> layout = Layout(text, o);
        if (layout) rt->DrawTextLayout(D2D1::Point2F(x, y), layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

private:
    std::wstring PickFamily(const std::wstring& preferred) {
        Com<IDWriteFontCollection> fonts;
        if (!factory_ || FAILED(factory_->GetSystemFontCollection(fonts.Put(), FALSE))) return L"Segoe UI";
        auto has = [&](const wchar_t* name) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            return SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists;
        };
        if (!preferred.empty() && has(preferred.c_str())) return preferred;
        if (has(L"Segoe UI Variable Text")) return L"Segoe UI Variable Text";
        return L"Segoe UI";
    }
    Com<IDWriteFactory> factory_;
    std::wstring family_ = L"Segoe UI";
    float size_ = 13;
    std::unordered_map<std::wstring, Com<IDWriteTextLayout>> cache_;
};
