## WTL Development Guidelines

This project is a native Windows desktop application whose UI is built on **WTL (Windows Template Library)**.

### Core principles

- **Prefer WTL to simplify Win32 development.**
- If WTL can do it, do not hand-write verbose Win32 API wrappers.
- Prefer the window classes, message maps, control wrappers, dialogs, layout, and GDI wrappers that WTL provides.
- Keep the native Win32/WTL architecture; do not bring in a heavyweight UI framework to write less code.
- Do not bring in Qt, MFC, wxWidgets, Electron, WebView, ImGui, or similar frameworks for the sake of "modernization".
- Use the Win32 API directly only for things WTL does not support or cannot reasonably simplify.
- Calling the Win32 API directly is fine when it solves a low-level problem, but avoid re-wrapping capabilities WTL already has in business code.

### Order of technology choices

When implementing Windows UI or window logic, choose in this order:

1. Existing WTL classes and utilities
2. Windows Common Controls
3. Win32 API
4. Custom Win32 wrappers

For example:

```text
Window                → CWindowImpl / CDialogImpl
Dialog                → CDialogImpl
Message handling      → BEGIN_MSG_MAP / MESSAGE_HANDLER / COMMAND_HANDLER
Buttons, edits, etc.  → WTL control classes
Centering a window    → WTL / simple Win32 API
Font                  → CFont
Brush                 → CBrush
Pen                   → CPen
Device context        → CDC / CClientDC
String                → CString
Window handle         → HWND / CWindow
Timer                 → WTL message map + Win32 SetTimer
Tray icon             → WTL's tray support; fall back to Shell_NotifyIcon if it is not enough
```

### Windows

Prefer WTL window classes:

```cpp
class CMainWindow :
    public CWindowImpl<CMainWindow>
{
public:
    DECLARE_WND_CLASS(L"MyApp.MainWindow")

    BEGIN_MSG_MAP(CMainWindow)
        MESSAGE_HANDLER(WM_CREATE, OnCreate)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        COMMAND_HANDLER(IDC_BUTTON, BN_CLICKED, OnButtonClicked)
    END_MSG_MAP()

private:
    LRESULT OnCreate(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnButtonClicked(WORD, WORD, HWND, BOOL&);
};
```

Do not write the window procedure as a hand-written `WndProc` unless there is a clear technical reason.

Not recommended:

```cpp
LRESULT CALLBACK WndProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (message) {
        ...
    }
}
```

If `CWindowImpl` can solve the problem, do not fall back to a hand-written `WndProc`.

### Message handling

Prefer WTL message maps.

Recommended:

```cpp
BEGIN_MSG_MAP(CMainWindow)
    MESSAGE_HANDLER(WM_PAINT, OnPaint)
    MESSAGE_HANDLER(WM_CLOSE, OnClose)
    COMMAND_HANDLER(IDC_OK, BN_CLICKED, OnOk)
END_MSG_MAP()
```

Avoid handling every window message in one large `switch (message)`.

Keep message handlers short:

```cpp
LRESULT OnOk(WORD, WORD, HWND, BOOL&)
{
    SaveSettings();
    CloseDialog(IDOK);
    return 0;
}
```

Put complex business logic in separate functions or business classes instead of piling it into message handlers.

### Dialogs

Prefer:

```cpp
CDialogImpl<T>
```

together with:

```cpp
DoModal()
Create()
DestroyWindow()
EndDialog()
```

Do not create a full window class, register it, and run your own message loop just for a simple settings window.

For example:

```cpp
class CSettingsDialog :
    public CDialogImpl<CSettingsDialog>
{
public:
    enum { IDD = IDD_SETTINGS };

    BEGIN_MSG_MAP(CSettingsDialog)
        COMMAND_ID_HANDLER(IDOK, OnOk)
        COMMAND_ID_HANDLER(IDCANCEL, OnCancel)
    END_MSG_MAP();

    LRESULT OnOk(WORD, WORD, HWND, BOOL&)
    {
        SaveSettings();
        EndDialog(IDOK);
        return 0;
    }

    LRESULT OnCancel(WORD, WORD, HWND, BOOL&)
    {
        EndDialog(IDCANCEL);
        return 0;
    }
};
```

### Working with controls

Prefer WTL control wrappers over calling these directly everywhere:

```cpp
GetDlgItem()
SendMessage()
```

For example:

```cpp
CEdit edit = GetDlgItem(IDC_EDIT_NAME);
edit.SetWindowText(L"Hello");
```

instead of:

```cpp
HWND hwnd = GetDlgItem(m_hWnd, IDC_EDIT_NAME);
SetWindowText(hwnd, L"Hello");
```

For lists, trees, tabs, ListView, and other Common Controls, prefer the matching WTL wrappers:

```cpp
CListViewCtrl
CTreeViewCtrl
CTabCtrl
CComboBox
CEdit
CButton
CStatic
```

### Strings

For Windows UI strings, prefer:

