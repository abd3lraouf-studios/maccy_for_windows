#include "HistoryRenderer.h"
#include "UiFont.h"
#include "ClipboardRules.h"
#include "GdiScope.h"
#include "Localization.h"
#include "Theme.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <optional>
#include <regex>
#include <filesystem>

#include <shellapi.h>
#include <shlobj.h>

#include "resource.h"

namespace {

// Row geometry in DIPs (96 DPI); scale with SearchHeaderLayout::Scale before use.
constexpr int kHistoryItemInset = 2;
constexpr int kHistoryItemLeftPadding = 10;
constexpr int kHistoryItemRightPadding = 10;
constexpr int kHistoryItemSlot = 16;
constexpr int kHistoryFileIconSize = 16;
constexpr int kHistoryItemSlotGap = 6;
constexpr int kHistoryShortcutWidth = 74;
constexpr int kHistoryItemRadius = 4;
constexpr int kSeparatorInset = 16;

std::wstring ReadWindowText(CWindow window) {
    if (window.m_hWnd == nullptr) {
        return {};
    }
    const int length = window.GetWindowTextLength();
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int copied = window.GetWindowText(text.data(), length + 1);
    text.resize(static_cast<size_t>(std::max(copied, 0)));
    return text;
}

int HexDigit(wchar_t character) {
    if (character >= L'0' && character <= L'9') {
        return character - L'0';
    }
    if (character >= L'a' && character <= L'f') {
        return character - L'a' + 10;
    }
    if (character >= L'A' && character <= L'F') {
        return character - L'A' + 10;
    }
    return -1;
}

std::optional<COLORREF> ParseHexColor(std::wstring_view text) {
    const size_t digit_start = !text.empty() && text.front() == L'#' ? 1 : 0;
    const size_t digit_count = text.size() - digit_start;
    if (digit_count != 3 && digit_count != 6) {
        return std::nullopt;
    }

    unsigned int value = 0;
    for (size_t index = digit_start; index < text.size(); ++index) {
        const int digit = HexDigit(text[index]);
        if (digit < 0) {
            return std::nullopt;
        }
        value = (value << 4) | static_cast<unsigned int>(digit);
    }

    if (digit_count == 3) {
        const unsigned int red = ((value >> 8) & 0x0F) * 0x11;
        const unsigned int green = ((value >> 4) & 0x0F) * 0x11;
        const unsigned int blue = (value & 0x0F) * 0x11;
        return RGB(red, green, blue);
    }

    return RGB((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
}

// The DC brush takes any palette colour without creating a GDI brush per row.
void FillRoundRect(HDC dc, const RECT &rect, int radius, COLORREF color) {
    ScopedGdiObjectSelection null_pen(dc, GetStockObject(NULL_PEN));
    ScopedGdiObjectSelection brush(dc, GetStockObject(DC_BRUSH));
    const COLORREF previous = SetDCBrushColor(dc, color);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SetDCBrushColor(dc, previous);
}

} // namespace

HistoryRenderer::HistoryRenderer(AppSettings& settings)
    : m_settings(settings) {}

HistoryRenderer::~HistoryRenderer() {
    Shutdown();
}

bool HistoryRenderer::Initialize(CWindow owner) {
    m_owner = owner.m_hWnd;
    if (!m_iconThread.joinable()) {
        m_iconThread = std::jthread([this](std::stop_token stop_token) {
            IconThreadMain(stop_token);
        });
    }
    return UpdateFonts(UiFont::DpiForWindow(owner.m_hWnd));
}

bool HistoryRenderer::UpdateFonts(UINT dpi) {
    CFont normalFont;
    CFont smallFont;
    CFont boldFont;
    CFont italicFont;
    CFont underlineFont;
    normalFont.Attach(UiFont::CreateUiFont(dpi, UiFont::kBodyPointSize));
    smallFont.Attach(UiFont::CreateUiFont(dpi, UiFont::kSmallPointSize));
    boldFont.Attach(UiFont::CreateUiFont(dpi, UiFont::kBodyPointSize, FW_BOLD));
    italicFont.Attach(UiFont::CreateUiFont(dpi, UiFont::kBodyPointSize, FW_NORMAL, TRUE));
    underlineFont.Attach(UiFont::CreateUiFont(dpi, UiFont::kBodyPointSize, FW_NORMAL, FALSE, TRUE));
    if (normalFont.IsNull() || smallFont.IsNull() || boldFont.IsNull() ||
        italicFont.IsNull() || underlineFont.IsNull()) {
        return false;
    }

    // Windows text scaling can make the body font larger than the DPI-scaled
    // row and shortcut column, so both are sized from the font as well.
    CDC measure;
    if (!measure.CreateCompatibleDC(nullptr)) {
        return false;
    }
    ScopedGdiObjectSelection measure_font(measure.m_hDC, normalFont);
    TEXTMETRICW metrics{};
    SIZE shortcut{};
    // Pins are lower-case letters; "m" is the widest of them.
    if (!measure.GetTextMetrics(&metrics) ||
        !measure.GetTextExtent(L"Ctrl+m", -1, &shortcut)) {
        return false;
    }
    m_lineHeight = metrics.tmHeight;
    m_shortcutTextWidth = shortcut.cx;

    m_normalFont.DeleteObject();
    m_smallFont.DeleteObject();
    m_boldFont.DeleteObject();
    m_italicFont.DeleteObject();
    m_underlineFont.DeleteObject();
    m_normalFont.Attach(normalFont.Detach());
    m_smallFont.Attach(smallFont.Detach());
    m_boldFont.Attach(boldFont.Detach());
    m_italicFont.Attach(italicFont.Detach());
    m_underlineFont.Attach(underlineFont.Detach());
    m_fittedLines.clear();
    return true;
}

void HistoryRenderer::Shutdown() {
    if (m_iconThread.joinable()) {
        m_iconThread.request_stop();
        m_iconCv.notify_all();
        m_iconThread.join();
    }
    {
        std::lock_guard lock(m_iconResultMutex);
        m_iconResults.clear();
    }
    m_iconPosted.store(false);
    m_owner = nullptr;

    m_normalFont.DeleteObject();
    m_smallFont.DeleteObject();
    m_boldFont.DeleteObject();
    m_italicFont.DeleteObject();
    m_underlineFont.DeleteObject();
    m_lineHeight = 0;
    m_shortcutTextWidth = 0;

    m_iconCache.clear();
    m_iconLru.clear();
    m_iconQueued.clear();
    m_fittedLines.clear();
    m_imageThumbnails.clear();
    m_unpinnedShortcutNumbers.clear();
    m_highlightQuery.clear();
    m_highlightRegex.reset();
    m_highlightPatternReady = false;
}

void HistoryRenderer::PrepareHistory(const std::vector<ClipboardItem> &items) {
    m_fittedLines.clear();
    m_unpinnedShortcutNumbers.assign(items.size(), 0);
    int number = 0;
    for (size_t index = 0; index < items.size(); ++index) {
        if (!items[index].pinned) {
            ++number;
            if (number <= 9) {
                m_unpinnedShortcutNumbers[index] = number;
            }
        }
    }
}

void HistoryRenderer::SetImageThumbnails(std::unordered_map<sqlite3_int64, PreviewBitmap> thumbnails) {
    m_imageThumbnails = std::move(thumbnails);
}

void HistoryRenderer::SetImageThumbnail(sqlite3_int64 id, PreviewBitmap thumbnail) {
    m_imageThumbnails.insert_or_assign(id, std::move(thumbnail));
}

const PreviewBitmap* HistoryRenderer::ImageThumbnail(sqlite3_int64 id) const noexcept {
    const auto it = m_imageThumbnails.find(id);
    return it == m_imageThumbnails.end() ? nullptr : &it->second;
}

void HistoryRenderer::ClearImageThumbnails() noexcept {
    m_imageThumbnails.clear();
}

std::wstring HistoryRenderer::DisplayText(const ClipboardItem& item) const {
    if (item.has_files) {
        std::wstring names;
        for (const auto &path : item.file_paths) {
            if (!names.empty()) names += L"; ";
            auto file = std::filesystem::path(path);
            if (file.filename().empty()) file = file.parent_path();
            names += file.filename().empty() ? file.root_path().wstring() : file.filename().wstring();
        }
        return names.empty() ? Localization::Text(IDS_CONTENT_FILE) : names;
    }
    const std::wstring image_placeholder = Localization::Text(IDS_CONTENT_IMAGE);
    if (!item.title.empty() && item.title != image_placeholder) {
        return item.title;
    }
    if (!item.preview.empty() && item.preview != image_placeholder) {
        return ClipboardRules::PreviewText(item.preview);
    }
    if (item.has_image) {
        return {};
    }
    if (item.has_files) {
        return Localization::Text(IDS_CONTENT_FILE);
    }
    return Localization::Text(IDS_CONTENT_CLIPBOARD_ITEM);
}

HistoryRenderer::HistoryItemLayout HistoryRenderer::LayoutHistoryItem(
    const RECT& row,
    const ClipboardItem& item,
    bool has_color_swatch,
    UINT dpi
) const {
    const int inset = SearchHeaderLayout::Scale(kHistoryItemInset, dpi);
    const int left_padding = SearchHeaderLayout::Scale(kHistoryItemLeftPadding, dpi);
    const int right_padding = SearchHeaderLayout::Scale(kHistoryItemRightPadding, dpi);
    const int slot = SearchHeaderLayout::Scale(kHistoryItemSlot, dpi);
    const int gap = SearchHeaderLayout::Scale(kHistoryItemSlotGap, dpi);
    const int shortcut_width = std::max(SearchHeaderLayout::Scale(kHistoryShortcutWidth, dpi),
                                        m_shortcutTextWidth);

    HistoryItemLayout layout{};
    layout.background = row;
    layout.background.left += inset;
    layout.background.right -= inset;
    layout.shortcut = row;
    layout.shortcut.left = std::max(row.left, row.right - right_padding - shortcut_width);
    layout.shortcut.right = row.right - right_padding;
    int x = row.left + left_padding;
    const int y = row.top + std::max<LONG>(0, ((row.bottom - row.top) - slot) / 2);
    if (m_settings.show_application_icons && !item.application.empty()) {
        layout.icon = {x, y, x + slot, y + slot};
        x += slot + gap;
    }
    if (item.has_image && !item.has_files) {
        if (const auto *thumbnail = ImageThumbnail(item.id); thumbnail != nullptr) {
            const int top = row.top + std::max<LONG>(0, ((row.bottom - row.top) - thumbnail->height) / 2);
            layout.attachment = {x, top, x + thumbnail->width, top + thumbnail->height};
            x += thumbnail->width + gap;
        } else {
            layout.attachment = {x, y, x + slot, y + slot};
            x += slot + gap;
        }
    } else if (item.has_files) {
        const int size = SearchHeaderLayout::Scale(kHistoryFileIconSize, dpi);
        const int top = row.top + std::max<LONG>(0, ((row.bottom - row.top) - size) / 2);
        layout.attachment = {x, top, x + size, top + size};
        x += size + gap;
    }
    if (has_color_swatch) {
        layout.swatch = {x, y, x + slot, y + slot};
        x += slot + gap;
    }
    layout.content = row;
    layout.content.left = x;
    layout.content.right = std::max<LONG>(x, layout.shortcut.left - gap);
    return layout;
}

HFONT HistoryRenderer::FontForHighlight(HighlightMatch match) const {
    switch (match) {
    case HighlightMatch::Bold:
        return m_boldFont;
    case HighlightMatch::Italic:
        return m_italicFont;
    case HighlightMatch::Underline:
        return m_underlineFont;
    case HighlightMatch::Color:
    default:
        return m_normalFont;
    }
}

void HistoryRenderer::PrepareHighlightPattern(std::wstring_view searchQuery) {
    if (m_highlightPatternReady && m_highlightQuery == searchQuery &&
        m_highlightMode == m_settings.search_mode) {
        return;
    }

    m_highlightQuery.assign(searchQuery);
    m_highlightMode = m_settings.search_mode;
    m_highlightRegex.reset();
    if (!searchQuery.empty() &&
        (m_highlightMode == SearchMode::Regexp || m_highlightMode == SearchMode::Mixed)) {
        try {
            m_highlightRegex.emplace(std::wstring(searchQuery));
        } catch (const std::regex_error&) {
            m_highlightRegex.reset();
        }
    }
    m_highlightPatternReady = true;
}

std::vector<std::pair<size_t, size_t>> HistoryRenderer::HighlightRanges(
    std::wstring_view text, std::wstring_view searchQuery) {
    PrepareHighlightPattern(searchQuery);
    std::vector<std::pair<size_t, size_t>> ranges;
    if (searchQuery.empty()) {
        return ranges;
    }
    const auto add_exact = [&]() {
        const std::wstring lower_text = ClipboardRules::Lower(text);
        const std::wstring lower_query = ClipboardRules::Lower(searchQuery);
        if (lower_query.empty()) {
            return;
        }
        size_t position = 0;
        while ((position = lower_text.find(lower_query, position)) != std::wstring::npos) {
            ranges.emplace_back(position, position + lower_query.size());
            position += lower_query.size();
        }
    };
    const auto add_regex = [&]() {
        if (!m_highlightRegex) {
            return;
        }
        const std::wstring_view haystack = ClipboardRules::BoundedRegexHaystack(text);
        const std::wstring value(haystack);
        for (std::wsregex_iterator it(value.begin(), value.end(), *m_highlightRegex), end;
             it != end; ++it) {
            const auto match = *it;
            ranges.emplace_back(
                static_cast<size_t>(match.position()),
                static_cast<size_t>(match.position() + match.length())
            );
        }
    };
    const auto add_fuzzy = [&]() {
        const std::wstring lower_text = ClipboardRules::Lower(text);
        const std::wstring lower_query = ClipboardRules::Lower(searchQuery);
        size_t text_position = 0;
        size_t start = std::wstring::npos;
        size_t last = std::wstring::npos;
        for (const wchar_t expected : lower_query) {
            const size_t found = lower_text.find(expected, text_position);
            if (found == std::wstring::npos) {
                ranges.clear();
                return;
            }
            if (start == std::wstring::npos) {
                start = found;
            }
            last = found;
            ranges.emplace_back(found, found + 1);
            text_position = found + 1;
        }
    };

    switch (m_settings.search_mode) {
    case SearchMode::Regexp:
        add_regex();
        break;
    case SearchMode::Fuzzy:
        add_fuzzy();
        break;
    case SearchMode::Mixed:
        add_exact();
        if (ranges.empty()) {
            add_regex();
        }
        if (ranges.empty()) {
            add_fuzzy();
        }
        break;
    case SearchMode::Exact:
    default:
        add_exact();
        break;
    }
    return ranges;
}

const HistoryRenderer::FittedLine &HistoryRenderer::FittedLineFor(
    sqlite3_int64 item_id,
    std::wstring_view text,
    int width,
    std::wstring_view search_query,
    HDC dc
) {
    FittedLine &slot = m_fittedLines[item_id];
    if (slot.width == width && slot.query == search_query) {
        return slot;
    }

    const auto original_ranges = HighlightRanges(text, search_query);
    const auto measure = [&](std::wstring_view value) {
        SIZE size{};
        ScopedGdiObjectSelection font_selection(
            dc,
            m_boldFont != nullptr ? m_boldFont : GetStockObject(DEFAULT_GUI_FONT)
        );
        GetTextExtentPoint32W(dc, value.data(), static_cast<int>(value.size()), &size);
        return size.cx;
    };
    std::wstring display(text);
    size_t prefix = text.size();
    size_t suffix = 0;
    if (width > 0 && measure(text) > width) {
        size_t low = 0;
        size_t high = text.size();
        while (low < high) {
            const size_t length = (low + high + 1) / 2;
            const std::wstring candidate = std::wstring(text.substr(0, (length + 1) / 2)) + L"…" +
                std::wstring(text.substr(text.size() - length / 2));
            if (measure(candidate) <= width) {
                low = length;
            } else {
                high = length - 1;
            }
        }
        prefix = (low + 1) / 2;
        suffix = low / 2;
        if (prefix != 0 && text[prefix - 1] >= 0xd800 && text[prefix - 1] <= 0xdbff) {
            --prefix;
        }
        if (suffix != 0 && text[text.size() - suffix] >= 0xdc00 && text[text.size() - suffix] <= 0xdfff) {
            --suffix;
        }
        display = std::wstring(text.substr(0, prefix)) + L"…" + std::wstring(text.substr(text.size() - suffix));
    }
    slot.ranges.clear();
    for (const auto &[start, end] : original_ranges) {
        if (start < prefix) {
            slot.ranges.emplace_back(start, std::min(end, prefix));
        }
        if (suffix != 0 && end > text.size() - suffix) {
            const size_t from = std::max(start, text.size() - suffix);
            slot.ranges.emplace_back(
                prefix + 1 + from - (text.size() - suffix),
                prefix + 1 + end - (text.size() - suffix)
            );
        }
    }
    slot.text = std::move(display);
    slot.width = width;
    slot.query.assign(search_query);
    return slot;
}

void HistoryRenderer::DrawTextWithHighlights(HDC dc, RECT rect, sqlite3_int64 item_id,
                                            std::wstring_view text,
                                            bool selected, std::wstring_view searchQuery) {
    ScopedDcState dc_state(dc);
    IntersectClipRect(dc, rect.left, rect.top, rect.right, rect.bottom);
    const int width = std::max(0L, rect.right - rect.left);
    const FittedLine &fitted = FittedLineFor(item_id, text, width, searchQuery, dc);
    text = fitted.text;
    const auto &ranges = fitted.ranges;
    ScopedGdiObjectSelection normal_font_selection(
        dc,
        m_normalFont != nullptr ? m_normalFont : GetStockObject(DEFAULT_GUI_FONT)
    );
    const Theme::Palette &palette = Theme::Current();
    const COLORREF row_text = selected ? palette.accent_text : palette.text;
    // A match on the selection bar is the inverse of the bar, which stays
    // legible on any accent; elsewhere the theme's own match pair.
    const COLORREF match_text = selected ? palette.accent : palette.match_text;
    const COLORREF match_background = selected ? palette.accent_text : palette.match;
    std::vector<std::pair<size_t, size_t>> segments;
    size_t position = 0;
    for (const auto& [start, end] : ranges) {
        if (start > position) {
            segments.emplace_back(position, start);
        }
        segments.emplace_back(start, std::max(start, end));
        position = std::max(position, end);
    }
    if (position < text.size()) {
        segments.emplace_back(position, text.size());
    }
    if (segments.empty()) {
        segments.emplace_back(0, text.size());
    }

    int x = rect.left;
    for (const auto& [start, end] : segments) {
        if (end <= start) {
            continue;
        }
        const bool highlighted = std::any_of(
            ranges.begin(),
            ranges.end(),
            [start, end](const auto& range) { return start >= range.first && end <= range.second; }
        );
        const std::wstring part(text.substr(start, end - start));
        SIZE size{};
        HFONT font = highlighted
            ? FontForHighlight(m_settings.highlight_match)
            : static_cast<HFONT>(m_normalFont);
        ScopedGdiObjectSelection font_selection(
            dc,
            font != nullptr ? font : GetStockObject(DEFAULT_GUI_FONT)
        );
        if (highlighted && m_settings.highlight_match == HighlightMatch::Color) {
            SetTextColor(dc, match_text);
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, match_background);
        } else {
            SetTextColor(dc, row_text);
            SetBkMode(dc, TRANSPARENT);
        }
        GetTextExtentPoint32W(dc, part.c_str(), static_cast<int>(part.size()), &size);
        TextOutW(dc, x, rect.top, part.c_str(), static_cast<int>(part.size()));
        x += size.cx;
        if (x >= rect.right) {
            break;
        }
    }
}

UniqueIcon HistoryRenderer::LoadShellIcon(const std::wstring &path) {
    SHFILEINFOW info{};
    if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) == 0) {
        SHGetFileInfoW(
            path.c_str(),
            FILE_ATTRIBUTE_NORMAL,
            &info,
            sizeof(info),
            SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES
        );
    }
    return UniqueIcon(info.hIcon);
}

