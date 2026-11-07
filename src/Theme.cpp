#include "Theme.h"

#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Theme {
namespace {

constexpr COLORREF Hex(std::uint32_t rgb) {
    return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

struct Variant {
    std::uint32_t window;
    std::uint32_t surface;
    std::uint32_t text;
    std::uint32_t muted;
    std::uint32_t accent;
    std::uint32_t accent_text;
    std::uint32_t border;
    std::uint32_t match;
    std::uint32_t match_text;
};

struct NamedTheme {
    const wchar_t *name;
    Variant light;
    Variant dark;
};

// Official palettes, slot by slot: window, surface, text, muted, accent,
// accent text, border, match, match text. Every accent text and match text
// pair reaches 4.5:1; where neither of the theme's own background or
// foreground does, black or white is used. Muted uses the theme's comment or
// "subtle" colour, or its official higher-contrast alternative when the
// comment colour is too faint for shortcut text. Windows (index 0) is computed
// from the system accent instead, so its table entry is never read.
constexpr std::array<NamedTheme, kThemeFamilyCount> kThemes{{
    {L"Windows", {}, {}},
    // altercation/solarized README + vim-colors-solarized (VertSplit, Search).
    {L"Solarized",
     {0xFDF6E3, 0xEEE8D5, 0x657B83, 0x93A1A1, 0x268BD2, 0x000000, 0x839496, 0xB58900, 0x002B36},
     {0x002B36, 0x073642, 0x839496, 0x586E75, 0x268BD2, 0x000000, 0x657B83, 0xB58900, 0x002B36}},
    // monokai.pro by Monokai's author: the Pro Light and Pro filters; muted is
    // dimmed1/dimmed2 (dimmed3, the comment colour, is under 3:1).
    {L"Monokai Pro",
     {0xFAF4F2, 0xFEFAF9, 0x29242A, 0x706B6E, 0x7058BE, 0xFAF4F2, 0xEDE7E5, 0xCC7A0A, 0x29242A},
     {0x2D2A2E, 0x403E41, 0xFCFCFA, 0x939293, 0xAB9DF2, 0x2D2A2E, 0x19181A, 0xFFD866, 0x2D2A2E}},
    // draculatheme.com/spec: Alucard Classic and Dracula Classic.
    {L"Dracula",
     {0xFFFBEB, 0xEFEDDC, 0x1F1F1F, 0x6C664B, 0x644AC9, 0xFFFBEB, 0x6C664B, 0x846E15, 0xFFFBEB},
     {0x282A36, 0x343746, 0xF8F8F2, 0x6272A4, 0xBD93F9, 0x282A36, 0x6272A4, 0xF1FA8C, 0x282A36}},
    // nordtheme/nord nord.css roles: Snow Storm for bright ambiance, Polar
    // Night for dark; muted #616E88 is the official VS Code port's comment.
    {L"Nord",
     {0xECEFF4, 0xE5E9F0, 0x2E3440, 0x4C566A, 0x88C0D0, 0x2E3440, 0xD8DEE9, 0xEBCB8B, 0x2E3440},
     {0x2E3440, 0x3B4252, 0xD8DEE9, 0x616E88, 0x88C0D0, 0x2E3440, 0x3B4252, 0xEBCB8B, 0x2E3440}},
    // morhetz/gruvbox colors/gruvbox.vim, medium contrast (Pmenu, VertSplit, Search).
    {L"Gruvbox",
     {0xFBF1C7, 0xEBDBB2, 0x3C3836, 0x928374, 0x076678, 0xFBF1C7, 0xBDAE93, 0xB57614, 0x000000},
     {0x282828, 0x3C3836, 0xEBDBB2, 0x928374, 0x83A598, 0x282828, 0x665C54, 0xFABD2F, 0x282828}},
    // catppuccin/palette v1.8.0: Latte and Mocha, Mauve accent; Latte muted
    // is Subtext 0.
    {L"Catppuccin",
     {0xEFF1F5, 0xCCD0DA, 0x4C4F69, 0x6C6F85, 0x8839EF, 0xEFF1F5, 0x9CA0B0, 0xDF8E1D, 0x000000},
     {0x1E1E2E, 0x313244, 0xCDD6F4, 0x9399B2, 0xCBA6F7, 0x1E1E2E, 0x6C7086, 0xF9E2AF, 0x1E1E2E}},
    // folke/tokyonight.nvim extras: Day and Night; Night muted is dark5 and
    // its border fg_gutter (the official border is under 1.2:1).
    {L"Tokyo Night",
     {0xE1E2E7, 0xC4C8DA, 0x3760BF, 0x848CB5, 0x2E7DE9, 0x000000, 0xB4B5B9, 0x8C6C3E, 0xFFFFFF},
     {0x1A1B26, 0x292E42, 0xC0CAF5, 0x737AA2, 0x7AA2F7, 0x1A1B26, 0x3B4261, 0xE0AF68, 0x1A1B26}},
    // atom/atom one-light/one-dark syntax and UI variables; muted is mono-2.
    {L"One",
     {0xFAFAFA, 0xE5E5E6, 0x383A42, 0x696C77, 0x5871EF, 0x000000, 0xDBDBDC, 0xCB7701, 0x000000},
     {0x282C34, 0x31363F, 0xABB2BF, 0x828997, 0x4D78CC, 0x000000, 0x181A1F, 0xE5C07B, 0x282C34}},
    // primer/github-vscode-theme v6.3.5 on Primer primitives 7.10.0.
    {L"GitHub",
     {0xFFFFFF, 0xF6F8FA, 0x1F2328, 0x656D76, 0x0969DA, 0xFFFFFF, 0xD0D7DE, 0xFAE17D, 0x1F2328},
     {0x0D1117, 0x161B22, 0xE6EDF3, 0x7D8590, 0x1F6FEB, 0xFFFFFF, 0x30363D, 0xF2CC60, 0x0D1117}},
    // rose-pine palette.json: Dawn and main (Subtle for secondary text,
    // Highlight High borders, Gold search highlight).
    {L"Rosé Pine",
     {0xFAF4ED, 0xFFFAF3, 0x464261, 0x797593, 0x907AA9, 0x000000, 0xCECACD, 0xEA9D34, 0x000000},
     {0x191724, 0x1F1D2E, 0xE0DEF4, 0x908CAA, 0xC4A7E7, 0x191724, 0x524F67, 0xF6C177, 0x191724}},
}};

Palette g_palette{};
bool g_resolved = false;
HBRUSH g_windowBrush = nullptr;
HBRUSH g_surfaceBrush = nullptr;

double Channel(BYTE value) {
    const double c = value / 255.0;
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double Luminance(COLORREF color) {
    return 0.2126 * Channel(GetRValue(color)) + 0.7152 * Channel(GetGValue(color)) +
        0.0722 * Channel(GetBValue(color));
}

double Contrast(COLORREF a, COLORREF b) {
    const double la = Luminance(a);
    const double lb = Luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

COLORREF Mix(COLORREF foreground, COLORREF background, int foreground_percent) {
    const auto mix = [foreground_percent](BYTE f, BYTE b) {
        return static_cast<BYTE>((f * foreground_percent + b * (100 - foreground_percent)) / 100);
    };
    return RGB(mix(GetRValue(foreground), GetRValue(background)),
               mix(GetGValue(foreground), GetGValue(background)),
               mix(GetBValue(foreground), GetBValue(background)));
}

bool HighContrastOn() {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    return ::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

bool SystemUsesDarkApps() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    return ::RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
        value == 0;
}

// The accent the Settings app shows; DWM stores it as 0xAABBGGRR, which is a
// COLORREF once alpha is dropped.
COLORREF SystemAccent() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor",
                       RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        return static_cast<COLORREF>(value & 0x00FFFFFF);
    }
    return ::GetSysColor(COLOR_HIGHLIGHT);
}

// Neutral Windows 11 greys around the user's accent colour. White on the
// accent reads like the system's own selected items; black only when the
// accent is too light for it.
Palette WindowsPalette(bool dark) {
    Palette palette;
    palette.dark = dark;
    if (dark) {
        palette.window = Hex(0x202020);
        palette.surface = Hex(0x2D2D2D);
        palette.text = Hex(0xFFFFFF);
        palette.muted = Hex(0xA3A3A3);
        palette.border = Hex(0x3B3B3B);
    } else {
        palette.window = Hex(0xFBFBFB);
        palette.surface = Hex(0xEDEDED);
        palette.text = Hex(0x1B1B1B);
        palette.muted = Hex(0x6B6B6B);
        palette.border = Hex(0xE0E0E0);
    }
    palette.accent = SystemAccent();
    palette.accent_text = Contrast(Hex(0xFFFFFF), palette.accent) >= 3.0 ? Hex(0xFFFFFF) : Hex(0x000000);
    palette.match = Mix(palette.accent, palette.window, dark ? 40 : 25);
    palette.match_text = palette.text;
    return palette;
}

Palette HighContrastPalette() {
    Palette palette;
    palette.high_contrast = true;
    palette.window = ::GetSysColor(COLOR_WINDOW);
    palette.surface = ::GetSysColor(COLOR_BTNFACE);
    palette.text = ::GetSysColor(COLOR_WINDOWTEXT);
    palette.muted = ::GetSysColor(COLOR_GRAYTEXT);
    palette.accent = ::GetSysColor(COLOR_HIGHLIGHT);
    palette.accent_text = ::GetSysColor(COLOR_HIGHLIGHTTEXT);
    palette.border = ::GetSysColor(COLOR_WINDOWTEXT);
    palette.match = ::GetSysColor(COLOR_HIGHLIGHT);
    palette.match_text = ::GetSysColor(COLOR_HIGHLIGHTTEXT);
    palette.dark = Luminance(palette.window) < 0.5;
    return palette;
}

Palette FromVariant(const Variant &variant, bool dark) {
    Palette palette;
    palette.dark = dark;
    palette.window = Hex(variant.window);
    palette.surface = Hex(variant.surface);
    palette.text = Hex(variant.text);
    palette.muted = Hex(variant.muted);
    palette.accent = Hex(variant.accent);
    palette.accent_text = Hex(variant.accent_text);
    palette.border = Hex(variant.border);
    palette.match = Hex(variant.match);
    palette.match_text = Hex(variant.match_text);
    return palette;
}

bool SamePalette(const Palette &a, const Palette &b) {
    return a.dark == b.dark && a.high_contrast == b.high_contrast && a.window == b.window &&
        a.surface == b.surface && a.text == b.text && a.muted == b.muted &&
        a.accent == b.accent && a.accent_text == b.accent_text && a.border == b.border &&
        a.match == b.match && a.match_text == b.match_text;
}

// uxtheme's dark-mode switches for menus and classic controls have no public
// names; they are stable by ordinal since Windows 10 1903 (build 18362) and
// are what Explorer and Notepad++ use.
enum class PreferredAppMode { Default = 0, AllowDark = 1, ForceDark = 2, ForceLight = 3 };
using SetPreferredAppModeFn = PreferredAppMode(WINAPI *)(PreferredAppMode);
using FlushMenuThemesFn = void(WINAPI *)();
using AllowDarkModeForWindowFn = bool(WINAPI *)(HWND, bool);

struct DarkModeApi {
    SetPreferredAppModeFn set_preferred_app_mode = nullptr;
    FlushMenuThemesFn flush_menu_themes = nullptr;
    AllowDarkModeForWindowFn allow_dark_mode_for_window = nullptr;
};

const DarkModeApi &UxTheme() {
    static const DarkModeApi api = [] {
        DarkModeApi result;
        using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        const auto rtl_get_version = ntdll != nullptr
            ? reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion"))
            : nullptr;
        OSVERSIONINFOW version{sizeof(version)};
        if (rtl_get_version == nullptr || rtl_get_version(&version) != 0 || version.dwBuildNumber < 18362) {
            return result;
        }
        const HMODULE uxtheme = ::LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (uxtheme == nullptr) {
            return result;
        }
        result.allow_dark_mode_for_window = reinterpret_cast<AllowDarkModeForWindowFn>(
            ::GetProcAddress(uxtheme, MAKEINTRESOURCEA(133)));
        result.set_preferred_app_mode = reinterpret_cast<SetPreferredAppModeFn>(
            ::GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
        result.flush_menu_themes = reinterpret_cast<FlushMenuThemesFn>(
            ::GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
        return result;
    }();
    return api;
}

void ApplyToMenus(const Palette &palette) {
    const DarkModeApi &api = UxTheme();
    if (api.set_preferred_app_mode == nullptr) {
        return;
    }
    api.set_preferred_app_mode(palette.high_contrast
        ? PreferredAppMode::Default
        : (palette.dark ? PreferredAppMode::ForceDark : PreferredAppMode::ForceLight));
    if (api.flush_menu_themes != nullptr) {
        api.flush_menu_themes();
    }
}

BOOL CALLBACK ApplyToChild(HWND child, LPARAM) {
    ApplyToControl(child);
    return TRUE;
}

} // namespace

bool Update(ThemeFamily family, ThemeMode mode) {
    const int index = static_cast<int>(family);
    const bool dark = mode == ThemeMode::Dark || (mode == ThemeMode::System && SystemUsesDarkApps());
    Palette next;
    if (HighContrastOn()) {
        next = HighContrastPalette();
    } else if (index <= 0 || index >= kThemeFamilyCount) {
        next = WindowsPalette(dark);
    } else {
        next = FromVariant(dark ? kThemes[index].dark : kThemes[index].light, dark);
    }
    if (g_resolved && SamePalette(next, g_palette)) {
        return false;
    }
    g_palette = next;
    g_resolved = true;
    if (g_windowBrush != nullptr) ::DeleteObject(g_windowBrush);
    if (g_surfaceBrush != nullptr) ::DeleteObject(g_surfaceBrush);
    g_windowBrush = ::CreateSolidBrush(g_palette.window);
    g_surfaceBrush = ::CreateSolidBrush(g_palette.surface);
    ApplyToMenus(g_palette);
    return true;
}

const Palette &Current() noexcept {
    return g_palette;
}

HBRUSH WindowBrush() noexcept {
    return g_windowBrush != nullptr ? g_windowBrush : ::GetSysColorBrush(COLOR_WINDOW);
}

HBRUSH SurfaceBrush() noexcept {
    return g_surfaceBrush != nullptr ? g_surfaceBrush : ::GetSysColorBrush(COLOR_BTNFACE);
}

std::wstring_view FamilyName(ThemeFamily family) noexcept {
    const int index = static_cast<int>(family);
    return index >= 0 && index < kThemeFamilyCount ? kThemes[index].name : kThemes[0].name;
}

void ApplyToWindow(HWND window) noexcept {
    if (window == nullptr) {
        return;
    }
    const BOOL dark = g_palette.dark ? TRUE : FALSE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE: dark title bar and frame on Windows 10 20H1+.
    ::DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    // DWMWA_BORDER_COLOR (Windows 11): the theme's border, or the system's in high contrast.
    const COLORREF border = g_palette.high_contrast ? 0xFFFFFFFF : g_palette.border;
    ::DwmSetWindowAttribute(window, 34, &border, sizeof(border));
    if (const DarkModeApi &api = UxTheme(); api.allow_dark_mode_for_window != nullptr) {
        api.allow_dark_mode_for_window(window, g_palette.dark && !g_palette.high_contrast);
    }
    ::EnumChildWindows(window, ApplyToChild, 0);
    ::RedrawWindow(window, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

void ApplyToControl(HWND control) noexcept {
    wchar_t class_name[64]{};
    if (control == nullptr || ::GetClassNameW(control, class_name, 64) == 0) {
        return;
    }
    const bool dark = g_palette.dark && !g_palette.high_contrast;
    const std::wstring_view name(class_name);
    const wchar_t *theme = nullptr;
    if (name == L"Edit" || name == L"ComboBox" || name == L"ComboLBox") {
        theme = dark ? L"DarkMode_CFD" : nullptr;
    } else if (name == L"SysHeader32") {
        theme = dark ? L"DarkMode_ItemsView" : L"ItemsView";
    } else if (name == L"SysListView32" || name == L"SysTreeView32") {
        theme = dark ? L"DarkMode_Explorer" : L"Explorer";
    } else if (name == L"Button" || name == L"ListBox" || name == L"ScrollBar" ||
               name == L"tooltips_class32" || name == L"SysTabControl32" ||
               name == L"msctls_updown32" || name == L"msctls_trackbar32") {
        theme = dark ? L"DarkMode_Explorer" : nullptr;
    } else {
        return;
    }
    if (const DarkModeApi &api = UxTheme(); api.allow_dark_mode_for_window != nullptr) {
        api.allow_dark_mode_for_window(control, dark);
    }
    ::SetWindowTheme(control, theme, nullptr);
}

bool IsThemeChangeMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept {
    if (message == WM_SYSCOLORCHANGE || message == WM_DWMCOLORIZATIONCOLORCHANGED ||
        message == WM_THEMECHANGED) {
        return true;
    }
    if (message != WM_SETTINGCHANGE) {
        return false;
    }
    return wParam == SPI_SETHIGHCONTRAST ||
        (lParam != 0 && ::lstrcmpiW(reinterpret_cast<const wchar_t *>(lParam), L"ImmersiveColorSet") == 0);
}

} // namespace Theme