```cpp
CString
```

Avoid mixing these in the UI layer:

```cpp
std::wstring
std::string
LPCWSTR
LPWSTR
```

If the business layer already uses `std::wstring`, there is no need to convert the whole project for WTL's sake.

WTL and the STL can coexist.

The principle is:

```text
UI layer       → CString / WTL
Business layer → STL
Windows API    → HWND / LPCWSTR, etc.
```

Do not add pointless string conversions for the sake of "consistency".

### GDI

Prefer WTL's GDI wrappers:

```cpp
CDC
CClientDC
CPaintDC
CBrush
CPen
CFont
CBitmap
```

For example:

```cpp
CPaintDC dc(m_hWnd);

CFont font;
font.CreatePointFont(90, L"Segoe UI");

dc.SelectFont(font);
dc.DrawText(
    L"Hello",
    -1,
    &rect,
    DT_CENTER | DT_VCENTER | DT_SINGLELINE
);
```

Do not create your own GDI wrapper layer for simple drawing.

### Layout

Prefer WTL's layout support.

For simple, fixed-size windows:

```text
Resource editor / dialog template
```

is preferred over creating every control by hand at runtime.

For windows that need dynamic layout:

```text
WTL Layout / AtlCtrlW
```

is preferred over lots of:

```cpp
SetWindowPos(...)
MoveWindow(...)
```

If you must use `SetWindowPos`, handle layout in one place; do not let every control's event handler move other controls.

### Resource files

Prefer `.rc` resources for UI:

```text
IDD_SETTINGS
IDD_HISTORY
IDC_EDIT_SEARCH
IDC_LIST_HISTORY
IDC_BUTTON_CLEAR
```

Do not create lots of static UI controls by hand in C++.

Prefer:

```text
Resource template
    ↓
WTL Dialog
    ↓
WTL control wrappers
```

over:

```text
Create the window in C++
    ↓
CreateWindowEx
    ↓
Set the font
    ↓
Set the position
    ↓
Set the style
    ↓
Handle messages
```

### When to use the Win32 API

The Win32 API is not forbidden.

Use it directly when WTL has no equivalent, or when calling the Win32 API directly is simpler.

For example:

```cpp
RegisterHotKey()
UnregisterHotKey()
Shell_NotifyIcon()
GetWindowLongPtr()
SetWindowLongPtr()
SetTimer()
KillTimer()
OpenProcess()
CreateProcess()
```

But do not bypass WTL just because a Win32 API exists.

Decision rule:

```text
Can WTL solve it directly?
    ↓
Yes → use WTL
    ↓
No
    ↓
Can the Win32 API solve it simply?
    ↓
Yes → use the Win32 API
    ↓
No
    ↓
Only then consider a custom wrapper
```

### Avoid redundant wrappers

Before adding a Helper, Wrapper, or Manager, check whether WTL already provides the capability.

For example, do not casually create:

```cpp
WindowHelper
DialogHelper
ControlHelper
StringHelper
GdiHelper
MessageHelper
```

If it only wraps WTL or the Win32 API in another layer without reducing complexity, do not create it.

Prefer using these directly:

```cpp
CWindow
CDialogImpl
CWindowImpl
CString
CDC
CFont
CBrush
CListViewCtrl
```

### Code size

Prefer the implementation with **the least code, the fewest dependencies, and the clearest lifetimes**.

If two approaches do the same thing:

```text
Approach A: 100 lines of Win32 boilerplate
Approach B: 30 lines of WTL
```

choose WTL.

If:

```text
Approach A: 20 lines of WTL + complex template code
Approach B: 10 lines of direct Win32 API
```

choose the simpler approach.

The goal is not "all code must be WTL", but:

> **Use WTL first to eliminate Win32 boilerplate, while keeping a native Windows architecture.**

### Changing existing code

When changing UI:

1. First confirm which WTL base class the window uses.
2. Check the existing message map.
3. Prefer reusing existing WTL controls.
4. Prefer changing the `.rc` resources over re-creating the UI in C++.
5. Do not rewrite a whole window for a small feature.
6. Do not bring in a new UI framework.
7. Do not replace simple WTL code with a more complex custom wrapper.

### Agent behavior

Before implementing a Windows UI feature, an agent should first ask:

```text
Has WTL already solved this problem?
```

If the answer is "yes", use WTL directly.

If the answer is "not sure", check the project's existing code and the WTL API first, then decide whether to call the lower-level Win32 API.

Do not default to:

```cpp
CreateWindowEx
SetWindowLongPtr
SendMessage
GetDlgItem
MoveWindow
WndProc
```

These APIs are low-level capabilities, not the default way to build the UI layer.

### Final goal

This project aims for:

```text
Native Win32 performance
        +
WTL's lightweight wrappers
        +
Resource-driven UI
        +
Little C++ code
        +
Clear message maps
        +
Few dependencies
```

**Never write ten lines of Win32 for a problem WTL solves in one.**