void HistoryRenderer::QueueIcon(std::wstring key, std::wstring path) {
    if (key.empty() || m_iconQueued.contains(key) || m_iconCache.contains(key)) {
        return;
    }
    m_iconQueued.insert(key);
    {
        std::lock_guard lock(m_iconMutex);
        m_iconJobs.push_back(IconJob{std::move(key), std::move(path)});
    }
    m_iconCv.notify_one();
}

void HistoryRenderer::IconThreadMain(std::stop_token stop_token) {
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(com);
    std::stop_callback wake(stop_token, [this] { m_iconCv.notify_all(); });
    while (!stop_token.stop_requested()) {
        IconJob job;
        {
            std::unique_lock lock(m_iconMutex);
            m_iconCv.wait(lock, [&] {
                return stop_token.stop_requested() || !m_iconJobs.empty();
            });
            if (stop_token.stop_requested() || m_iconJobs.empty()) {
                break;
            }
            job = std::move(m_iconJobs.front());
            m_iconJobs.pop_front();
        }
        IconResult result{std::move(job.key), LoadShellIcon(job.path)};
        {
            std::lock_guard lock(m_iconResultMutex);
            m_iconResults.push_back(std::move(result));
        }
        if (!m_iconPosted.exchange(true) && m_owner != nullptr) {
            ::PostMessageW(m_owner, AppConstants::kHistoryIconMessage, 0, 0);
        }
    }
    if (uninitialize) {
        ::CoUninitialize();
    }
}

