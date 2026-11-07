#include "SettingsWindow.h"
#include "AutoSizeText.h"
#include "Constants.h"
#include "ClipboardRules.h"
#include "KeyboardHandler.h"
#include "Localization.h"
#include "Theme.h"

#include <windows.h>
#include <atltypes.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <vssym32.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

#include "PinKeys.h"
#include "TrayIcon.h"

// Defined in main.cpp; owns the thread's CMessageLoop.
extern CAppModule _Module;

namespace {

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

void SetLocalizedText(CWindow window, int control_id, UINT resource_id) {
    if (window.m_hWnd == nullptr) {
        return;
    }
    CWindow control = window.GetDlgItem(control_id);
    if (control.m_hWnd == nullptr) {
        return;
    }
    const std::wstring text = Localization::Text(resource_id);
    // Push buttons keep their layout width as a minimum: the page grid gives
    // them standard sizes, and shrinking to the caption makes them cramped.
    // Labels and check boxes still fit their text.
    int minimum_width = 0;
    const LONG type = control.GetStyle() & BS_TYPEMASK;
    wchar_t class_name[16]{};
    ::GetClassNameW(control, class_name, 16);
    if (::lstrcmpiW(class_name, L"Button") == 0 && (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON)) {
        RECT rect{};
        control.GetWindowRect(&rect);
        minimum_width = ::MulDiv(rect.right - rect.left, USER_DEFAULT_SCREEN_DPI,
                                 static_cast<int>(::GetDpiForWindow(control)));
    }
    AutoSizeTextControl(control).SetText(text, minimum_width);
}

void ShowSettingsRuntimeError(
    HWND owner,
    std::string_view error,
    UINT fallback_message_resource,
    UINT title_resource = IDS_SETTINGS_ERROR_TITLE
) {
    const std::wstring message = error.empty()
        ? Localization::Text(fallback_message_resource)
        : Localization::FromUtf8(error);
    const std::wstring title = Localization::Text(title_resource);
    ::MessageBoxW(owner, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
}

// Field labels end in a colon ("Pin:"); messages and titles name the field.
std::wstring LabelName(UINT resource_id) {
    std::wstring text = Localization::Text(resource_id);
    while (!text.empty() && (text.back() == L':' || text.back() == L' ')) {
        text.pop_back();
    }
    return text;
}

std::wstring FindHotkeyConflict(
    const HotKeyConfig &open,
    const HotKeyConfig &pin,
    const HotKeyConfig &remove,
    const HotKeyConfig &preview
) {
    struct Binding {
        UINT name_resource;
        const HotKeyConfig *hotkey;
    };
    const std::array<Binding, 4> bindings = {{
        {IDS_G_OPEN, &open},
        {IDS_G_PIN, &pin},
        {IDS_G_DELETE, &remove},
        {IDS_G_PREVIEW, &preview},
    }};

    for (size_t first = 0; first < bindings.size(); ++first) {
        if (bindings[first].hotkey->virtual_key == 0) {
            continue;
        }
        for (size_t second = first + 1; second < bindings.size(); ++second) {
            if (!SameHotKey(*bindings[first].hotkey, *bindings[second].hotkey)) {
                continue;
            }

            const std::wstring first_name = LabelName(bindings[first].name_resource);
            const std::wstring second_name = LabelName(bindings[second].name_resource);
            const std::wstring shortcut = HotKeyToText(*bindings[first].hotkey);
            return Localization::Format(
                IDS_SETTINGS_SHORTCUT_CONFLICT,
                {first_name, second_name, shortcut}
            );
        }
    }
    return {};
}

} // namespace

std::wstring PinTextContent(const ClipboardItem &item) {
    for (const ClipboardFormatData &data : item.data) {
        if (data.format == CF_UNICODETEXT || data.name == L"CF_UNICODETEXT") {
            if (data.bytes.size() < sizeof(wchar_t)) {
                return {};
            }
            const auto *text = reinterpret_cast<const wchar_t *>(data.bytes.data());
            size_t length = 0;
            const size_t capacity = data.bytes.size() / sizeof(wchar_t);
            while (length < capacity && text[length] != L'\0') {
                ++length;
            }
            return std::wstring(text, length);
        }
    }
    return {};
}

// EditPinDialog
EditPinDialog::EditPinDialog(const AppSettings &settings, sqlite3_int64 item_id,
                             const std::vector<ClipboardItem> &pins, ClipboardItem item)
    : m_settings(settings), m_itemId(item_id), m_pins(pins), m_item(std::move(item)) {
}

LRESULT EditPinDialog::OnInitDialog(UINT, WPARAM, LPARAM, BOOL &handled) {
    handled = TRUE;
    CenterWindow(GetParent());

    SetWindowText(Localization::Text(IDS_EDIT_PIN_TITLE).c_str());
    ::SetDlgItemTextW(m_hWnd, IDC_EDIT_PIN_KEY_LABEL, Localization::Text(IDS_EDIT_PIN_KEY).c_str());
    ::SetDlgItemTextW(m_hWnd, IDC_EDIT_PIN_TITLE_LABEL, Localization::Text(IDS_EDIT_PIN_ALIAS).c_str());
    ::SetDlgItemTextW(m_hWnd, IDC_EDIT_PIN_CONTENT_LABEL, Localization::Text(IDS_EDIT_PIN_CONTENT).c_str());
    ::SetDlgItemTextW(m_hWnd, IDOK, Localization::Text(IDS_EDIT_PIN_OK).c_str());
    ::SetDlgItemTextW(m_hWnd, IDCANCEL, Localization::Text(IDS_EDIT_PIN_CANCEL).c_str());

    m_keyCombo = GetDlgItem(IDC_EDIT_PIN_KEY);
    m_titleEdit = GetDlgItem(IDC_EDIT_PIN_TITLE);
    m_contentEdit = GetDlgItem(IDC_EDIT_PIN_CONTENT);
    m_hintLabel = GetDlgItem(IDC_P_CONTENT_HINT);

    ClipboardItem &item = m_item;
    m_key = item.pin;
    m_title = item.title;
    m_originalContent = PinTextContent(item);
    if (m_originalContent.empty()) {
        m_originalContent = item.preview;
    }

    // Offer only the keys the pin policy allows for this item.
    for (const wchar_t ch : PinKeyPolicy::Available(m_pins, m_settings, m_itemId)) {
        std::wstring key(1, ch);
        m_keyCombo.AddString(key.c_str());
    }
    if (m_key.size() == 1 &&
        m_keyCombo.FindStringExact(-1, m_key.c_str()) == CB_ERR) {
        m_keyCombo.AddString(m_key.c_str());
    }

    // Select the current key.
    int keyIndex = m_keyCombo.FindStringExact(-1, m_key.c_str());
    if (keyIndex != CB_ERR) {
        m_keyCombo.SetCurSel(keyIndex);
    }

    // Fill in the title and content.
    m_titleEdit.SetWindowText(m_title.c_str());

    // Only plain-text items can have their content edited.
    m_textEditable = item.has_text && !item.has_image && !item.has_files;
    if (m_textEditable) {
        m_contentEdit.SetWindowText(m_originalContent.c_str());
        m_contentEdit.EnableWindow(TRUE);
        m_hintLabel.SetWindowText(L"");
    } else {
        m_contentEdit.SetWindowText(m_originalContent.c_str());
        m_contentEdit.EnableWindow(FALSE);
        m_hintLabel.SetWindowText(Localization::Text(IDS_UNEDITABLE_CONTENT).c_str());
    }

    Theme::ApplyToWindow(m_hWnd);
    return TRUE;
}

LRESULT EditPinDialog::OnOK(WORD, WORD, HWND, BOOL &handled) {
    handled = TRUE;

    // Read the key.
    int keyIndex = m_keyCombo.GetCurSel();
    if (keyIndex != CB_ERR) {
        wchar_t keyBuffer[256] = {};
        m_keyCombo.GetLBText(keyIndex, keyBuffer);
        m_key = keyBuffer;
    }

    // Validate the key.
    std::wstring keyLower = m_key;
    std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(), std::towlower);
    if (!PinKeyPolicy::IsValid(keyLower, m_pins, m_settings, m_itemId)) {
        const std::wstring message = Localization::Text(IDS_SETTINGS_INVALID_PIN_KEY);
        const std::wstring title = LabelName(IDS_G_PIN);
        MessageBoxW(message.c_str(), title.c_str(), MB_OK | MB_ICONWARNING);
        return 0;
    }
    m_key = keyLower;

    // Read the title.
    m_title = ReadWindowText(m_titleEdit);

    // Read the content (when editable).
    if (m_textEditable) {
        m_content = ReadWindowText(m_contentEdit);

        if (m_content != m_originalContent) {
            const std::wstring message = Localization::Text(IDS_SETTINGS_EDIT_CONTENT_WARNING);
            const std::wstring title = Localization::Text(IDS_EDIT_PIN_TITLE);
            if (MessageBoxW(message.c_str(), title.c_str(),
                MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES) {
                return 0;
            }
            m_contentModified = true;
        }
    }

    EndDialog(IDOK);
    return 0;
}

LRESULT EditPinDialog::OnCancel(WORD, WORD, HWND, BOOL &handled) {
    handled = TRUE;
    EndDialog(IDCANCEL);
    return 0;
}

// EditIgnoreDialog
EditIgnoreDialog::EditIgnoreDialog(const std::wstring &value, const std::wstring &description, int ignore_page)
    : m_value(value), m_description(description), m_ignorePage(ignore_page) {
}

LRESULT EditIgnoreDialog::OnInitDialog(UINT, WPARAM, LPARAM, BOOL &handled) {
    handled = TRUE;
    CenterWindow(GetParent());

    SetWindowText(Localization::Text(IDS_EDIT_IGNORE_TITLE).c_str());
    ::SetDlgItemTextW(m_hWnd, IDC_EDIT_IGNORE_VALUE_LABEL, Localization::Text(IDS_EDIT_IGNORE_VALUE).c_str());
    ::SetDlgItemTextW(m_hWnd, IDOK, Localization::Text(IDS_EDIT_IGNORE_OK).c_str());
    ::SetDlgItemTextW(m_hWnd, IDCANCEL, Localization::Text(IDS_EDIT_IGNORE_CANCEL).c_str());

    m_valueEdit = GetDlgItem(IDC_EDIT_IGNORE_VALUE);
    m_descriptionLabel = GetDlgItem(IDC_I_DESCRIPTION);

    m_valueEdit.SetWindowText(m_value.c_str());
    m_descriptionLabel.SetWindowText(m_description.c_str());

    // A new entry starts from an example value.
    if (m_value.empty()) {
        if (m_ignorePage == 1) {
            m_valueEdit.SetWindowText(L"xxx.yyy.zzz");
        } else if (m_ignorePage == 2) {
            m_valueEdit.SetWindowText(L"^[a-zA-Z0-9]{50}$");
        }
    }

    Theme::ApplyToWindow(m_hWnd);
    m_valueEdit.SetSel(0, -1);
    m_valueEdit.SetFocus();

    return FALSE; // FALSE: focus has already been set.
}

LRESULT EditIgnoreDialog::OnOK(WORD, WORD, HWND, BOOL &handled) {
    handled = TRUE;

    m_value = ReadWindowText(m_valueEdit);

    if (m_value.empty()) {
        const std::wstring message = Localization::Text(IDS_SETTINGS_VALUE_EMPTY);
        const std::wstring title = Localization::Text(IDS_SETTINGS_ERROR_TITLE);
        MessageBoxW(message.c_str(), title.c_str(), MB_OK | MB_ICONWARNING);
        return 0;
    }

    EndDialog(IDOK);
    return 0;
}

LRESULT EditIgnoreDialog::OnCancel(WORD, WORD, HWND, BOOL &handled) {
    handled = TRUE;
    EndDialog(IDCANCEL);
    return 0;
}

namespace {

enum SettingsControlId : int {
    kTabs = IDC_SETTINGS_TABS,

