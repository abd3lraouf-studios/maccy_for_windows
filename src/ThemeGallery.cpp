#include "ThemeGallery.h"

#include "Theme.h"

#include <atlgdi.h>
#include <atltypes.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <string>

namespace {

constexpr int kColumns = 4;
constexpr int kRows = (kThemeFamilyCount + kColumns - 1) / kColumns;

// Every size below is in pixels at 100% and scaled to the window's DPI.
int Scale(int value, UINT dpi) {
    return ::MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

// Cells split the client area evenly, so the grid always fills the control.
CRect CellRect(const CRect &client, int index) {
    const int column = index % kColumns;
    const int row = index / kColumns;
    return CRect(client.left + client.Width() * column / kColumns,
                 client.top + client.Height() * row / kRows,
                 client.left + client.Width() * (column + 1) / kColumns,
                 client.top + client.Height() * (row + 1) / kRows);
}

// The DC brush takes any palette colour without a GDI brush per shape. With
// a null pen RoundRect fills one pixel short on the right and bottom.
void FillRound(CDCHandle dc, const CRect &rect, int radius, COLORREF color) {
    const HPEN previous_pen = dc.SelectStockPen(NULL_PEN);
    const HBRUSH previous_brush = dc.SelectStockBrush(DC_BRUSH);
    dc.SetDCBrushColor(color);
    dc.RoundRect(rect.left, rect.top, rect.right + 1, rect.bottom + 1, radius, radius);
    dc.SelectBrush(previous_brush);
    dc.SelectPen(previous_pen);
}

// An outline drawn inside the rectangle, so rings of any width share one edge.
void Ring(CDCHandle dc, const CRect &rect, int radius, int width, COLORREF color) {
    CPen pen;
    if (pen.CreatePen(PS_SOLID | PS_INSIDEFRAME, width, color) == nullptr) {
        return;
    }
    const HPEN previous_pen = dc.SelectPen(pen);
    const HBRUSH previous_brush = dc.SelectStockBrush(HOLLOW_BRUSH);
    dc.RoundRect(rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    dc.SelectBrush(previous_brush);
    dc.SelectPen(previous_pen);
}

// A stand-in for a line of text: a short pill centred on y.
void Bar(CDCHandle dc, int left, int right, int center_y, UINT dpi, COLORREF color) {
    const int height = std::max(2, Scale(3, dpi));
    const CRect rect(left, center_y - height / 2, std::max(left + height, right),
                     center_y - height / 2 + height);
    FillRound(dc, rect, height, color);
}

// The history popup in miniature: search field, four rows with the first
// selected and a search match in the third, the footer separator and two
// footer items. Every colour slot of the palette appears somewhere.
void PaintMiniature(CDCHandle dc, const CRect &card, const Theme::Palette &palette, UINT dpi) {
    const auto px = [dpi](int value) { return Scale(value, dpi); };
    const int line = std::max(1, px(1));

    FillRound(dc, card, px(10), palette.window);
    Ring(dc, card, px(10), line, palette.border);

    const int pad = px(6);
    const CRect search(card.left + pad, card.top + pad, card.right - pad, card.top + pad + px(12));
    FillRound(dc, search, search.Height(), palette.surface);
    const int icon = px(7);
    const CRect lens(CPoint(search.left + px(4), search.CenterPoint().y - icon / 2), CSize(icon, icon));
    {
        CPen pen;
        if (pen.CreatePen(PS_SOLID, line, palette.muted) != nullptr) {
            const HPEN previous_pen = dc.SelectPen(pen);
            const HBRUSH previous_brush = dc.SelectStockBrush(HOLLOW_BRUSH);
            dc.Ellipse(lens);
            dc.SelectBrush(previous_brush);
            dc.SelectPen(previous_pen);
        }
    }

    // The footer is anchored to the bottom and the rows share what is left,
    // so the miniature keeps its proportions at any card size.
    const int footer_line = px(8);
    const int footer_bottom = card.bottom - pad;
    const int separator_y = footer_bottom - 2 * footer_line - px(3);
    const int rows_top = search.bottom + px(4);
    const int row_height = std::max(1, (separator_y - px(3) - rows_top) / 4);

    const int row_left = card.left + px(4);
    const int row_right = card.right - px(4);
    const int text_left = row_left + px(5);
    const int shortcut_width = px(9);
    const int shortcut_left = row_right - px(5) - shortcut_width;
    const int text_space = std::max(1, shortcut_left - px(6) - text_left);
    constexpr std::array<int, 4> text_percent{58, 80, 70, 64};

    for (int row = 0; row < 4; ++row) {
        const CRect rect(row_left, rows_top + row * row_height,
                         row_right, rows_top + (row + 1) * row_height - line);
        const int center_y = rect.CenterPoint().y;
        const int text_right = text_left + text_space * text_percent[static_cast<size_t>(row)] / 100;
        COLORREF text = palette.text;
        COLORREF shortcut = palette.muted;
        if (row == 0) {
            FillRound(dc, rect, px(5), palette.accent);
            text = palette.accent_text;
            shortcut = palette.accent_text;
        }
        int bar_left = text_left;
        if (row == 2) {
            // The matched word, highlighted as an unselected row shows it.
            const CRect chip(text_left, center_y - px(4), text_left + px(14), center_y - px(4) + px(8));
            FillRound(dc, chip, px(3), palette.match);
            Bar(dc, chip.left + px(3), chip.right - px(3), center_y, dpi, palette.match_text);
            bar_left = chip.right + px(2);
        }
        Bar(dc, bar_left, text_right, center_y, dpi, text);
        Bar(dc, shortcut_left, shortcut_left + shortcut_width, center_y, dpi, shortcut);
    }

    dc.FillSolidRect(CRect(card.left + pad, separator_y, card.right - pad, separator_y + line),
                     palette.border);
    const int inner = card.Width() - 2 * pad;
    Bar(dc, text_left, text_left + inner * 30 / 100, footer_bottom - footer_line - footer_line / 2,
        dpi, palette.muted);
    Bar(dc, text_left, text_left + inner * 22 / 100, footer_bottom - footer_line / 2,
        dpi, palette.muted);
}

} // namespace

void ThemeGallery::Attach(HWND window) {
    if (IsWindow()) {
        UnsubclassWindow();
    }
    SubclassWindow(window);
}

void ThemeGallery::SetSelection(ThemeFamily family) {
    m_selected = std::clamp(static_cast<int>(family), 0, kThemeFamilyCount - 1);
    m_focused = m_selected;
    if (IsWindow()) {
        Invalidate(FALSE);
    }
}

int ThemeGallery::HitTest(POINT point) const {
    CRect client;
    GetClientRect(&client);
    for (int index = 0; index < kThemeFamilyCount; ++index) {
        if (CellRect(client, index).PtInRect(point)) {
            return index;
        }
    }
    return -1;
}

void ThemeGallery::Pick(int index) {
    if (index < 0 || index >= kThemeFamilyCount) {
        return;
    }
    m_focused = index;
    Invalidate(FALSE);
    if (index == m_selected) {
        return;
    }
    m_selected = index;
    GetParent().SendMessage(WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(), kSelectionChanged),
                            reinterpret_cast<LPARAM>(m_hWnd));
}

// OnPaint covers every pixel.
LRESULT ThemeGallery::OnEraseBackground(UINT, WPARAM, LPARAM, BOOL &) {
    return 1;
}

LRESULT ThemeGallery::OnPaint(UINT, WPARAM, LPARAM, BOOL &) {
    CPaintDC window_dc(m_hWnd);
    CMemoryDC memory(window_dc, window_dc.m_ps.rcPaint);
    CDCHandle dc(memory.m_hDC);
    CRect client;
    GetClientRect(&client);
    const UINT dpi = ::GetDpiForWindow(m_hWnd);
    const auto px = [dpi](int value) { return Scale(value, dpi); };
    const Theme::Palette &page = Theme::Current();

    // High contrast keeps the page on its native background; the cards still
    // show the themes' own colours, since they are previews.
    if (page.high_contrast) {
        ::DrawThemeParentBackground(m_hWnd, dc, &client);
    } else {
        dc.FillSolidRect(client, page.window);
    }

    // The control's own font: ScaleForDpi gives every control the font for
    // the new DPI, while the page keeps the one it was created with.
    HFONT font = GetFont();
    if (font == nullptr) {
        font = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    }
    LOGFONTW font_info{};
    CFontHandle(font).GetLogFont(&font_info);
    font_info.lfWeight = FW_SEMIBOLD;
    CFont semibold;
    semibold.CreateFontIndirect(&font_info);
    const HFONT previous_font = dc.SelectFont(font);
    TEXTMETRICW metrics{};
    dc.GetTextMetrics(&metrics);
    dc.SetBkMode(TRANSPARENT);
    dc.SetTextColor(page.text);

    const auto ui_state = static_cast<UINT>(SendMessage(WM_QUERYUISTATE));
    const bool show_focus = ::GetFocus() == m_hWnd && (ui_state & UISF_HIDEFOCUS) == 0;
    const int margin = px(6);
    const int ring_gap = px(3);
    const int name_gap = px(4);
    const int radius = px(10);

    for (int index = 0; index < kThemeFamilyCount; ++index) {
        const auto family = static_cast<ThemeFamily>(index);
        const CRect cell = CellRect(client, index);
        CRect card = cell;
        card.DeflateRect(margin, margin);
        card.bottom -= metrics.tmHeight + name_gap;
        if (card.Width() <= 0 || card.Height() <= 0) {
            continue;
        }
        PaintMiniature(dc, card, Theme::Preview(family, page.dark), dpi);

        CRect ring = card;
        ring.InflateRect(ring_gap, ring_gap);
        if (index == m_selected) {
            Ring(dc, ring, radius + 2 * ring_gap, std::max(2, px(2)), page.accent);
        } else if (index == m_hot) {
            Ring(dc, ring, radius + 2 * ring_gap, std::max(1, px(1)), page.muted);
        }

        const std::wstring name(Theme::FamilyName(family));
        CRect label(cell.left + px(2), card.bottom + name_gap, cell.right - px(2),
                    card.bottom + name_gap + metrics.tmHeight);
        dc.SelectFont(index == m_selected && semibold.m_hFont != nullptr ? semibold.m_hFont : font);
        dc.DrawText(name.c_str(), -1, &label,
                    DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

        if (index == m_focused && show_focus) {
            CRect focus = cell;
            focus.DeflateRect(std::max(1, px(1)), std::max(1, px(1)));
            dc.DrawFocusRect(&focus);
        }
    }
    dc.SelectFont(previous_font);
    return 0;
}

LRESULT ThemeGallery::OnSize(UINT, WPARAM, LPARAM, BOOL &) {
    Invalidate(FALSE);
    return 0;
}

// Arrows move between cards and Enter picks one; Tab and Esc stay with the
// dialog manager, so the gallery is one tab stop on the page.
LRESULT ThemeGallery::OnGetDlgCode(UINT, WPARAM, LPARAM lParam, BOOL &) {
    const auto *message = reinterpret_cast<const MSG *>(lParam);
    const bool enter = message != nullptr && message->message == WM_KEYDOWN &&
        message->wParam == VK_RETURN;
    return DLGC_WANTARROWS | (enter ? DLGC_WANTMESSAGE : 0);
}

LRESULT ThemeGallery::OnKeyDown(UINT, WPARAM key, LPARAM, BOOL &handled) {
    const int last = kThemeFamilyCount - 1;
    int next = m_focused;
    switch (key) {
    case VK_LEFT:
        next = std::max(0, m_focused - 1);
        break;
    case VK_RIGHT:
        next = std::min(last, m_focused + 1);
        break;
    case VK_UP:
        next = m_focused >= kColumns ? m_focused - kColumns : m_focused;
        break;
    case VK_DOWN:
        // From a column the short last row lacks, Down lands on its last card.
        next = m_focused / kColumns < last / kColumns ? std::min(last, m_focused + kColumns) : m_focused;
        break;
    case VK_HOME:
        next = 0;
        break;
    case VK_END:
        next = last;
        break;
    case VK_SPACE:
    case VK_RETURN:
        Pick(m_focused);
        break;
    default:
        handled = FALSE;
        return 0;
    }
    // Keyboard use shows focus cues, as the dialog manager does for Tab.
    SendMessage(WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS));
    if (next != m_focused) {
        m_focused = next;
        Invalidate(FALSE);
    }
    return 0;
}

// Handled here, not by the static, so it never sends STN_* notifications of
// its own.
LRESULT ThemeGallery::OnButtonDown(UINT, WPARAM, LPARAM lParam, BOOL &) {
    SetFocus();
    Pick(HitTest(CPoint(lParam)));
    return 0;
}

LRESULT ThemeGallery::OnButtonUp(UINT, WPARAM, LPARAM, BOOL &) {
    return 0;
}

LRESULT ThemeGallery::OnMouseMove(UINT, WPARAM, LPARAM lParam, BOOL &) {
    if (!m_trackingMouse) {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, m_hWnd, 0};
        m_trackingMouse = ::TrackMouseEvent(&track) != FALSE;
    }
    const int hot = HitTest(CPoint(lParam));
    if (hot != m_hot) {
        m_hot = hot;
        Invalidate(FALSE);
    }
    return 0;
}

LRESULT ThemeGallery::OnMouseLeave(UINT, WPARAM, LPARAM, BOOL &) {
    m_trackingMouse = false;
    if (m_hot != -1) {
        m_hot = -1;
        Invalidate(FALSE);
    }
    return 0;
}

// Tabbing in starts on the saved theme, like a group of radio buttons.
LRESULT ThemeGallery::OnFocusChanged(UINT message, WPARAM, LPARAM, BOOL &handled) {
    if (message == WM_SETFOCUS) {
        m_focused = m_selected;
    }
    Invalidate(FALSE);
    handled = FALSE;
    return 0;
}

LRESULT ThemeGallery::OnUpdateUiState(UINT message, WPARAM wParam, LPARAM lParam, BOOL &) {
    const LRESULT result = DefWindowProc(message, wParam, lParam);
    Invalidate(FALSE);
    return result;
}