bool HistoryRenderer::ApplyLoadedIcons() {
    m_iconPosted.store(false);
    std::deque<IconResult> loaded;
    {
        std::lock_guard lock(m_iconResultMutex);
        loaded.swap(m_iconResults);
    }
    if (loaded.empty()) {
        return false;
    }
    for (IconResult &result : loaded) {
        m_iconQueued.erase(result.key);
        if (m_iconCache.contains(result.key)) {
            continue;
        }
        if (m_iconCache.size() >= 64) {
            const std::wstring evicted_key = m_iconLru.back();
            m_iconLru.pop_back();
            m_iconCache.erase(evicted_key);
        }
        m_iconLru.push_front(result.key);
        m_iconCache.emplace(result.key, IconCacheEntry{std::move(result.icon), m_iconLru.begin()});
    }
    return true;
}

HICON HistoryRenderer::IconForPath(std::wstring_view path) {
    if (path.empty()) {
        return nullptr;
    }
    const std::wstring key = ClipboardRules::NormalizePath(std::wstring(path));
    if (const auto found = m_iconCache.find(key); found != m_iconCache.end()) {
        m_iconLru.splice(m_iconLru.begin(), m_iconLru, found->second.lru);
        return found->second.icon.Get();
    }
    QueueIcon(key, std::wstring(path));
    return nullptr;
}

