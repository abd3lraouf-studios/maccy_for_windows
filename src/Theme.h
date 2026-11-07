#pragma once

#include "PlatformConfig.h"

#include "Settings.h"

#include <string_view>

// One resolved palette for everything Maccy paints. Each named theme ships its
// official light and dark variant; "Windows" uses the system accent colour.
// High contrast always wins and maps to the system colours.
//
// UI thread only: the palette and brushes are shared by the popup, preview,
// settings and tray menu, which all run on the UI thread.
namespace Theme {

struct Palette {
    bool dark = false;
    bool high_contrast = false;
    COLORREF window = 0;      // popup and dialog background
    COLORREF surface = 0;     // search field, text boxes, button faces
    COLORREF text = 0;        // primary text
    COLORREF muted = 0;       // shortcuts, placeholders, metadata
    COLORREF accent = 0;      // selected row
    COLORREF accent_text = 0; // text on the selected row
    COLORREF border = 0;      // separators and window border
    COLORREF match = 0;       // search match background in an unselected row
    COLORREF match_text = 0;  // text on a search match
};

// Resolves the palette for the theme settings and the current Windows state.
// Call at startup, after the theme settings change, and when
// IsThemeChangeMessage reports a system change. Returns true when the palette
// changed, so callers repaint and re-apply window themes only then.
bool Update(ThemeFamily family, ThemeMode mode);
const Palette &Current() noexcept;

// Owned here and rebuilt by Update: return them from WM_CTLCOLOR* freely.
HBRUSH WindowBrush() noexcept;
HBRUSH SurfaceBrush() noexcept;

// Proper names, not translated.
std::wstring_view FamilyName(ThemeFamily family) noexcept;

// Dark or light title bar and border for a top-level window, plus the matching
// visual style for every child control (scrollbars, buttons, edits, combos,
// list views, headers, tabs, tooltips). Call after creating the window and
// after Update returns true.
void ApplyToWindow(HWND window) noexcept;
void ApplyToControl(HWND control) noexcept;

// WM_SETTINGCHANGE (light/dark switch, high contrast), WM_SYSCOLORCHANGE,
// WM_DWMCOLORIZATIONCOLORCHANGED (accent colour) and WM_THEMECHANGED.
bool IsThemeChangeMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept;

} // namespace Theme
