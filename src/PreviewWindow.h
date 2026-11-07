#pragma once

#include "PlatformConfig.h"
#include "Constants.h"

#include <atlbase.h>
#include <atlapp.h>
#include <atlctrls.h>
#include <atlgdi.h>
#include <atlwin.h>

#include <array>
#include <string>

#include "PreviewDecoder.h"
#include "resource.h"

class PreviewWindow : public CDialogImpl<PreviewWindow> {
public:
    enum { IDD = IDD_PREVIEW };

    PreviewWindow();

    BEGIN_MSG_MAP(PreviewWindow)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        MESSAGE_HANDLER(WM_DPICHANGED, OnDpiChanged)
        MESSAGE_HANDLER(WM_CLOSE, OnClose)
        MESSAGE_HANDLER(WM_DRAWITEM, OnDrawItem)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        COMMAND_HANDLER(IDC_PREVIEW_PIN, BN_CLICKED, OnCommand)
        COMMAND_HANDLER(IDC_PREVIEW_DELETE, BN_CLICKED, OnCommand)
        MESSAGE_HANDLER(WM_CTLCOLOREDIT, OnControlColor)
        MESSAGE_HANDLER(WM_CTLCOLORSTATIC, OnControlColor)
        MESSAGE_HANDLER(WM_CTLCOLORDLG, OnControlColor)
        MESSAGE_HANDLER(WM_CTLCOLORBTN, OnControlColor)
        MESSAGE_HANDLER(AppConstants::kThemeChangedMessage, OnThemeChanged)
    ALT_MSG_MAP(1)
        MESSAGE_HANDLER(WM_GETDLGCODE, OnActionButtonDialogCode)
        MESSAGE_HANDLER(WM_SETFOCUS, OnActionButtonFocus)
        MESSAGE_HANDLER(WM_KEYDOWN, OnActionButtonKey)
        MESSAGE_HANDLER(WM_KEYUP, OnActionButtonKey)
        MESSAGE_HANDLER(WM_CHAR, OnActionButtonKey)
    END_MSG_MAP()

    bool Initialize(HWND owner);
    void SetItem(const ClipboardItem &item, std::wstring text, PreviewBitmap bitmap = {});
    void GetImageSize(UINT &width, UINT &height) const noexcept;
    void PositionNextToOwner(HWND owner, bool show);
    void Hide();
    bool IsVisible() const noexcept;
    HWND Window() const noexcept { return m_hWnd; }

private:
    static constexpr size_t kControlCount = 5;

    void ClearBitmap();
    bool UpdateFont(UINT dpi);
    std::array<CWindow, kControlCount> Controls() const noexcept;
    void LayoutControls(UINT dpi);
    void UpdateStatus(const ClipboardItem &item);

    LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL &handled);
    LRESULT OnDpiChanged(UINT, WPARAM, LPARAM, BOOL &handled);
    LRESULT OnClose(UINT, WPARAM, LPARAM, BOOL &handled);
    LRESULT OnDrawItem(UINT, WPARAM, LPARAM lParam, BOOL &handled);
    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL &handled);
    LRESULT OnCommand(WORD, WORD id, HWND, BOOL &handled);
    LRESULT OnControlColor(UINT, WPARAM wParam, LPARAM lParam, BOOL &handled);
    LRESULT OnThemeChanged(UINT, WPARAM, LPARAM, BOOL &handled);
    LRESULT OnActionButtonDialogCode(UINT, WPARAM, LPARAM, BOOL &handled);
    LRESULT OnActionButtonFocus(UINT, WPARAM, LPARAM, BOOL &handled);
    LRESULT OnActionButtonKey(UINT, WPARAM, LPARAM, BOOL &handled);

    CStatic m_image;
    CEdit m_text;
    CStatic m_status;
    CContainedWindowT<CButton> m_pinButton;
    CContainedWindowT<CButton> m_deleteButton;
    CBitmap m_bitmap;
    CFont m_font;
    // Control rectangles as the dialog template placed them at m_templateDpi.
    // DPI changes rescale from these, so moving between monitors never
    // accumulates rounding error.
    std::array<RECT, kControlCount> m_templateRects{};
    UINT m_templateDpi = USER_DEFAULT_SCREEN_DPI;
    int m_bitmapWidth = 0;
    int m_bitmapHeight = 0;
    sqlite3_int64 m_itemId = 0;
};
