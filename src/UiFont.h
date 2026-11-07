#pragma once

#include "PlatformConfig.h"

#include <atlbase.h>
#include <atlapp.h>
#include <atlgdi.h>

namespace UiFont {

// Sizes at the default Windows text size, where the message font is 9 pt.
// Other sizes keep their ratio to it, so the "Text size" setting scales them too.
constexpr int kBodyPointSize = 9;
constexpr int kSmallPointSize = 7;

inline UINT DpiForWindow(HWND window) {
    if (window != nullptr) {
        const UINT dpi = ::GetDpiForWindow(window);
        if (dpi > 0) {
            return dpi;
        }
    }
    return USER_DEFAULT_SCREEN_DPI;
}

// A variant of the system message font at `dpi`. The message font carries the
// user's UI typeface and text scaling; at default settings it is Segoe UI 9 pt,
// so the look is unchanged there.
inline HFONT CreateUiFont(UINT dpi, int pointSize, LONG weight = FW_NORMAL,
                          BOOL italic = FALSE, BOOL underline = FALSE) {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    LOGFONTW font{};
    if (::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                     &metrics, 0, dpi) != FALSE) {
        font = metrics.lfMessageFont;
        font.lfHeight = ::MulDiv(font.lfHeight, pointSize, kBodyPointSize);
    } else {
        font.lfHeight = -::MulDiv(pointSize, static_cast<int>(dpi), 72);
        font.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(font.lfFaceName, L"Segoe UI");
    }
    font.lfWeight = weight;
    font.lfItalic = static_cast<BYTE>(italic);
    font.lfUnderline = static_cast<BYTE>(underline);
    font.lfQuality = CLEARTYPE_QUALITY;
    return ::CreateFontIndirectW(&font);
}

} // namespace UiFont