    kGLaunch = IDC_G_LAUNCH,
    kGUpdates = IDC_G_UPDATES,
    kGCheckNow = IDC_G_CHECK_NOW,
    kGOpenHotKey = IDC_G_OPEN_HOTKEY,
    kGPinHotKey = IDC_G_PIN_HOTKEY,
    kGDeleteHotKey = IDC_G_DELETE_HOTKEY,
    kGPreviewHotKey = IDC_G_PREVIEW_HOTKEY,
    kGSearchMode = IDC_G_SEARCH_MODE,
    kGPasteByDefault = IDC_G_PASTE_BY_DEFAULT,
    kGRemoveFormatting = IDC_G_REMOVE_FORMATTING,
    kGNotifications = IDC_G_NOTIFICATIONS,

    kAPopupPosition = IDC_A_POPUP_POSITION,
    kAPopupScreen = IDC_A_POPUP_SCREEN,
    kAResetPosition = IDC_A_RESET_POSITION,
    kAPinTo = IDC_A_PIN_TO,
    kAImageHeight = IDC_A_IMAGE_HEIGHT,
    kAOpenPreview = IDC_A_OPEN_PREVIEW,
    kAPreviewDelay = IDC_A_PREVIEW_DELAY,
    kAHighlight = IDC_A_HIGHLIGHT,
    kAMenuIcon = IDC_A_MENU_ICON,
    kAShowStatus = IDC_A_SHOW_STATUS,
    kAShowSearch = IDC_A_SHOW_SEARCH,
    kASearchVisibility = IDC_A_SEARCH_VISIBILITY,
    kAShowTitle = IDC_A_SHOW_TITLE,
    kAShowFooter = IDC_A_SHOW_FOOTER,
    kAShowSpecial = IDC_A_SHOW_SPECIAL,
    kAShowIcons = IDC_A_SHOW_ICONS,
    kAShowSwatch = IDC_A_SHOW_SWATCH,

    kTMode = IDC_T_MODE,
    kTGallery = IDC_T_GALLERY,

    kSSaveFiles = IDC_S_SAVE_FILES,
    kSSaveImages = IDC_S_SAVE_IMAGES,
    kSSaveText = IDC_S_SAVE_TEXT,
    kSHistorySize = IDC_S_HISTORY_SIZE,
    kSSortBy = IDC_S_SORT_BY,
    kSStorageSize = IDC_S_STORAGE_SIZE,
    kSCurrentSize = IDC_S_CURRENT_SIZE,

    kIgnoreTabs = IDC_IGNORE_TABS,
    kIList = IDC_I_LIST,
    kIAdd = IDC_I_ADD,
    kIRemove = IDC_I_REMOVE,
    kIReset = IDC_I_RESET,
    kIWhitelist = IDC_I_WHITELIST,

    kPList = IDC_P_LIST,
    kPKey = IDC_P_KEY,
    kPTitle = IDC_P_TITLE,
    kPContent = IDC_P_CONTENT,
    kPSave = IDC_P_SAVE,
    kPDelete = IDC_P_DELETE,
    kPContentHint = IDC_P_CONTENT_HINT,

