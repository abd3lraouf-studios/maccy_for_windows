#pragma once

#include "PlatformConfig.h"

#include <atlbase.h>
#include <atlapp.h>
#include <atlctrls.h>
#include <atlwin.h>

#include "Settings.h"

// The theme picker: one card per ThemeFamily, in enum order, each a miniature
// of the history popup painted in that theme's palette, so the choice is made
// by looking rather than by name. Cards show the variant the current Mode
// gives (Theme::Current().dark); the page around them uses the page palette.
//
// It subclasses a WS_TABSTOP | SS_NOTIFY static from the page template. Arrow
// keys, Home and End move the focus between cards, Space or Enter picks the
// focused one, a click picks the card under the mouse. A pick the user makes
// is reported to the parent as WM_COMMAND(MAKEWPARAM(id, kSelectionChanged)).
class ThemeGallery : public CWindowImpl<ThemeGallery, CStatic> {
public:
    // Not STN_DBLCLK, although it shares the value: the static's own mouse
    // handling never runs, so every code with this ID comes from a pick.
    static constexpr WORD kSelectionChanged = 1;

    BEGIN_MSG_MAP(ThemeGallery)
        MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBackground)
        MESSAGE_HANDLER(WM_PAINT, OnPaint)
        MESSAGE_HANDLER(WM_SIZE, OnSize)
        MESSAGE_HANDLER(WM_GETDLGCODE, OnGetDlgCode)
        MESSAGE_HANDLER(WM_KEYDOWN, OnKeyDown)
        MESSAGE_HANDLER(WM_LBUTTONDOWN, OnButtonDown)
        MESSAGE_HANDLER(WM_LBUTTONDBLCLK, OnButtonDown)
        MESSAGE_HANDLER(WM_LBUTTONUP, OnButtonUp)
        MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
        MESSAGE_HANDLER(WM_MOUSELEAVE, OnMouseLeave)
        MESSAGE_HANDLER(WM_SETFOCUS, OnFocusChanged)
        MESSAGE_HANDLER(WM_KILLFOCUS, OnFocusChanged)
        MESSAGE_HANDLER(WM_UPDATEUISTATE, OnUpdateUiState)
    END_MSG_MAP()

    void Attach(HWND window);
    // Shows the saved theme; does not notify the parent.
    void SetSelection(ThemeFamily family);
    ThemeFamily Selection() const noexcept { return static_cast<ThemeFamily>(m_selected); }

private:
    LRESULT OnEraseBackground(UINT, WPARAM, LPARAM, BOOL &handled);
    LRESULT OnPaint(UINT, WPARAM, LPARAM, BOOL &handled);
    LRESULT OnSize(UINT, WPARAM, LPARAM, BOOL &handled);
    LRESULT OnGetDlgCode(UINT, WPARAM, LPARAM lParam, BOOL &handled);
    LRESULT OnKeyDown(UINT, WPARAM key, LPARAM, BOOL &handled);
    LRESULT OnButtonDown(UINT, WPARAM, LPARAM lParam, BOOL &handled);
    LRESULT OnButtonUp(UINT, WPARAM, LPARAM, BOOL &handled);
    LRESULT OnMouseMove(UINT, WPARAM, LPARAM lParam, BOOL &handled);
    LRESULT OnMouseLeave(UINT, WPARAM, LPARAM, BOOL &handled);
    LRESULT OnFocusChanged(UINT message, WPARAM, LPARAM, BOOL &handled);
    LRESULT OnUpdateUiState(UINT message, WPARAM wParam, LPARAM lParam, BOOL &handled);

    int HitTest(POINT point) const;
    void Pick(int index);

    int m_selected = 0;  // the saved theme
    int m_focused = 0;   // the card keyboard input acts on
    int m_hot = -1;      // the card under the mouse
    bool m_trackingMouse = false;
};