void HistoryRenderer::DrawHistoryItem(DRAWITEMSTRUCT* draw,
                                     const std::vector<ClipboardItem>& items,
                                     int activeIndex,
                                     int activeFooter,
                                     std::wstring_view searchQuery) {
    if (draw == nullptr || draw->itemID == static_cast<UINT>(-1) ||
        static_cast<size_t>(draw->itemData) >= items.size()) {
        return;
    }
    if (m_unpinnedShortcutNumbers.size() != items.size()) {
        PrepareHistory(items);
    }
    const int index = static_cast<int>(draw->itemData);
    const ClipboardItem& item = items[index];
    const bool selected = activeFooter < 0 && index == activeIndex;
    const std::wstring text = DisplayText(item);
    std::optional<COLORREF> swatch_color;
    if (m_settings.show_hex_color_swatch) {
        swatch_color = ParseHexColor(text);
    }
    const UINT dpi = UiFont::DpiForWindow(draw->hwndItem);
    const int radius = SearchHeaderLayout::Scale(kHistoryItemRadius, dpi);
    const HistoryItemLayout layout = LayoutHistoryItem(draw->rcItem, item,
        swatch_color.has_value(), dpi);
    // Compose the row off screen: its fill, selection and text then reach the
    // list in one copy instead of flashing in stages while it repaints.
    CMemoryDC canvas(draw->hDC, draw->rcItem);
    const HDC dc = canvas.m_hDC;
    const Theme::Palette &palette = Theme::Current();
    // Markers drawn beside the text: muted, or the selection's text colour on the bar.
    const COLORREF marker = selected ? palette.accent_text : palette.muted;
    FillRect(dc, &draw->rcItem, Theme::WindowBrush());
    if (selected) {
        RECT selected_rect = layout.background;
        const bool previous_selected = index > 0 && index - 1 == activeIndex;
        const bool next_selected = index + 1 < static_cast<int>(items.size()) && index + 1 == activeIndex;
        if (previous_selected) selected_rect.top = draw->rcItem.top;
        if (next_selected) selected_rect.bottom = draw->rcItem.bottom;
        FillRoundRect(dc, selected_rect, radius, palette.accent);
        if (previous_selected || next_selected) {
            RECT join = selected_rect;
            join.left += radius / 2;
            join.right -= radius / 2;
            canvas.FillSolidRect(&join, palette.accent);
        }
    }
    ScopedGdiObjectSelection normal_font_selection(
        dc,
        m_normalFont != nullptr ? m_normalFont : GetStockObject(DEFAULT_GUI_FONT)
    );

    TEXTMETRICW metrics{};
    const int text_height = GetTextMetricsW(dc, &metrics) != FALSE && metrics.tmHeight > 0
        ? metrics.tmHeight
        : 16;
    const int row_height = std::max(1L, draw->rcItem.bottom - draw->rcItem.top);
    const int text_top = draw->rcItem.top + std::max(0, (row_height - text_height) / 2);
    RECT text_rect = layout.content;
    text_rect.top = text_top;
    text_rect.bottom = text_top + text_height;

    if (!IsRectEmpty(&layout.icon)) {
        if (const HICON icon = IconForPath(item.application)) {
            DrawIconEx(dc, layout.icon.left, layout.icon.top, icon,
                layout.icon.right - layout.icon.left, layout.icon.bottom - layout.icon.top,
                0, nullptr, DI_NORMAL);
        }
    }
    if (!IsRectEmpty(&layout.attachment)) {
        const auto thumbnail = m_imageThumbnails.find(item.id);
        if (item.has_files) {
            if (!item.file_paths.empty()) {
                if (const HICON icon = IconForPath(item.file_paths.front())) {
                    DrawIconEx(dc, layout.attachment.left, layout.attachment.top,
                        icon, layout.attachment.right - layout.attachment.left,
                        layout.attachment.bottom - layout.attachment.top, 0, nullptr, DI_NORMAL);
                }
            }
        } else if (item.has_image && thumbnail != m_imageThumbnails.end() && thumbnail->second.handle != nullptr) {
            const int width = thumbnail->second.width;
            const int height = thumbnail->second.height;
            const int x = layout.attachment.left;
            const int y = draw->rcItem.top + std::max(0, (row_height - height) / 2);
            CDC memory;
            if (memory.CreateCompatibleDC(dc)) {
                ScopedGdiObjectSelection bitmap(memory.m_hDC, thumbnail->second.handle);
                if (bitmap.IsSelected()) {
                    // Blend over the row (window or selection colour) already painted here.
                    ::AlphaBlend(dc, x, y, width, height, memory.m_hDC, 0, 0, width, height,
                                 PreviewBitmap::kBlend);
                }
            }
        } else {
            // Placeholder frame until the image thumbnail arrives; only image
            // rows reach this branch.
            CPen marker_pen;
            if (marker_pen.CreatePen(PS_SOLID, 1, marker)) {
                ScopedGdiObjectSelection pen_selection(dc, marker_pen);
                ScopedGdiObjectSelection brush_selection(dc, GetStockObject(NULL_BRUSH));
                Rectangle(dc, layout.attachment.left + 1, layout.attachment.top + 2,
                    layout.attachment.right - 1, layout.attachment.bottom - 2);
            }
        }
    }

    if (!IsRectEmpty(&layout.swatch) && swatch_color.has_value()) {
        CBrush brush;
        if (brush.CreateSolidBrush(*swatch_color)) {
            FillRect(dc, &layout.swatch, brush);
        }
        // The frame keeps a swatch of the row's own colour visible. The stock
        // DC brush paints in the colour set on this DC.
        const COLORREF previous = SetDCBrushColor(dc, marker);
        FrameRect(dc, &layout.swatch, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        SetDCBrushColor(dc, previous);
    }

    std::wstring shortcut;
    // Ctrl+1..9 paste unpinned items, so a pin keyed 1-9 by an older build
    // has no shortcut until it is given another key.
    const bool quick_paste_digit = item.pin.size() == 1 && item.pin[0] >= L'1' && item.pin[0] <= L'9';
    if (item.pinned) {
        if (!item.pin.empty() && !quick_paste_digit) shortcut = L"Ctrl+" + item.pin;
    } else if (m_unpinnedShortcutNumbers[static_cast<size_t>(index)] > 0) {
        shortcut = L"Ctrl+" + std::to_wstring(
            m_unpinnedShortcutNumbers[static_cast<size_t>(index)]
        );
    }
    RECT keyRect = layout.shortcut;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, marker);
    DrawTextW(dc, shortcut.c_str(), -1, &keyRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    text_rect.right = layout.content.right;
    DrawTextWithHighlights(dc, text_rect, item.id, text, selected, searchQuery);
}

void HistoryRenderer::DrawMenuButton(DRAWITEMSTRUCT* draw,
                                    int activeFooter,
                                    const std::array<CButton, AppConstants::UI::kFooterButtonCount>& footerButtons) {
    auto it = std::find_if(footerButtons.begin(), footerButtons.end(), [draw](const CButton& button) {
        return button.m_hWnd == draw->hwndItem;
    });
    const int index = it == footerButtons.end() ? -1 : static_cast<int>(it - footerButtons.begin());
    const bool selected = index >= 0 && index == activeFooter;
    const UINT dpi = UiFont::DpiForWindow(draw->hwndItem);
    const int inset = SearchHeaderLayout::Scale(kHistoryItemInset, dpi);
    const int radius = SearchHeaderLayout::Scale(kHistoryItemRadius, dpi);
    const int left_padding = SearchHeaderLayout::Scale(kHistoryItemLeftPadding, dpi);
    const int right_padding = SearchHeaderLayout::Scale(kHistoryItemRightPadding, dpi);
    // Off screen for the same reason as the history rows.
    CMemoryDC canvas(draw->hDC, draw->rcItem);
    const HDC dc = canvas.m_hDC;
    const Theme::Palette &palette = Theme::Current();
    const COLORREF marker = selected ? palette.accent_text : palette.muted;
    FillRect(dc, &draw->rcItem, Theme::WindowBrush());
    if (selected || (draw->itemState & ODS_SELECTED)) {
        RECT selected_rect = draw->rcItem;
        selected_rect.left += inset;
        selected_rect.right -= inset;
        FillRoundRect(dc, selected_rect, radius, selected ? palette.accent : palette.surface);
    }
    ScopedGdiObjectSelection normal_font_selection(
        dc,
        m_normalFont != nullptr ? m_normalFont : GetStockObject(DEFAULT_GUI_FONT)
    );
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, selected ? palette.accent_text : palette.text);
    RECT rect = draw->rcItem;
    rect.left += left_padding;
    rect.right -= right_padding;
    if (draw->CtlID == IDC_HISTORY_PREVIEW) {
        const int width = draw->rcItem.right - draw->rcItem.left;
        const int height = draw->rcItem.bottom - draw->rcItem.top;
        const int iconWidth = std::min(width - MulDiv(6, static_cast<int>(dpi), 96),
                                       MulDiv(18, static_cast<int>(dpi), 96));
        const int iconHeight = std::min(height - MulDiv(6, static_cast<int>(dpi), 96),
                                        MulDiv(14, static_cast<int>(dpi), 96));
        const int left = draw->rcItem.left + (width - iconWidth) / 2;
        const int top = draw->rcItem.top + (height - iconHeight) / 2;
        CPen pen;
        if (pen.CreatePen(PS_SOLID, 1, marker)) {
            ScopedGdiObjectSelection pen_selection(dc, pen);
            ScopedGdiObjectSelection brush_selection(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, left, top, left + iconWidth, top + iconHeight);
            const int divider = left + iconWidth / 2;
            MoveToEx(dc, divider, top, nullptr);
            LineTo(dc, divider, top + iconHeight);
        }
        return;
    }
    const auto title = ReadWindowText(CWindow(draw->hwndItem));
    RECT key_rect{};
    const wchar_t* key = nullptr;
    if (index >= 0) {
        const wchar_t* keys[] = {
            (GetKeyState(VK_SHIFT) & 0x8000) ? L"Ctrl+Alt+Shift+Backspace" : L"Ctrl+Alt+Backspace",
            L"Ctrl+,",
            L"",
            L"Ctrl+Q"
        };
        key = keys[index];
        if (*key != L'\0') {
            SIZE key_size{};
            GetTextExtentPoint32W(dc, key, static_cast<int>(wcslen(key)), &key_size);
            key_rect = draw->rcItem;
            key_rect.right -= right_padding;
            key_rect.left = std::max<LONG>(rect.left, key_rect.right - key_size.cx);
            rect.right = std::max<LONG>(rect.left,
                key_rect.left - SearchHeaderLayout::Scale(kHistoryItemSlotGap, dpi));
        }
    }
    DrawTextW(dc, title.c_str(), -1, &rect,
        DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_LEFT);
    if (key != nullptr && *key != L'\0') {
        SetTextColor(dc, marker);
        DrawTextW(dc, key, -1, &key_rect,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
}

void HistoryRenderer::OnPaint(HDC windowDc, const RECT& client,
                             int pinSeparatorY,
                             int footerSeparatorY,
                             const SearchHeaderLayout::Geometry& header,
                             bool showSearchClear) {
    // Compose the invalid part off screen and copy it once. Painting the
    // background fill straight to the window flickers during resizes.
    RECT paint{};
    if (::GetClipBox(windowDc, &paint) == ERROR) {
        paint = client;
    }
    if (IsRectEmpty(&paint)) {
        return;
    }
    CMemoryDC canvas(windowDc, paint);
    const HDC dc = canvas.m_hDC;
    const Theme::Palette &palette = Theme::Current();
    FillRect(dc, &client, Theme::WindowBrush());
    CPen separator;
    if (separator.CreatePen(PS_SOLID, 1, palette.border)) {
        ScopedGdiObjectSelection pen_selection(dc, separator);
        const int inset = SearchHeaderLayout::Scale(kSeparatorInset, UiFont::DpiForWindow(m_owner));
        for (int y : {pinSeparatorY, footerSeparatorY}) if (y >= 0) {
            MoveToEx(dc, inset, y, nullptr); LineTo(dc, client.right - inset, y);
        }
    }
    if (header.showSearch) {
        FillRoundRect(dc, header.search, header.metrics.cornerRadius, palette.surface);
        CPen iconPen;
        if (iconPen.CreatePen(PS_SOLID, 1, palette.muted)) {
            {
                ScopedGdiObjectSelection pen_selection(dc, iconPen);
                ScopedGdiObjectSelection brush_selection(dc, GetStockObject(NULL_BRUSH));
                const SearchHeaderLayout::IconGeometry icon = SearchHeaderLayout::IconFor(header);
                Ellipse(dc, icon.left, icon.top,
                        icon.left + header.metrics.iconLensSize,
                        icon.top + header.metrics.iconLensSize);
                MoveToEx(dc, icon.left + header.metrics.iconHandleStart,
                         icon.top + header.metrics.iconHandleStart, nullptr);
                LineTo(dc, icon.left + header.metrics.iconHandleEnd,
                       icon.top + header.metrics.iconHandleEnd);
                if (showSearchClear && !IsRectEmpty(&header.searchClear)) {
                    const int centerX = (header.searchClear.left + header.searchClear.right) / 2;
                    const int centerY = (header.searchClear.top + header.searchClear.bottom) / 2;
                    const int halfSize = std::max(2, MulDiv(3, header.metrics.clearWidth,
                                                             SearchHeaderLayout::kClearWidth));
                    MoveToEx(dc, centerX - halfSize, centerY - halfSize, nullptr);
                    LineTo(dc, centerX + halfSize + 1, centerY + halfSize + 1);
                    MoveToEx(dc, centerX + halfSize, centerY - halfSize, nullptr);
                    LineTo(dc, centerX - halfSize - 1, centerY + halfSize + 1);
                }
            }
        }
    }
    if (m_settings.show_title && header.showTitle && !IsRectEmpty(&header.title)) {
        const HFONT title_font = m_smallFont.IsNull()
            ? static_cast<HFONT>(m_normalFont)
            : static_cast<HFONT>(m_smallFont);
        ScopedGdiObjectSelection font_selection(dc, title_font);
        const COLORREF previous_color = SetTextColor(dc, palette.muted);
        const int previous_mode = SetBkMode(dc, TRANSPARENT);
        RECT title = header.title;
        DrawTextW(dc, L"maccy", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SetBkMode(dc, previous_mode);
        SetTextColor(dc, previous_color);
    }
}