    kXIgnoreEvents = IDC_X_IGNORE_EVENTS,
    kXIgnoreNext = IDC_X_IGNORE_NEXT,
    kXClearOnQuit = IDC_X_CLEAR_ON_QUIT,
    kXClearClipboard = IDC_X_CLEAR_CLIPBOARD,
    kXRespectWindowsClipboardHistory = IDC_X_RESPECT_WINDOWS_CLIPBOARD_HISTORY,
};

constexpr int kPageGeneral = 0;
constexpr int kPageStorage = 1;
constexpr int kPageAppearance = 2;
constexpr int kPageThemes = 3;
constexpr int kPagePins = 4;
constexpr int kPageIgnore = 5;
constexpr int kPageAdvanced = 6;

int ScaleFromDefaultDpi(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

// Fits pages over the display area of the tab control they belong to. They
// are its siblings, so the area is computed in the shared parent's client
// coordinates.
template <typename Pages>
void FitPagesToTab(CTabCtrl tabs, const Pages &pages) {
    CRect area;
    tabs.GetWindowRect(&area);
    tabs.GetParent().ScreenToClient(&area);
    tabs.AdjustRect(FALSE, &area);
    area.right = std::max(area.right, area.left + 1);
    area.bottom = std::max(area.bottom, area.top + 1);
    for (const auto &page : pages) {
        if (page != nullptr && page->Handle() != nullptr) {
            page->Window().SetWindowPos(nullptr, &area, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}

void UpdateBehaviorHint(CStatic window, bool paste_default, bool plain_default) {
    const std::array<std::wstring, 3> key_expressions = {
        paste_default ? L"Alt+Enter" : L"Enter",
        paste_default ? L"Enter" : L"Alt+Enter",
        plain_default
            ? (paste_default ? L"Enter" : L"Alt+Shift+Enter")
            : (paste_default ? L"Ctrl+Shift+Enter" : L"Alt+Shift+Enter"),
    };

    const std::wstring hint = Localization::Format(
        IDS_G_BEHAVIOR_HINT,
        {key_expressions[0], key_expressions[1], key_expressions[2]}
    );
    window.SetWindowText(hint.c_str());
}

int ReadValidatedInteger(CEdit window, int fallback, int minimum, int maximum) {
    const int safe_fallback = std::clamp(fallback, minimum, maximum);
    const std::wstring text = ReadWindowText(window);
    try {
        size_t parsed = 0;
        const long long raw = std::stoll(text, &parsed);
        if (parsed != text.size()) {
            throw std::invalid_argument("invalid integer");
        }
        const int value = static_cast<int>(std::clamp(
            raw,
            static_cast<long long>(minimum),
            static_cast<long long>(maximum)
        ));
        window.SetWindowText(std::to_wstring(value).c_str());
        return value;
    } catch (...) {
        window.SetWindowText(std::to_wstring(safe_fallback).c_str());
        return safe_fallback;
    }
}

bool IsChecked(CButton window) {
    return window.m_hWnd != nullptr && window.GetCheck() == BST_CHECKED;
}

int ComboSelection(CComboBox window, int fallback = 0) {
    const int result = window.m_hWnd == nullptr ? CB_ERR : window.GetCurSel();
    return result == CB_ERR ? fallback : result;
}

void AddComboItem(CComboBox combo, const wchar_t *text) {
    combo.AddString(text);
}

void SetControlFont(CWindow window) {
    if (window.m_hWnd != nullptr) {
        window.SetFont(static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    }
}

int SelectedListViewItem(CListViewCtrl list) {
    return list.m_hWnd == nullptr ? -1 : list.GetNextItem(-1, LVNI_SELECTED);
}

void ConfigureListView(CListViewCtrl list, bool allow_label_editing, bool show_column_header) {
    if (list.m_hWnd == nullptr) {
        return;
    }

    const DWORD oldStyle = list.GetStyle();
    DWORD style = oldStyle;
    style |= LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS;
    if (allow_label_editing) {
        style |= LVS_EDITLABELS;
    } else {
        style &= ~static_cast<DWORD>(LVS_EDITLABELS);
    }
    if (show_column_header) {
        style &= ~static_cast<DWORD>(LVS_NOCOLUMNHEADER);
    } else {
        style |= LVS_NOCOLUMNHEADER;
    }
    list.ModifyStyle(
        oldStyle & ~style,
        style & ~oldStyle,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED
    );
    list.SetExtendedListViewStyle(
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER
    );
}

void AddListViewColumn(CListViewCtrl list, int index, int width, const wchar_t *title) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.cx = width;
    column.pszText = const_cast<wchar_t *>(title);
    list.InsertColumn(index, &column);
}

struct PageDefinition {
    UINT title_resource;
};

constexpr std::array<PageDefinition, AppConstants::SettingsUI::kPageCount> kPageDefinitions = {
    PageDefinition{IDS_SETTINGS_GENERAL},
    PageDefinition{IDS_SETTINGS_STORAGE},
    PageDefinition{IDS_SETTINGS_APPEARANCE},
    PageDefinition{IDS_SETTINGS_THEMES},
    PageDefinition{IDS_SETTINGS_PINS},
    PageDefinition{IDS_SETTINGS_IGNORE},
    PageDefinition{IDS_SETTINGS_ADVANCED},
};

struct IgnorePageDefinition {
    UINT title_resource;
};

constexpr std::array<IgnorePageDefinition, AppConstants::SettingsUI::kIgnorePageCount>
    kIgnorePageDefinitions = {
    IgnorePageDefinition{IDS_SETTINGS_IGNORE_APPLICATIONS},
    IgnorePageDefinition{IDS_SETTINGS_IGNORE_FORMATS},
    IgnorePageDefinition{IDS_SETTINGS_IGNORE_REGEXPS},
};

bool HasClass(HWND window, const wchar_t *class_name) {
    wchar_t name[32]{};
    return window != nullptr && ::GetClassNameW(window, name, ARRAYSIZE(name)) > 0 &&
        ::lstrcmpiW(name, class_name) == 0;
}

// Draws a check box or radio button the way the control would, but with the
// label in palette colours: with visual styles the control ignores the
// WM_CTLCOLORSTATIC text colour, which leaves dark text on a dark page.
void DrawCheckButton(CButton button, CDCHandle dc, CRect rect, bool radio) {
    const Theme::Palette &palette = Theme::Current();
    const UINT dpi = ::GetDpiForWindow(button);
    const bool enabled = button.IsWindowEnabled() != FALSE;
    const UINT state = button.GetState();
    const int check = button.GetCheck();
    const auto ui_state = static_cast<UINT>(button.SendMessage(WM_QUERYUISTATE));

    // Each check state has four glyphs in a row: normal, hot, pressed, disabled.
    const int column = !enabled ? 3 : (state & BST_PUSHED) != 0 ? 2 : (state & BST_HOT) != 0 ? 1 : 0;
    const int part = radio ? BP_RADIOBUTTON : BP_CHECKBOX;
    const int glyph_state = column + (radio
        ? (check == BST_CHECKED ? RBS_CHECKEDNORMAL : RBS_UNCHECKEDNORMAL)
        : (check == BST_CHECKED ? CBS_CHECKEDNORMAL
            : check == BST_INDETERMINATE ? CBS_MIXEDNORMAL : CBS_UNCHECKEDNORMAL));

    dc.FillRect(rect, Theme::WindowBrush());

    // Opened from the button, so it is the glyph set ApplyToControl chose
    // (dark or light) at the button's DPI.
    const HTHEME theme = ::OpenThemeDataForDpi(button, VSCLASS_BUTTON, dpi);
    SIZE glyph{ScaleFromDefaultDpi(13, dpi), ScaleFromDefaultDpi(13, dpi)};
    if (theme != nullptr) {
        ::GetThemePartSize(theme, dc, part, glyph_state, nullptr, TS_DRAW, &glyph);
    }
    // BS_VCENTER: the glyph and the label are centred in the control.
    const CRect box(CPoint(rect.left, rect.top + (rect.Height() - glyph.cy) / 2), CSize(glyph));
    if (theme != nullptr) {
        ::DrawThemeBackground(theme, dc, part, glyph_state, &box, nullptr);
        ::CloseThemeData(theme);
    } else {
        CRect frame = box;
        dc.DrawFrameControl(&frame, DFC_BUTTON,
            (radio ? DFCS_BUTTONRADIO : DFCS_BUTTONCHECK) |
            (check == BST_CHECKED ? DFCS_CHECKED : 0) | (enabled ? 0 : DFCS_INACTIVE));
    }

    const std::wstring text = ReadWindowText(button);
    const HFONT previous_font = dc.SelectFont(button.GetFont());
    // BS_MULTILINE everywhere in the templates: wrap like the control.
    const UINT format = DT_LEFT | DT_WORDBREAK | ((ui_state & UISF_HIDEACCEL) != 0 ? DT_HIDEPREFIX : 0);
    // The same gap AutoSizeTextControl reserves after the glyph.
    CRect label(box.right + ScaleFromDefaultDpi(4, dpi), rect.top, rect.right, rect.bottom);
    CRect measured = label;
    dc.DrawText(text.c_str(), -1, &measured, format | DT_CALCRECT);
    label.top += std::max(0, (label.Height() - measured.Height()) / 2);
    label.bottom = label.top + measured.Height();
    dc.SetBkMode(TRANSPARENT);
    dc.SetTextColor(enabled ? palette.text : palette.muted);
    dc.DrawText(text.c_str(), -1, &label, format);
    if ((state & BST_FOCUS) != 0 && (ui_state & UISF_HIDEFOCUS) == 0 && !text.empty()) {
        CRect focus(label.left - 1, label.top, label.left + measured.Width() + 1, label.bottom);
        dc.DrawFocusRect(&focus);
    }
    dc.SelectFont(previous_font);
}

// Tabs drawn like the native control: a body frame, the selected tab raised
// and joined to the body, the others on the surface colour with muted text.
void PaintTabs(CTabCtrl tabs, CDCHandle dc) {
    const Theme::Palette &palette = Theme::Current();
    const int line = std::max(1, ScaleFromDefaultDpi(1, ::GetDpiForWindow(tabs)));
    const auto border = [&](int left, int top, int right, int bottom) {
        dc.FillSolidRect(CRect(left, top, right, bottom), palette.border);
    };

    CRect client;
    tabs.GetClientRect(&client);
    dc.FillSolidRect(client, palette.window);

    const int count = tabs.GetItemCount();
    const int selected = tabs.GetCurSel();
    CRect body = client;
    if (count > 0) {
        CRect first;
        tabs.GetItemRect(0, &first);
        body.top = first.bottom;
    }
    border(body.left, body.top, body.right, body.top + line);
    border(body.left, body.top, body.left + line, body.bottom);
    border(body.right - line, body.top, body.right, body.bottom);
    border(body.left, body.bottom - line, body.right, body.bottom);

    const auto ui_state = static_cast<UINT>(tabs.SendMessage(WM_QUERYUISTATE));
    const UINT format = DT_CENTER | DT_VCENTER | DT_SINGLELINE |
        ((ui_state & UISF_HIDEACCEL) != 0 ? DT_HIDEPREFIX : 0);
    const HFONT previous_font = dc.SelectFont(tabs.GetFont());
    dc.SetBkMode(TRANSPARENT);
    const auto draw_tab = [&](int index) {
        const bool is_selected = index == selected;
        CRect tab;
        tabs.GetItemRect(index, &tab);
        if (is_selected) {
            tab.InflateRect(2 * line, 2 * line, 2 * line, 0);
            tab.bottom += line; // covers the body's top line under the tab
        }
        dc.FillSolidRect(tab, is_selected ? palette.window : palette.surface);
        border(tab.left, tab.top, tab.right, tab.top + line);
        border(tab.left, tab.top, tab.left + line, tab.bottom);
        border(tab.right - line, tab.top, tab.right, tab.bottom);

        wchar_t text[128]{};
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = text;
        item.cchTextMax = ARRAYSIZE(text);
        tabs.GetItem(index, &item);
        dc.SetTextColor(is_selected ? palette.text : palette.muted);
        dc.DrawText(text, -1, &tab, format);
        if (is_selected && ::GetFocus() == tabs.m_hWnd && (ui_state & UISF_HIDEFOCUS) == 0) {
            CRect focus = tab;
            focus.DeflateRect(3 * line, 3 * line);
            dc.DrawFocusRect(&focus);
        }
    };
    // The selected tab last, so its raised edges overlap its neighbours.
    for (int index = 0; index < count; ++index) {
        if (index != selected) {
            draw_tab(index);
        }
    }
    if (selected >= 0 && selected < count) {
        draw_tab(selected);
    }
    dc.SelectFont(previous_font);
}

}

LRESULT ThemedDialog::OnControlColor(UINT message, WPARAM wParam, LPARAM lParam, BOOL &handled) {
    const Theme::Palette &palette = Theme::Current();
    CDCHandle dc(reinterpret_cast<HDC>(wParam));
    const HWND control = reinterpret_cast<HWND>(lParam);
    const bool enabled = ::IsWindowEnabled(control) != FALSE;
    // Read-only and disabled edits ask for static colours, but they are still
    // input fields: the hotkey boxes would otherwise look like plain labels.
    const bool field = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX ||
        (message == WM_CTLCOLORSTATIC && HasClass(control, WC_EDITW));
    if (palette.high_contrast) {
        if (message == WM_CTLCOLORSTATIC && field && enabled) {
            dc.SetBkColor(::GetSysColor(COLOR_WINDOW));
            dc.SetTextColor(::GetSysColor(COLOR_WINDOWTEXT));
            return reinterpret_cast<LRESULT>(::GetSysColorBrush(COLOR_WINDOW));
        }
        handled = FALSE;
        return 0;
    }
    // The Themes page hint is secondary text under its field, like a
    // placeholder, so it takes the muted colour.
    const bool hint = message == WM_CTLCOLORSTATIC && ::GetDlgCtrlID(control) == IDC_T_MODE_HINT;
    dc.SetTextColor(enabled && !hint ? palette.text : palette.muted);
    dc.SetBkColor(field ? palette.surface : palette.window);
    return reinterpret_cast<LRESULT>(field ? Theme::SurfaceBrush() : Theme::WindowBrush());
}

// The templates' only owner-drawn statics are separator lines: an etched
// static ignores every colour message, so it would stay a light 3D groove.
LRESULT ThemedDialog::OnDrawItem(UINT, WPARAM, LPARAM lParam, BOOL &handled) {
    const auto *item = reinterpret_cast<const DRAWITEMSTRUCT *>(lParam);
    if (item == nullptr || item->CtlType != ODT_STATIC) {
        handled = FALSE;
        return 0;
    }
    CDCHandle dc(item->hDC);
    CRect rect(item->rcItem);
    const Theme::Palette &palette = Theme::Current();
    if (palette.high_contrast) {
        ::DrawThemeParentBackground(item->hwndItem, dc, &rect);
        dc.DrawEdge(&rect, EDGE_ETCHED, BF_TOP);
        return TRUE;
    }
    dc.FillRect(rect, Theme::WindowBrush());
    const int line = std::max(1, ScaleFromDefaultDpi(1, ::GetDpiForWindow(item->hwndItem)));
    rect.bottom = std::min(rect.bottom, rect.top + line);
    dc.FillSolidRect(rect, palette.border);
    return TRUE;
}

LRESULT ThemedDialog::OnCustomDraw(int, LPNMHDR header, BOOL &handled) {
    CButton button(header->hwndFrom);
    const UINT type = HasClass(button, WC_BUTTONW) ? (button.GetButtonStyle() & BS_TYPEMASK) : BS_PUSHBUTTON;
    const bool check = type == BS_CHECKBOX || type == BS_AUTOCHECKBOX ||
        type == BS_3STATE || type == BS_AUTO3STATE;
    const bool radio = type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
    if (Theme::Current().high_contrast || (!check && !radio)) {
        handled = FALSE;
        return 0;
    }
    const auto *draw = reinterpret_cast<const NMCUSTOMDRAW *>(header);
    if (draw->dwDrawStage != CDDS_PREPAINT) {
        return CDRF_DODEFAULT;
    }
    DrawCheckButton(button, draw->hdc, draw->rc, radio);
    return CDRF_SKIPDEFAULT;
}

void ThemedTabCtrl::Attach(HWND window) {
    if (IsWindow()) {
        UnsubclassWindow();
    }
    SubclassWindow(window);
}

LRESULT ThemedTabCtrl::OnEraseBackground(UINT, WPARAM, LPARAM, BOOL &handled) {
    // OnPaint covers every pixel.
    handled = !Theme::Current().high_contrast;
    return 1;
}

LRESULT ThemedTabCtrl::OnPaint(UINT, WPARAM, LPARAM, BOOL &handled) {
    if (Theme::Current().high_contrast) {
        handled = FALSE;
        return 0;
    }
    CPaintDC dc(m_hWnd);
    PaintTabs(*this, dc.m_hDC);
    return 0;
}

void ThemedListView::Attach(HWND window) {
    if (IsWindow()) {
        UnsubclassWindow();
    }
    SubclassWindow(window);
}

LRESULT ThemedListView::OnHeaderCustomDraw(int, LPNMHDR header, BOOL &handled) {
    if (Theme::Current().high_contrast || header->hwndFrom != GetHeader().m_hWnd) {
        handled = FALSE;
        return 0;
    }
    const auto *draw = reinterpret_cast<const NMCUSTOMDRAW *>(header);
    if (draw->dwDrawStage == CDDS_PREPAINT) {
        return CDRF_NOTIFYITEMDRAW;
    }
    if (draw->dwDrawStage == CDDS_ITEMPREPAINT) {
        ::SetTextColor(draw->hdc, Theme::Current().text);
    }
    return CDRF_DODEFAULT;
}

void HotkeyCaptureEdit::Attach(HWND window) {
    if (IsWindow()) {
        UnsubclassWindow();
    }
    SubclassWindow(window);
}

void HotkeyCaptureEdit::SetHotKey(const HotKeyConfig &hotkey) {
    m_hotkey = hotkey;
    UpdateText();
}

void HotkeyCaptureEdit::UpdateText() {
    const std::wstring text = m_hotkey.virtual_key == 0
        ? Localization::Text(IDS_HOTKEY_EMPTY)
        : HotKeyToText(m_hotkey);
    SetWindowText(text.c_str());
}

LRESULT HotkeyCaptureEdit::OnCaptureHotkey(UINT, WPARAM virtual_key, LPARAM modifiers, BOOL &handled) {
    const UINT key = static_cast<UINT>(virtual_key);
    if (key == VK_BACK) {
        m_hotkey = HotKeyConfig{0, 0};
    } else if (modifiers == 0) {
        // Hotkeys must contain at least one modifier. Keep capturing until a
        // valid combination is entered.
        handled = TRUE;
        return 0;
    } else {
        m_hotkey = HotKeyConfig{static_cast<UINT>(modifiers), key};
    }
    UpdateText();
    // Leave the box so capture ends. The tab strip is a neutral target:
    // the next control could be another hotkey box, which would start a new
    // capture, and focusing the dialog itself gives focus straight back here.
    const HWND tabs = ::GetDlgItem(::GetAncestor(m_hWnd, GA_ROOT), IDC_SETTINGS_TABS);
    if (tabs != nullptr) {
        ::SetFocus(tabs);
    }
    handled = TRUE;
    return 0;
}

LRESULT HotkeyCaptureEdit::OnSetFocus(UINT, WPARAM, LPARAM, BOOL &handled) {
    KeyboardHandler::SetHotkeyCaptureActive(true);
    SetWindowText(Localization::Text(IDS_HOTKEY_PRESS).c_str());
    handled = FALSE;
    return 0;
}

LRESULT HotkeyCaptureEdit::OnKillFocus(UINT, WPARAM, LPARAM, BOOL &handled) {
    KeyboardHandler::SetHotkeyCaptureActive(false);
    UpdateText();
    handled = FALSE;
    return 0;
}

SettingsWindow::SettingsWindow(
    StorageWorker &storage,
    HWND owner,
    AppSettings settings,
    StorageWorker::IgnoreLists ignored_lists,
    SettingsChangedCallback on_changed,
    UpdateCheckCallback on_update_check
)
    : m_storage(storage),
      m_owner(owner),
      m_onChanged(std::move(on_changed)),
      m_onUpdateCheck(std::move(on_update_check)),
      m_settings(std::move(settings)),
      m_ignoredLists(std::move(ignored_lists)) {}

void SettingsWindow::SetSettingsSnapshot(const AppSettings &settings) {
    SetStateSnapshot(settings, m_ignoredLists);
}

void SettingsWindow::SetStateSnapshot(
    const AppSettings &settings,
    StorageWorker::IgnoreLists ignored_lists
) {
    m_settings = settings;
    m_ignoredLists = std::move(ignored_lists);
    for (size_t page = 0; page < m_ignorePageObjects.size(); ++page) {
        m_ignorePageObjects[page].SetValues(m_ignoredLists[page]);
    }
    if (m_hWnd != nullptr && IsWindow()) {
        LoadControlsFromSettings();
    }
}

void SettingsWindow::SetUpdateCheckBusy(bool busy) {
    m_updateCheckBusy = busy;
    if (m_gCheckNow.m_hWnd != nullptr && m_gCheckNow.IsWindow()) {
        m_gCheckNow.EnableWindow(busy ? FALSE : TRUE);
    }
}

bool SettingsWindow::CreateOrShow() {
    if (m_hWnd != nullptr && IsWindow()) {
        LoadControlsFromSettings();
        SetWindowPos(
            HWND_NOTOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE
        );
        ShowWindow(::IsIconic(m_hWnd) ? SW_RESTORE : SW_SHOWNORMAL);
        SetForegroundWindow(m_hWnd);
        return true;
    }

    m_destroying = false;
    const HWND window = Create(nullptr);
    if (window == nullptr) {
        return false;
    }

    HMONITOR monitor = ::MonitorFromWindow(m_owner.m_hWnd, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr) {
        monitor = ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }

    RECT work_area{0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN)};
    MONITORINFO monitor_info{sizeof(monitor_info)};
    if (monitor != nullptr && ::GetMonitorInfoW(monitor, &monitor_info)) {
        work_area = monitor_info.rcWork;
    }

    RECT window_rect{};
    GetWindowRect(&window_rect);
    const int window_width = window_rect.right - window_rect.left;
    const int window_height = window_rect.bottom - window_rect.top;
    const int x = work_area.left + ((work_area.right - work_area.left) - window_width) / 2;
    const int y = work_area.top + ((work_area.bottom - work_area.top) - window_height) / 2;
    SetWindowPos(HWND_NOTOPMOST, x, y, 0, 0, SWP_SHOWWINDOW | SWP_NOSIZE);
    ::SetForegroundWindow(window);
    return true;
}

void SettingsWindow::DestroyForOwner() {
    m_destroying = true;
    if (m_hWnd != nullptr && IsWindow()) {
        DestroyWindow();
    }
}

void SettingsWindow::CreateTabs() {
    m_tabs.Attach(GetDlgItem(kTabs));

    for (const PageDefinition &definition : kPageDefinitions) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        const std::wstring title = Localization::Text(definition.title_resource);
        item.pszText = const_cast<wchar_t *>(title.c_str());
        m_tabs.InsertItem(m_tabs.GetItemCount(), &item);
    }
}

void SettingsWindow::LocalizeControls() {
    SetWindowText(Localization::Text(IDS_SETTINGS_TITLE).c_str());

    const CWindow general = m_pages[kPageGeneral]->Window();
    SetLocalizedText(general, IDC_G_LAUNCH, IDS_G_LAUNCH);
    SetLocalizedText(general, IDC_G_UPDATES, IDS_G_UPDATES);
    SetLocalizedText(general, IDC_G_CHECK_NOW, IDS_G_CHECK_NOW);
    SetLocalizedText(general, IDC_G_OPEN_LABEL, IDS_G_OPEN);
    SetLocalizedText(general, IDC_G_PIN_LABEL, IDS_G_PIN);
    SetLocalizedText(general, IDC_G_DELETE_LABEL, IDS_G_DELETE);
    SetLocalizedText(general, IDC_G_PREVIEW_LABEL, IDS_G_PREVIEW);
    SetLocalizedText(general, IDC_G_SEARCH_LABEL, IDS_G_SEARCH);
    SetLocalizedText(general, IDC_G_BEHAVIOR_LABEL, IDS_G_BEHAVIOR);
    SetLocalizedText(general, IDC_G_PASTE_BY_DEFAULT, IDS_G_AUTO_PASTE);
    SetLocalizedText(general, IDC_G_REMOVE_FORMATTING, IDS_G_PLAIN_PASTE);
    SetLocalizedText(general, IDC_G_NOTIFICATIONS, IDS_G_NOTIFICATIONS);

    const CWindow appearance = m_pages[kPageAppearance]->Window();
    SetLocalizedText(appearance, IDC_A_POPUP_POSITION_LABEL, IDS_A_POPUP_POSITION);
    SetLocalizedText(appearance, IDC_A_SCREEN_LABEL, IDS_A_SCREEN);
    SetLocalizedText(appearance, IDC_A_RESET_POSITION, IDS_A_RESET_POSITION);
    SetLocalizedText(appearance, IDC_A_PIN_TO_LABEL, IDS_A_PIN_TO);
    SetLocalizedText(appearance, IDC_A_IMAGE_HEIGHT_LABEL, IDS_A_IMAGE_HEIGHT);
    SetLocalizedText(appearance, IDC_A_OPEN_PREVIEW, IDS_A_AUTO_PREVIEW);
    SetLocalizedText(appearance, IDC_A_PREVIEW_DELAY_LABEL, IDS_A_PREVIEW_DELAY);
    SetLocalizedText(appearance, IDC_A_HIGHLIGHT_LABEL, IDS_A_HIGHLIGHT);
    SetLocalizedText(appearance, IDC_A_SHOW_SPECIAL, IDS_A_SHOW_SPECIAL);
    SetLocalizedText(appearance, IDC_A_SHOW_STATUS, IDS_A_SHOW_STATUS);
    SetLocalizedText(appearance, IDC_A_SHOW_RECENT, IDS_A_SHOW_RECENT);
    SetLocalizedText(appearance, IDC_A_SHOW_SEARCH, IDS_A_SHOW_SEARCH);
    SetLocalizedText(appearance, IDC_A_SHOW_TITLE, IDS_A_SHOW_TITLE);
    SetLocalizedText(appearance, IDC_A_SHOW_ICONS, IDS_A_SHOW_ICONS);
    SetLocalizedText(appearance, IDC_A_SHOW_SWATCH, IDS_A_SHOW_SWATCH);
    SetLocalizedText(appearance, IDC_A_SHOW_FOOTER, IDS_A_SHOW_FOOTER);

    const CWindow themes = m_pages[kPageThemes]->Window();
    SetLocalizedText(themes, IDC_T_MODE_LABEL, IDS_T_MODE);
    SetLocalizedText(themes, IDC_T_MODE_HINT, IDS_T_MODE_HINT);

    const CWindow storage = m_pages[kPageStorage]->Window();
    SetLocalizedText(storage, IDC_S_SAVE_LABEL, IDS_S_SAVE);
    SetLocalizedText(storage, IDC_S_SAVE_FILES, IDS_S_FILES);
    SetLocalizedText(storage, IDC_S_SAVE_IMAGES, IDS_S_IMAGES);
    SetLocalizedText(storage, IDC_S_SAVE_TEXT, IDS_S_TEXT);
    SetLocalizedText(storage, IDC_S_DESCRIPTION, IDS_S_DESCRIPTION);
    SetLocalizedText(storage, IDC_S_COUNT_LABEL, IDS_S_COUNT);
    SetLocalizedText(storage, IDC_S_SORT_LABEL, IDS_S_SORT);

    SetLocalizedText(m_pages[kPagePins]->Window(), IDC_P_LIST_HINT, IDS_P_LIST_HINT);
    SetLocalizedText(m_pages[kPageAdvanced]->Window(), IDC_X_IGNORE_EVENTS, IDS_X_PAUSE);
    SetLocalizedText(m_pages[kPageAdvanced]->Window(), IDC_X_DESCRIPTION, IDS_X_PAUSE_DESCRIPTION);
    SetLocalizedText(m_pages[kPageAdvanced]->Window(), IDC_X_IGNORE_NEXT, IDS_X_IGNORE_NEXT);
    SetLocalizedText(m_pages[kPageAdvanced]->Window(), IDC_X_CLEAR_ON_QUIT, IDS_X_CLEAR_ON_QUIT);
    SetLocalizedText(m_pages[kPageAdvanced]->Window(), IDC_X_CLEAR_CLIPBOARD, IDS_X_CLEAR_CLIPBOARD);
    SetLocalizedText(
        m_pages[kPageAdvanced]->Window(),
        IDC_X_RESPECT_WINDOWS_CLIPBOARD_HISTORY,
        IDS_X_RESPECT_HISTORY
    );
    SetLocalizedText(
        m_pages[kPageAdvanced]->Window(),
        IDC_X_HISTORY_DESCRIPTION,
        IDS_X_HISTORY_DESCRIPTION
    );

    SetLocalizedText(
        m_ignorePages[0]->Window(),
        IDC_I_WHITELIST,
        IDS_I_WHITELIST
    );
    for (const auto &page : m_ignorePages) {
        SetLocalizedText(page->Window(), IDC_I_ADD, IDS_IGNORE_ADD);
        SetLocalizedText(page->Window(), IDC_I_REMOVE, IDS_IGNORE_REMOVE);
    }
    SetLocalizedText(m_ignorePages[1]->Window(), IDC_I_RESET, IDS_IGNORE_RESET);

}

bool SettingsWindow::CreatePageWindows() {
    if (m_tabs.m_hWnd == nullptr) {
        return false;
    }

    for (size_t page = 0; page < kPageDefinitions.size(); ++page) {
        if (m_pages[page]->CreatePage(m_tabs.m_hWnd, *this) == nullptr) {
            return false;
        }
    }

    const CWindow ignoreTabWindow = m_pages[kPageIgnore]->Control(kIgnoreTabs);
    if (ignoreTabWindow.m_hWnd == nullptr) {
        return false;
    }
    m_ignoreTabs.Attach(ignoreTabWindow);

    for (const IgnorePageDefinition &definition : kIgnorePageDefinitions) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        const std::wstring title = Localization::Text(definition.title_resource);
        item.pszText = const_cast<wchar_t *>(title.c_str());
        m_ignoreTabs.InsertItem(m_ignoreTabs.GetItemCount(), &item);
    }

    for (size_t page = 0; page < kIgnorePageDefinitions.size(); ++page) {
        if (m_ignorePages[page]->CreatePage(m_ignoreTabs.m_hWnd, *this) == nullptr) {
            return false;
        }
    }

    ConfigureIgnoreList();

    for (size_t page = 0; page < m_ignorePageObjects.size(); ++page) {
        m_ignorePageObjects[page].Initialize(
            m_ignorePages[page]->Window(),
            m_storage,
            m_ignoredLists[page]
        );
    }

    return true;
}

void SettingsWindow::LayoutPages() {
    if (m_tabs.m_hWnd == nullptr) {
        return;
    }

    // The tab control fills the window, as in the template; after a DPI
    // change only the window has been resized.
    CRect client;
    GetClientRect(&client);
    m_tabs.SetWindowPos(nullptr, &client, SWP_NOZORDER | SWP_NOACTIVATE);
    FitPagesToTab(m_tabs, m_pages);

    // The ignore page holds a second tab control with its own pages.
    if (m_ignoreTabs.m_hWnd != nullptr) {
        FitPagesToTab(m_ignoreTabs, m_ignorePages);
    }
}

void SettingsWindow::ScaleForDpi(UINT old_dpi, UINT new_dpi) {
    if (old_dpi == new_dpi || m_tabs.m_hWnd == nullptr) {
        return;
    }
    const auto scale = [&](int value) {
        return MulDiv(value, static_cast<int>(new_dpi), static_cast<int>(old_dpi));
    };

    LOGFONTW font_info{};
    if (!CFontHandle(m_tabs.GetFont()).GetLogFont(&font_info)) {
        return;
    }
    font_info.lfHeight = scale(font_info.lfHeight);
    CFont font;
    if (font.CreateFontIndirect(&font_info) == nullptr) {
        return;
    }
    m_tabs.SetFont(font, FALSE);

    // Every page's controls, including the ignore tab control and the ignore
    // pages placed on the ignore page. LayoutPages then fits the pages
    // themselves exactly.
    const auto scale_controls = [&](CWindow page) {
        for (CWindow control = page.GetWindow(GW_CHILD); control.m_hWnd != nullptr;
             control = control.GetWindow(GW_HWNDNEXT)) {
            CRect rect;
            control.GetWindowRect(&rect);
            page.ScreenToClient(&rect);
            control.SetWindowPos(nullptr, scale(rect.left), scale(rect.top),
                                 scale(rect.Width()), scale(rect.Height()),
                                 SWP_NOZORDER | SWP_NOACTIVATE);
            control.SetFont(font, FALSE);
        }
    };
    for (const auto &page : m_pages) {
        scale_controls(page->Window());
    }
    for (const auto &page : m_ignorePages) {
        scale_controls(page->Window());
    }

    // Keep any widths the user dragged, only in the new DPI.
    const int column_count = m_pList.GetHeader().GetItemCount();
    for (int column = 0; column < column_count; ++column) {
        m_pList.SetColumnWidth(column, scale(m_pList.GetColumnWidth(column)));
    }
    if (!m_ignoreImageList.IsNull()) {
        const int icon_size = ScaleFromDefaultDpi(16, new_dpi);
        m_ignoreImageList.SetIconSize(icon_size, icon_size);
        // Re-attach so the list recomputes its row height.
        CListViewCtrl(m_ignorePages[0]->Control(kIList)).SetImageList(m_ignoreImageList, LVSIL_SMALL);
    }

    // The controls now use the new font, so the previous one can go.
    m_dpiFont.Attach(font.Detach());
}

void SettingsWindow::BindControls() {
    const auto get = [this](int page, int id) {
        return m_pages[static_cast<size_t>(page)]->Control(id);
    };

    m_gCheckNow = get(kPageGeneral, kGCheckNow);
    m_gBehaviorHint = get(kPageGeneral, IDC_G_BEHAVIOR_HINT);
    m_gOpenHotKey.Attach(get(kPageGeneral, kGOpenHotKey).m_hWnd);
    m_gPinHotKey.Attach(get(kPageGeneral, kGPinHotKey).m_hWnd);
    m_gDeleteHotKey.Attach(get(kPageGeneral, kGDeleteHotKey).m_hWnd);
    m_gPreviewHotKey.Attach(get(kPageGeneral, kGPreviewHotKey).m_hWnd);
    m_gPasteByDefault = get(kPageGeneral, kGPasteByDefault);
    m_gRemoveFormatting = get(kPageGeneral, kGRemoveFormatting);

    m_aPopupPosition = get(kPageAppearance, kAPopupPosition);
    m_aResetPosition = get(kPageAppearance, kAResetPosition);
    m_aImageHeight = get(kPageAppearance, kAImageHeight);
    m_aOpenPreview = get(kPageAppearance, kAOpenPreview);
    m_aPreviewDelay = get(kPageAppearance, kAPreviewDelay);
    m_aMenuIcon = get(kPageAppearance, kAMenuIcon);
    m_aShowStatus = get(kPageAppearance, kAShowStatus);
    m_aShowSearch = get(kPageAppearance, kAShowSearch);
    m_aSearchVisibility = get(kPageAppearance, kASearchVisibility);
    m_aShowTitle = get(kPageAppearance, kAShowTitle);

    m_tGallery.Attach(get(kPageThemes, kTGallery));

    m_sHistorySize = get(kPageStorage, kSHistorySize);
    m_sStorageSize = get(kPageStorage, kSStorageSize);
    m_sCurrentSize = get(kPageStorage, kSCurrentSize);

    m_pList.Attach(get(kPagePins, kPList));
    m_xIgnoreEvents = get(kPageAdvanced, kXIgnoreEvents);
    m_xIgnoreNext = get(kPageAdvanced, kXIgnoreNext);

    CComboBox searchMode(get(kPageGeneral, kGSearchMode).m_hWnd);
    AddComboItem(searchMode, Localization::Text(IDS_SEARCH_EXACT).c_str());
    AddComboItem(searchMode, Localization::Text(IDS_SEARCH_FUZZY).c_str());
    AddComboItem(searchMode, Localization::Text(IDS_SEARCH_REGEX).c_str());
    AddComboItem(searchMode, Localization::Text(IDS_SEARCH_HYBRID).c_str());

    AddComboItem(m_aPopupPosition, Localization::Text(IDS_POPUP_CURSOR).c_str());
    AddComboItem(m_aPopupPosition, Localization::Text(IDS_POPUP_STATUS_ICON).c_str());
    AddComboItem(m_aPopupPosition, Localization::Text(IDS_POPUP_WINDOW_CENTER).c_str());
    AddComboItem(m_aPopupPosition, Localization::Text(IDS_POPUP_SCREEN_CENTER).c_str());
    AddComboItem(m_aPopupPosition, Localization::Text(IDS_POPUP_LAST_POSITION).c_str());

    CComboBox pinTo(get(kPageAppearance, kAPinTo).m_hWnd);
    AddComboItem(pinTo, Localization::Text(IDS_PIN_TOP).c_str());
    AddComboItem(pinTo, Localization::Text(IDS_PIN_BOTTOM).c_str());

    // Indexed by ThemeMode.
    CComboBox themeMode(get(kPageThemes, kTMode).m_hWnd);
    AddComboItem(themeMode, Localization::Text(IDS_THEME_MODE_SYSTEM).c_str());
    AddComboItem(themeMode, Localization::Text(IDS_THEME_MODE_LIGHT).c_str());
    AddComboItem(themeMode, Localization::Text(IDS_THEME_MODE_DARK).c_str());

    CComboBox highlight(get(kPageAppearance, kAHighlight).m_hWnd);
    AddComboItem(highlight, Localization::Text(IDS_HIGHLIGHT_COLOR).c_str());
    AddComboItem(highlight, Localization::Text(IDS_HIGHLIGHT_BOLD).c_str());
    AddComboItem(highlight, Localization::Text(IDS_HIGHLIGHT_ITALIC).c_str());
    AddComboItem(highlight, Localization::Text(IDS_HIGHLIGHT_EMPHASIS).c_str());

    AddComboItem(m_aMenuIcon, L"maccy");
    AddComboItem(m_aMenuIcon, Localization::Text(IDS_MENU_ICON_CLIPBOARD).c_str());
    AddComboItem(m_aMenuIcon, Localization::Text(IDS_MENU_ICON_SCISSORS).c_str());
    AddComboItem(m_aMenuIcon, Localization::Text(IDS_MENU_ICON_PAPERCLIP).c_str());

    AddComboItem(m_aSearchVisibility, Localization::Text(IDS_SEARCH_ALWAYS).c_str());
    AddComboItem(m_aSearchVisibility, Localization::Text(IDS_SEARCH_WHILE_TYPING).c_str());

    CComboBox sortBy(get(kPageStorage, kSSortBy).m_hWnd);
    AddComboItem(sortBy, Localization::Text(IDS_SORT_LAST_COPIED).c_str());
    AddComboItem(sortBy, Localization::Text(IDS_SORT_FIRST_COPIED).c_str());
    AddComboItem(sortBy, Localization::Text(IDS_SORT_COPY_COUNT).c_str());

    ConfigurePinsList();
    LayoutPages();
}

void SettingsWindow::ConfigureIgnoreList() {
    const int icon_size = ScaleFromDefaultDpi(16, m_dpi);
    if (m_ignoreImageList.IsNull()) {
        m_ignoreImageList.Create(
            icon_size,
            icon_size,
            ILC_COLOR32 | ILC_MASK,
            8,
            8
        );
    } else {
        m_ignoreImageList.SetIconSize(icon_size, icon_size);
    }

    for (size_t index = 0; index < m_ignorePages.size(); ++index) {
        CListViewCtrl list;
        list = m_ignorePages[index]->Control(kIList);
        ConfigureListView(list, index != 0, false);
        // IgnorePage::Refresh widens the single column to the list.
        AddListViewColumn(list, 0, ScaleFromDefaultDpi(100, m_dpi), L"");
        if (index == 0 && !m_ignoreImageList.IsNull()) {
            list.SetImageList(m_ignoreImageList, LVSIL_SMALL);
        }
    }
}

void SettingsWindow::ConfigurePinsList() {
    ConfigureListView(m_pList, false, true);
    AddListViewColumn(m_pList, 0, ScaleFromDefaultDpi(63, m_dpi), Localization::Text(IDS_P_KEY).c_str());
    AddListViewColumn(m_pList, 1, ScaleFromDefaultDpi(158, m_dpi), Localization::Text(IDS_P_TITLE).c_str());
    AddListViewColumn(m_pList, 2, ScaleFromDefaultDpi(300, m_dpi), Localization::Text(IDS_P_CONTENT).c_str());
}

// What controls keep once set; everything else reads Theme::Current() when
// it paints.
void SettingsWindow::ApplyThemeColors() {
    const Theme::Palette &palette = Theme::Current();
    // The tab-body texture only matches the native tab control, which high
    // contrast keeps; otherwise WM_CTLCOLORDLG gives the pages the palette.
    const DWORD texture = palette.high_contrast ? ETDT_ENABLETAB : ETDT_DISABLE;
    for (const auto &page : m_pages) {
        ::EnableThemeDialogTexture(page->Handle(), texture);
    }
    for (const auto &page : m_ignorePages) {
        ::EnableThemeDialogTexture(page->Handle(), texture);
    }

    // Lists read like the text boxes. In high contrast the palette holds the
    // system window colours, which are the list view defaults.
    const COLORREF background = palette.high_contrast ? palette.window : palette.surface;
    const auto color_list = [&](CListViewCtrl list) {
        if (list.m_hWnd != nullptr) {
            list.SetBkColor(background);
            list.SetTextBkColor(background);
            list.SetTextColor(palette.text);
        }
    };
    color_list(m_pList);
    for (const IgnorePage &page : m_ignorePageObjects) {
        color_list(page.ListWindow());
    }
}

void SettingsWindow::SetPage(int page) {
    m_currentPage = std::clamp(page, 0, AppConstants::SettingsUI::kPageCount - 1);
    if (m_tabs.m_hWnd != nullptr) {
        m_tabs.SetCurSel(m_currentPage);
    }

    for (size_t index = 0; index < m_pages.size(); ++index) {
        CWindow pageWindow = m_pages[index]->Window();
        if (pageWindow.m_hWnd != nullptr) {
            pageWindow.ShowWindow(
                static_cast<int>(index) == m_currentPage ? SW_SHOW : SW_HIDE
            );
        }
    }

    // The ignore page shows one of its sub-pages; bring the current one up.
    if (m_currentPage == kPageIgnore) {
        SetIgnorePage(m_ignorePage);
    }
}

void SettingsWindow::SetIgnorePage(int page) {
    m_ignorePage = std::clamp(page, 0, AppConstants::SettingsUI::kIgnorePageCount - 1);
    if (m_ignoreTabs.m_hWnd != nullptr) {
        m_ignoreTabs.SetCurSel(m_ignorePage);
    }

    // Hide every sub-page...
    for (size_t i = 0; i < m_ignorePageObjects.size(); ++i) {
        m_ignorePageObjects[i].Hide();
    }

    // ...then show the current one.
    m_ignorePageObjects[static_cast<size_t>(m_ignorePage)].Show();
}

void SettingsWindow::LoadGeneralControls() {
    auto &page = *m_pages[kPageGeneral];
    page.LoadSettings(m_settings);
    page.ExchangeSettings(DDX_LOAD);
    m_gOpenHotKey.SetHotKey(m_settings.open_hotkey);
    m_gPinHotKey.SetHotKey(m_settings.pin_hotkey);
    m_gDeleteHotKey.SetHotKey(m_settings.delete_hotkey);
    m_gPreviewHotKey.SetHotKey(m_settings.preview_hotkey);
}

void SettingsWindow::LoadAppearanceControls() {
    CComboBox popupScreen(m_pages[kPageAppearance]->Control(kAPopupScreen).m_hWnd);
    popupScreen.ResetContent();
    const int monitor_count = std::max(1, GetSystemMetrics(SM_CMONITORS));
    AddComboItem(popupScreen, Localization::Text(IDS_ACTIVE_SCREEN).c_str());
    for (int index = 0; index < monitor_count; ++index) {
        const std::wstring name = Localization::Format(
            IDS_MONITOR,
            {std::to_wstring(index + 1)}
        );
        AddComboItem(popupScreen, name.c_str());
    }
    auto &page = *m_pages[kPageAppearance];
    page.LoadSettings(m_settings);
    page.ExchangeSettings(DDX_LOAD);
    m_aImageHeight.SetWindowText(std::to_wstring(m_settings.image_max_height).c_str());
    m_aPreviewDelay.SetWindowText(std::to_wstring(m_settings.preview_delay).c_str());
    m_aPreviewDelay.EnableWindow(m_settings.open_preview_automatically);
    UpdateDependencies();
}

void SettingsWindow::LoadThemesControls() {
    auto &page = *m_pages[kPageThemes];
    page.LoadSettings(m_settings);
    page.ExchangeSettings(DDX_LOAD);
    m_tGallery.SetSelection(m_settings.theme);
}

void SettingsWindow::UpdateDependencies() {
    UpdateBehaviorHint(
        m_gBehaviorHint,
        IsChecked(m_gPasteByDefault),
        IsChecked(m_gRemoveFormatting)
    );
    m_aSearchVisibility.EnableWindow(IsChecked(m_aShowSearch));
    m_aShowTitle.EnableWindow(IsChecked(m_aShowSearch));
    m_aPreviewDelay.EnableWindow(IsChecked(m_aOpenPreview));
    m_aMenuIcon.EnableWindow(IsChecked(m_aShowStatus));
    const auto position = static_cast<PopupPosition>(ComboSelection(m_aPopupPosition));
    m_aResetPosition.EnableWindow(position == PopupPosition::LastPosition);
}

void SettingsWindow::LoadStorageControls() {
    auto &page = *m_pages[kPageStorage];
    page.LoadSettings(m_settings);
    page.ExchangeSettings(DDX_LOAD);
    m_sHistorySize.SetWindowText(std::to_wstring(m_settings.history_size).c_str());
    const std::uintmax_t storage_bytes = m_storage.StorageBytes();
    const sqlite3_int64 current_count = m_storage.CountItems();
    m_sStorageSize.SetWindowText(FormatByteCount(storage_bytes).c_str());
    const std::wstring current_size_text = Localization::Format(
        IDS_CURRENT_ITEMS,
        {std::to_wstring(current_count)}
    );
    m_sCurrentSize.SetWindowText(current_size_text.c_str());
}

void SettingsWindow::LoadAdvancedControls() {
    auto &page = *m_pages[kPageAdvanced];
    page.LoadSettings(m_settings);
    page.ExchangeSettings(DDX_LOAD);
}

void SettingsWindow::LoadControlsFromSettings() {
    m_loading = true;
    LoadGeneralControls();
    LoadAppearanceControls();
    LoadThemesControls();
    LoadStorageControls();
    LoadAdvancedControls();

    // The whitelist belongs to the applications page.
    m_ignorePages[0]->LoadSettings(m_settings);
    m_ignorePages[0]->ExchangeSettings(DDX_LOAD);

    m_loading = false;
}

void SettingsWindow::RefreshPinsList() {
    if (m_pList.m_hWnd == nullptr) {
        return;
    }

    const sqlite3_int64 previous_selection = [&] {
        const int selected = SelectedListViewItem(m_pList);
        return selected >= 0 && selected < static_cast<int>(m_pins.size())
            ? m_pins[static_cast<size_t>(selected)].id
            : 0;
    }();
    auto pins = m_storage.GetPinnedItems(PayloadMode::Metadata);
    std::stable_sort(pins.begin(), pins.end(), [](const ClipboardItem &lhs, const ClipboardItem &rhs) {
        if (lhs.first_copied_at != rhs.first_copied_at) {
            return lhs.first_copied_at < rhs.first_copied_at;
        }
        return lhs.id < rhs.id;
    });
    m_pins = std::move(pins);

    m_pList.DeleteAllItems();
    for (size_t index = 0; index < m_pins.size(); ++index) {
        const ClipboardItem &item = m_pins[index];
        LVITEMW row{};
        row.mask = LVIF_TEXT | LVIF_PARAM;
        row.iItem = static_cast<int>(index);
        row.lParam = item.id;
        row.pszText = const_cast<wchar_t *>(item.pin.c_str());
        m_pList.InsertItem(&row);

        m_pList.SetItemText(
            static_cast<int>(index),
            1,
            item.title.c_str()
        );
        std::wstring content;
        if (item.has_text) {
            content = PinTextContent(item);
            if (content.empty()) {
                content = item.preview;
            }
        } else {
            content = Localization::Text(IDS_UNEDITABLE_CONTENT);
        }
        for (wchar_t &character : content) {
            if (character == L'\r' || character == L'\n') {
                character = L' ';
            }
        }
        if (content.size() > 120) {
            content.resize(120);
            content += L"…";
        }
        m_pList.SetItemText(static_cast<int>(index), 2, content.c_str());
    }

    int selected_index = -1;
    if (previous_selection != 0) {
        for (size_t index = 0; index < m_pins.size(); ++index) {
            if (m_pins[index].id == previous_selection) {
                selected_index = static_cast<int>(index);
                break;
            }
        }
    }
    if (selected_index < 0 && !m_pins.empty()) {
        selected_index = 0;
    }
    if (selected_index >= 0) {
        m_pList.SetItemState(
            selected_index,
            LVIS_SELECTED | LVIS_FOCUSED,
            LVIS_SELECTED | LVIS_FOCUSED
        );
    }
}

void SettingsWindow::SaveCurrentPage() {
    if (m_loading) {
        return;
    }

    const AppSettings previous = m_settings;
    try {
        switch (m_currentPage) {
        case kPageGeneral: {
            m_pages[kPageGeneral]->AttachSettings(m_settings);
            m_pages[kPageGeneral]->ExchangeSettings(DDX_SAVE);
            const HotKeyConfig openHotkey = m_gOpenHotKey.GetHotKey();
            const HotKeyConfig pinHotkey = m_gPinHotKey.GetHotKey();
            const HotKeyConfig deleteHotkey = m_gDeleteHotKey.GetHotKey();
            const HotKeyConfig previewHotkey = m_gPreviewHotKey.GetHotKey();
            const std::wstring conflict = FindHotkeyConflict(
                openHotkey,
                pinHotkey,
                deleteHotkey,
                previewHotkey
            );
            if (!conflict.empty()) {
                m_settings = previous;
                m_loading = true;
                LoadGeneralControls();
                m_loading = false;
                const std::wstring title = Localization::Text(IDS_SETTINGS_ERROR_TITLE);
                ::MessageBoxW(m_hWnd, conflict.c_str(), title.c_str(), MB_OK | MB_ICONWARNING);
                return;
            }
            m_settings.open_hotkey = openHotkey;
            m_settings.pin_hotkey = pinHotkey;
            m_settings.delete_hotkey = deleteHotkey;
            m_settings.preview_hotkey = previewHotkey;
            break;
        }
        case kPageAppearance: {
            m_pages[kPageAppearance]->AttachSettings(m_settings);
            m_pages[kPageAppearance]->ExchangeSettings(DDX_SAVE);
            m_settings.image_max_height = ReadValidatedInteger(
                m_aImageHeight,
                m_settings.image_max_height,
                1,
                200
            );
            m_settings.preview_delay = ReadValidatedInteger(
                m_aPreviewDelay,
                m_settings.preview_delay,
                200,
                100000
            );
            break;
        }
        case kPageThemes:
            m_pages[kPageThemes]->AttachSettings(m_settings);
            m_pages[kPageThemes]->ExchangeSettings(DDX_SAVE);
            m_settings.theme = m_tGallery.Selection();
            break;
        case kPageStorage:
            m_pages[kPageStorage]->AttachSettings(m_settings);
            m_pages[kPageStorage]->ExchangeSettings(DDX_SAVE);
            m_settings.history_size = ReadValidatedInteger(
                m_sHistorySize,
                m_settings.history_size,
                1,
                999
            );
            break;
        case kPageIgnore:
            m_ignorePages[0]->AttachSettings(m_settings);
            m_ignorePages[0]->ExchangeSettings(DDX_SAVE);
            break;
        case kPageAdvanced:
            m_pages[kPageAdvanced]->AttachSettings(m_settings);
            m_pages[kPageAdvanced]->ExchangeSettings(DDX_SAVE);
            break;
        default:
            break;
        }

        const AppSettings snapshot = m_settings;
        m_storage.SaveSettings(snapshot);
        if (previous.history_size != snapshot.history_size) {
            m_storage.TrimUnpinned(snapshot.history_size);
        }
        if (previous.show_special_symbols != snapshot.show_special_symbols) {
            m_storage.RegenerateTitles(snapshot.show_special_symbols);
        }
        m_settings = snapshot;
        UpdateDependencies();
        NotifyOwner();
    } catch (const std::exception &error) {
        m_settings = previous;
        LoadControlsFromSettings();
        ShowSettingsRuntimeError(m_hWnd, error.what(), IDS_SETTINGS_SAVE_ERROR);
    }
}

void SettingsWindow::NotifyOwner(std::uint32_t updateMask) {
    if ((updateMask & AppConstants::UiUpdate::kIgnoreRules) != 0 &&
        m_ignorePage >= 0 && m_ignorePage < static_cast<int>(m_ignorePageObjects.size())) {
        m_ignoredLists[static_cast<size_t>(m_ignorePage)] =
            m_ignorePageObjects[static_cast<size_t>(m_ignorePage)].Values();
    }
    if (m_onChanged) {
        m_onChanged(m_settings, updateMask);
    }
}

void SettingsWindow::EditSelectedPin() {
    const int selected = SelectedListViewItem(m_pList);
    if (selected < 0 || selected >= static_cast<int>(m_pins.size())) {
        return;
    }

    const sqlite3_int64 item_id = m_pins[selected].id;
    try {
        const auto item = m_storage.GetItem(item_id, PayloadMode::Full);
        if (!item.has_value()) {
            return;
        }
        EditPinDialog dialog(m_settings, item->id, m_pins, *item);
        if (dialog.DoModal(m_hWnd) != IDOK) {
            return;
        }
        const sqlite3_int64 edited_id = dialog.GetItemId();
        const std::wstring key = dialog.GetKey();
        const std::wstring title = dialog.GetTitle();
        if (dialog.ContentModified()) {
            m_storage.UpdatePinnedItem(edited_id, key, title, dialog.GetContent());
        } else {
            m_storage.UpdatePinnedMetadata(edited_id, key, title);
        }
        RefreshPinsList();
        NotifyOwner();
    } catch (const std::exception &error) {
        ShowSettingsRuntimeError(m_hWnd, error.what(), IDS_SETTINGS_EDIT_PIN_ERROR);
    } catch (...) {
        ShowSettingsRuntimeError(m_hWnd, {}, IDS_SETTINGS_EDIT_PIN_ERROR);
    }
}

void SettingsWindow::DeleteSelectedPin() {
    const int selected = SelectedListViewItem(m_pList);
    if (selected < 0 || selected >= static_cast<int>(m_pins.size())) {
        return;
    }
    const std::wstring delete_message = Localization::Text(IDS_SETTINGS_DELETE_PIN_CONFIRM);
    const std::wstring confirm_title = Localization::Text(IDS_SETTINGS_CONFIRM_TITLE);
    if (::MessageBoxW(
            m_hWnd,
            delete_message.c_str(),
            confirm_title.c_str(),
            MB_YESNO | MB_ICONQUESTION
        ) != IDYES) {
        return;
    }
    const sqlite3_int64 item_id = m_pins[selected].id;
    try {
        m_storage.DeleteItem(item_id);
        RefreshPinsList();
        NotifyOwner();
    } catch (const std::exception &error) {
        ShowSettingsRuntimeError(m_hWnd, error.what(), IDS_SETTINGS_DELETE_PIN_ERROR);
    } catch (...) {
        ShowSettingsRuntimeError(m_hWnd, {}, IDS_SETTINGS_DELETE_PIN_ERROR);
    }
}

void SettingsWindow::OpenNotificationsSettings() {
    ShellExecuteW(m_hWnd, L"open", L"ms-settings:notifications", nullptr, nullptr, SW_SHOWNORMAL);
}

void SettingsWindow::CheckForUpdatesNow() {
    if (m_updateCheckBusy) {
        const std::wstring message = Localization::Text(IDS_SETTINGS_UPDATE_BUSY);
        const std::wstring title = Localization::Text(IDS_MAIN_UPDATE_CHECK_TITLE);
        ::MessageBoxW(
            m_hWnd,
            message.c_str(),
            title.c_str(),
            MB_OK | MB_ICONINFORMATION
        );
        return;
    }
    if (m_onUpdateCheck == nullptr || !m_onUpdateCheck()) {
        const std::wstring message = Localization::Text(IDS_SETTINGS_UPDATE_START_ERROR);
        const std::wstring title = Localization::Text(IDS_MAIN_UPDATE_CHECK_TITLE);
        ::MessageBoxW(
            m_hWnd,
            message.c_str(),
            title.c_str(),
            MB_OK | MB_ICONWARNING
        );
    }
}

void SettingsWindow::ResetPopupPosition() {
    const AppSettings previous = m_settings;
    m_settings.popup_x = 0;
    m_settings.popup_y = 0;
    try {
        m_storage.SaveSettings(m_settings);
        NotifyOwner();
    } catch (const std::exception &error) {
        m_settings = previous;
        LoadControlsFromSettings();
        ShowSettingsRuntimeError(m_hWnd, error.what(), IDS_SETTINGS_SAVE_ERROR);
    } catch (...) {
        m_settings = previous;
        LoadControlsFromSettings();
        ShowSettingsRuntimeError(m_hWnd, {}, IDS_SETTINGS_SAVE_ERROR);
    }
}

LRESULT SettingsWindow::OnInitDialog(UINT, WPARAM, LPARAM, BOOL &handled) {
    handled = TRUE;
    // Keep preferences as a normal top-level window so it remains visible in
    // the taskbar and Alt+Tab without inheriting the main window's topmost state.
    SetWindowText(Localization::Text(IDS_SETTINGS_TITLE).c_str());
    m_windowIcon = LoadApplicationIcon();
    if (m_windowIcon) {
        SetIcon(m_windowIcon.Get(), TRUE);
        SetIcon(m_windowIcon.Get(), FALSE);
    }
    ModifyStyleEx(
        WS_EX_TOOLWINDOW,
        WS_EX_APPWINDOW,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED
    );
    SetWindowPos(
        HWND_NOTOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED
    );

    // Per-Monitor v2 rescales only a dialog's immediate children, but the
    // pages here are dialogs of their own. OnDpiChanged scales the whole
    // tree instead, and must be the only one doing it.
    m_dpi = ::GetDpiForWindow(m_hWnd);
    ::SetDialogDpiChangeBehavior(m_hWnd, DDC_DISABLE_ALL, DDC_DISABLE_ALL);

    CreateTabs();
    if (!CreatePageWindows()) {
        handled = FALSE;
        return FALSE;
    }
    LocalizeControls();
    LayoutPages();
    BindControls();
    LoadControlsFromSettings();
    SetUpdateCheckBusy(m_updateCheckBusy);
    SetPage(kPageGeneral);
    SetIgnorePage(0);
    // MainWindow re-applies both on later theme changes, see OnThemeChanged.
    Theme::ApplyToWindow(m_hWnd);
    ApplyThemeColors();

    // Settings is modeless, so the main loop must route its keys through
    // the dialog manager for Tab, Esc, Enter and arrow-key navigation.
    if (CMessageLoop *loop = _Module.GetMessageLoop(); loop != nullptr) {
        loop->AddMessageFilter(this);
    }
    return TRUE;
}

BOOL SettingsWindow::PreTranslateMessage(MSG *message) {
    // IsDialogMessage ignores messages for windows outside this dialog, so
    // the history popup keeps its own keyboard handling.
    return m_hWnd != nullptr && IsDialogMessage(message);
}

LRESULT SettingsWindow::OnDpiChanged(UINT, WPARAM wParam, LPARAM lParam, BOOL &handled) {
    handled = TRUE;
    const UINT dpi = LOWORD(wParam);
    // Scale the controls before resizing the window: the resize lays the
    // tab controls and pages out, and that must use the new tab metrics.
    ScaleForDpi(m_dpi, dpi);
    m_dpi = dpi;
    const auto *suggested = reinterpret_cast<const RECT *>(lParam);
    if (suggested != nullptr) {
        SetWindowPos(nullptr, suggested, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    LayoutPages();
    if (m_currentPage == kPageIgnore) {
        m_ignorePageObjects[static_cast<size_t>(m_ignorePage)].Refresh();
    }
    RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    return 0;
}

LRESULT SettingsWindow::OnSize(UINT, WPARAM, LPARAM, BOOL &handled) {
    handled = TRUE;
    LayoutPages();
    return 0;
}

LRESULT SettingsWindow::OnClose(UINT, WPARAM, LPARAM, BOOL &handled) {
    handled = TRUE;
    SaveCurrentPage();
    if (!m_destroying) {
        ShowWindow(SW_HIDE);
    } else {
        DestroyWindow();
    }
    return 0;
}

LRESULT SettingsWindow::OnCancelCommand(WORD, WORD, HWND, BOOL &handled) {
    // The dialog manager turns Esc into IDCANCEL.
    handled = TRUE;
    SendMessage(WM_CLOSE);
    return 0;
}

LRESULT SettingsWindow::OnCheckUpdatesCommand(WORD, WORD, HWND, BOOL &handled) {
    handled = TRUE;
    if (!m_loading) {
        CheckForUpdatesNow();
    }
    return 0;
}

LRESULT SettingsWindow::OnNotificationsCommand(WORD, WORD, HWND, BOOL &handled) {
    handled = TRUE;
    if (!m_loading) {
        OpenNotificationsSettings();
    }
    return 0;
}

LRESULT SettingsWindow::OnResetPositionCommand(WORD, WORD, HWND, BOOL &handled) {
    handled = TRUE;
    if (!m_loading) {
        ResetPopupPosition();
    }
    return 0;
}

LRESULT SettingsWindow::OnIgnorePageCommand(WORD, WORD id, HWND, BOOL &handled) {
    handled = TRUE;
    if (m_loading) {
        return 0;
    }

    IgnorePage &page = m_ignorePageObjects[static_cast<size_t>(m_ignorePage)];
    bool changed = false;
    switch (id) {
    case kIAdd:
        changed = page.AddValue();
        break;
    case kIRemove:
        changed = page.RemoveValue();
        break;
    case kIReset:
        changed = page.ResetToDefaults();
        break;
    default:
        handled = FALSE;
        return 0;
    }
    if (changed) {
        NotifyOwner(AppConstants::UiUpdate::kIgnoreRules);
    }
    return 0;
}

LRESULT SettingsWindow::OnSettingChanged(WORD, WORD id, HWND, BOOL &handled) {
    handled = TRUE;
    if (m_loading) {
        return 0;
    }
    if (id == kAOpenPreview) {
        m_aPreviewDelay.EnableWindow(IsChecked(m_aOpenPreview));
    }
    SaveCurrentPage();
    return 0;
}

LRESULT SettingsWindow::OnTabsSelectionChanged(int, LPNMHDR, BOOL &handled) {
    handled = TRUE;
    SaveCurrentPage();
    SetPage(m_tabs.GetCurSel());
    if (m_currentPage == kPageIgnore) {
        SetIgnorePage(m_ignorePage);
    } else if (m_currentPage == kPagePins) {
        RefreshPinsList();
    }
    return 0;
}

LRESULT SettingsWindow::OnIgnoreTabsSelectionChanged(int, LPNMHDR, BOOL &handled) {
    handled = TRUE;
    SetIgnorePage(m_ignoreTabs.GetCurSel());
    return 0;
}

LRESULT SettingsWindow::OnIgnoreListDoubleClick(int, LPNMHDR header, BOOL &handled) {
    if (m_currentPage != kPageIgnore ||
        header == nullptr || header->hwndFrom !=
            m_ignorePageObjects[static_cast<size_t>(m_ignorePage)].ListWindow().m_hWnd) {
        handled = FALSE;
        return 0;
    }
    handled = TRUE;
    if (m_ignorePageObjects[static_cast<size_t>(m_ignorePage)].EditValue()) {
        NotifyOwner(AppConstants::UiUpdate::kIgnoreRules);
    }
    return 0;
}

LRESULT SettingsWindow::OnIgnoreListKeyDown(int, LPNMHDR header, BOOL &handled) {
    if (m_currentPage != kPageIgnore ||
        header == nullptr || header->hwndFrom !=
            m_ignorePageObjects[static_cast<size_t>(m_ignorePage)].ListWindow().m_hWnd) {
        handled = FALSE;
        return 0;
    }
    handled = TRUE;
    const auto *key = reinterpret_cast<const NMLVKEYDOWN *>(header);
    if (key->wVKey == VK_DELETE &&
        m_ignorePageObjects[static_cast<size_t>(m_ignorePage)].RemoveValue()) {
        NotifyOwner(AppConstants::UiUpdate::kIgnoreRules);
    }
    return 0;
}

LRESULT SettingsWindow::OnPinsListDoubleClick(int, LPNMHDR header, BOOL &handled) {
    handled = header != nullptr && header->hwndFrom == m_pList.m_hWnd;
    if (handled) {
        EditSelectedPin();
    }
    return 0;
}

LRESULT SettingsWindow::OnPinsListKeyDown(int, LPNMHDR header, BOOL &handled) {
    handled = header != nullptr && header->hwndFrom == m_pList.m_hWnd;
    if (!handled) {
        return 0;
    }
    const auto *key = reinterpret_cast<const NMLVKEYDOWN *>(header);
    if (key->wVKey == VK_DELETE) {
        DeleteSelectedPin();
    }
    return 0;
}

LRESULT SettingsWindow::OnDestroy(UINT, WPARAM, LPARAM, BOOL &handled) {
    handled = TRUE;
    if (CMessageLoop *loop = _Module.GetMessageLoop(); loop != nullptr) {
        loop->RemoveMessageFilter(this);
    }
    if (m_windowIcon) {
        SetIcon(nullptr, TRUE);
        SetIcon(nullptr, FALSE);
        m_windowIcon.Reset();
    }
    if (!m_ignoreImageList.IsNull()) {
        m_ignoreImageList.Destroy();
    }
    m_hWnd = nullptr;
    return 0;
}

// MainWindow has already run Theme::ApplyToWindow on this window, which
// restyled every control; what is left is the state set once and a repaint
// of every page, tab strip and list.
LRESULT SettingsWindow::OnThemeChanged(UINT, WPARAM, LPARAM, BOOL &handled) {
    handled = TRUE;
    ApplyThemeColors();
    RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    return 0;
}

namespace {

void SetIgnoreListViewColumnWidth(CListViewCtrl list, int width) {
    if (list.m_hWnd != nullptr) {
        list.SetColumnWidth(0, width);
    }
}

}

void IgnorePage::SetValues(std::vector<std::wstring> values) {
    m_values = std::move(values);
    m_persistedValues = m_values;
    Refresh();
}

bool IgnorePage::PersistValues() {
    if (m_storage == nullptr) {
        return false;
    }
    try {
        m_storage->ReplaceList(m_listKind, m_values);
        m_persistedValues = m_values;
        return true;
    } catch (const std::exception &error) {
        m_values = m_persistedValues;
        Refresh();
        ShowSettingsRuntimeError(
            m_pageWindow.m_hWnd,
            error.what(),
            IDS_SETTINGS_IGNORE_SAVE_ERROR
        );
        return false;
    } catch (...) {
        m_values = m_persistedValues;
        Refresh();
        ShowSettingsRuntimeError(
            m_pageWindow.m_hWnd,
            {},
            IDS_SETTINGS_IGNORE_SAVE_ERROR
        );
        return false;
    }
}

void IgnorePage::Initialize(
    CWindow page_window,
    StorageWorker &storage,
    std::vector<std::wstring> values
) {
    m_pageWindow = page_window;
    m_storage = &storage;
    m_list = page_window.GetDlgItem(IDC_I_LIST);
    m_description = page_window.GetDlgItem(IDC_I_DESCRIPTION);
    SetValues(std::move(values));
    if (m_description.m_hWnd != nullptr) {
        m_description.SetWindowText(
            Localization::Text(m_descriptionResource).c_str()
        );
    }
}

void IgnorePage::Show() {
    if (m_pageWindow.m_hWnd != nullptr) {
        m_pageWindow.ShowWindow(SW_SHOW);
        Refresh();
    }
}

void IgnorePage::Hide() {
    if (m_pageWindow.m_hWnd != nullptr) {
        m_pageWindow.ShowWindow(SW_HIDE);
    }
}

void IgnorePage::Refresh() {
    if (m_list.m_hWnd == nullptr) {
        return;
    }

    m_list.DeleteAllItems();
    for (size_t index = 0; index < m_values.size(); ++index) {
        LVITEMW row{};
        row.mask = LVIF_TEXT;
        row.iItem = static_cast<int>(index);
        row.pszText = const_cast<wchar_t *>(m_values[index].c_str());
        m_list.InsertItem(&row);
    }

    RECT list_rect{};
    m_list.GetClientRect(&list_rect);
    SetIgnoreListViewColumnWidth(m_list, list_rect.right - list_rect.left);
}

bool IgnorePage::ChooseFile(std::wstring &value, bool editing) const {
    std::array<wchar_t, MAX_PATH> path{};
    if (editing && wcscpy_s(path.data(), path.size(), value.c_str()) != 0) {
        return false;
    }

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = m_pageWindow.m_hWnd;
    const std::wstring application_filter = Localization::Text(
        IDS_IGNORE_WINDOWS_APPLICATION_FILTER
    );
    const std::wstring all_files_filter = Localization::Text(IDS_IGNORE_ALL_FILES_FILTER);
    const std::wstring filter = application_filter + L'\0' + L"*.exe" + L'\0' +
        all_files_filter + L'\0' + L"*.*" + L'\0' + L'\0';
    dialog.lpstrFilter = filter.c_str();
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&dialog)) {
        return false;
    }

    value = path.data();
    return !value.empty();
}

bool IgnorePage::EditTextValue(std::wstring &value, bool editing) const {
    const std::wstring hint = Localization::Text(
        editing ? m_editHintResource : m_addHintResource
    );
    EditIgnoreDialog dialog(
        editing ? value : L"",
        hint,
        m_dialogPage
    );
    if (dialog.DoModal(m_pageWindow) != IDOK) {
        return false;
    }

    value = dialog.GetValue();
    return !value.empty();
}

bool IgnorePage::IsDuplicate(const std::wstring &value, int selected) const {
    for (size_t index = 0; index < m_values.size(); ++index) {
        if (static_cast<int>(index) == selected || m_values[index] != value) {
            continue;
        }
        if (selected >= 0) {
            const UINT message_resource = m_filePicker
                ? IDS_SETTINGS_DUPLICATE_APPLICATION
                : IDS_SETTINGS_DUPLICATE_VALUE;
            const std::wstring message = Localization::Text(message_resource);
            const std::wstring title = Localization::Text(IDS_SETTINGS_ERROR_TITLE);
            MessageBoxW(
                m_pageWindow.m_hWnd,
                message.c_str(),
                title.c_str(),
                MB_OK | MB_ICONWARNING
            );
        }
        return true;
    }
    return false;
}

void IgnorePage::SelectValue(int index) {
    if (m_list.m_hWnd != nullptr && index >= 0) {
        m_list.SetItemState(
            index,
            LVIS_SELECTED | LVIS_FOCUSED,
            LVIS_SELECTED | LVIS_FOCUSED
        );
    }
}

bool IgnorePage::AddValue() {
    std::wstring value;
    const bool accepted = m_filePicker
        ? ChooseFile(value, false)
        : EditTextValue(value, false);
    if (!accepted || IsDuplicate(value, -1)) {
        return false;
    }

    m_values.push_back(std::move(value));
    const bool saved = SaveList();
    Refresh();
    if (saved) {
        SelectValue(static_cast<int>(m_values.size() - 1));
    }
    return saved;
}

bool IgnorePage::EditValue() {
    const int selected = SelectedListViewItem(m_list);
    if (selected < 0 || selected >= static_cast<int>(m_values.size())) {
        return false;
    }

    std::wstring value = m_values[static_cast<size_t>(selected)];
    const bool edited = m_filePicker
        ? ChooseFile(value, true)
        : EditTextValue(value, true);
    if (!edited || IsDuplicate(value, selected)) {
        return false;
    }

    m_values[static_cast<size_t>(selected)] = std::move(value);
    const bool saved = SaveList();
    Refresh();
    return saved;
}

bool IgnorePage::RemoveValue() {
    const int selected = SelectedListViewItem(m_list);
    if (selected < 0 || selected >= static_cast<int>(m_values.size())) {
        return false;
    }

    m_values.erase(m_values.begin() + selected);
    const bool saved = SaveList();
    Refresh();
    return saved;
}

bool IgnorePage::ResetToDefaults() {
    if (m_listKind != IgnoreListKind::Formats) {
        return false;
    }

    m_values = ClipboardRules::DefaultIgnoredFormats();
    const bool saved = SaveList();
    Refresh();
    return saved;
}

bool IgnorePage::SaveList() {
    return PersistValues();
}
