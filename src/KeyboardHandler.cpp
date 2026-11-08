#include "KeyboardHandler.h"
#include "Constants.h"
#include "resource.h"

#include <algorithm>
#include <array>
#include <future>
#include <imm.h>
#include <utility>
#include <windowsx.h>

std::atomic<bool> KeyboardHandler::s_hotkeyCaptureActive{false};

namespace {

// Everything the low-level hook reads or writes. It lives on the hook thread's
// stack and is only touched there, so no field needs synchronisation; the
// open chord and owner window are copied in at install time.
struct HookState {
    HotKeyConfig openHotkey{};
    HWND owner = nullptr;
    bool suppressingOpenKey = false;
    UINT suppressingCaptureKey = 0;
    HWND captureTargetWindow = nullptr;
    UINT capturedKey = 0;
    UINT capturedModifiers = 0;
    bool capturedKeyReleased = false;
    UINT pendingWinKey = 0;
    DWORD pendingWinScan = 0;
    DWORD pendingWinFlags = 0;
    bool winKeyPassedThrough = false;
    bool suppressingWinKey = false;
};

// Windows calls a WH_KEYBOARD_LL procedure on the thread that installed it,
// so a thread_local pointer reaches exactly that thread's state.
thread_local HookState* t_hookState = nullptr;

// Only one handler may own the global hook; a second one would double-post
// the open message for every chord.
std::atomic<const KeyboardHandler*> g_hookOwner{nullptr};

bool AsyncKeyDown(int virtual_key) {
    return (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
}

// The hook thread owns no windows, so GetFocus() there is always null. The
// settings hotkey edit lives on the UI thread, which is the foreground thread
// while the user is capturing a chord.
HWND ForegroundFocusWindow() {
    const HWND foreground = GetForegroundWindow();
    GUITHREADINFO info{sizeof(info)};
    const DWORD thread = foreground != nullptr ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    if (thread != 0 && GetGUIThreadInfo(thread, &info) && info.hwndFocus != nullptr) {
        return info.hwndFocus;
    }
    return foreground;
}

void ReplayPendingWin(const HookState& state, UINT virtual_key, bool key_up) {
    // Replay the delayed Win key with original scan/extended flags (H-07).
    INPUT replay{};
    replay.type = INPUT_KEYBOARD;
    replay.ki.wVk = static_cast<WORD>(virtual_key);
    replay.ki.wScan = static_cast<WORD>(state.pendingWinScan);
    if ((state.pendingWinFlags & LLKHF_EXTENDED) != 0) {
        replay.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    if (key_up) {
        replay.ki.dwFlags |= KEYEVENTF_KEYUP;
    }
    SendInput(1, &replay, sizeof(replay));
}

void PostCapturedHotkey(HookState& state) {
    if (IsWindow(state.captureTargetWindow)) {
        PostMessageW(state.captureTargetWindow, AppConstants::kCaptureHotkeyMessage,
                     static_cast<WPARAM>(state.capturedKey),
                     static_cast<LPARAM>(state.capturedModifiers));
    }
    state.captureTargetWindow = nullptr;
    state.capturedKey = 0;
    state.capturedKeyReleased = false;
}

LRESULT CALLBACK OpenHotkeyHookProc(int code, WPARAM message, LPARAM data) {
    HookState* const state_pointer = t_hookState;
    if (code < 0 || state_pointer == nullptr) {
        return CallNextHookEx(nullptr, code, message, data);
    }
    HookState& state = *state_pointer;
    const auto* const key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
    const bool keyDown = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool keyUp = message == WM_KEYUP || message == WM_SYSKEYUP;
    if ((key->flags & LLKHF_INJECTED) != 0) {
        return CallNextHookEx(nullptr, code, message, data);
    }

    const bool isWinKey = key->vkCode == VK_LWIN || key->vkCode == VK_RWIN;
    const bool isModifierKey = isWinKey || key->vkCode == VK_SHIFT || key->vkCode == VK_LSHIFT ||
        key->vkCode == VK_RSHIFT || key->vkCode == VK_CONTROL || key->vkCode == VK_LCONTROL ||
        key->vkCode == VK_RCONTROL || key->vkCode == VK_MENU || key->vkCode == VK_LMENU ||
        key->vkCode == VK_RMENU;
    // Prefer an explicit capture flag from the settings hotkey edit (M-15).
    const bool capturingHotkey = KeyboardHandler::IsHotkeyCaptureActive();

    if (state.suppressingCaptureKey == key->vkCode && (keyDown || keyUp)) {
        if (keyUp) {
            state.suppressingCaptureKey = 0;
            state.capturedKeyReleased = true;
            if (!state.suppressingWinKey) {
                PostCapturedHotkey(state);
            }
        }
        return 1;
    }

    if (capturingHotkey && keyDown && !isModifierKey) {
        UINT modifiers = 0;
        if (AsyncKeyDown(VK_MENU)) modifiers |= MOD_ALT;
        if (AsyncKeyDown(VK_CONTROL)) modifiers |= MOD_CONTROL;
        if (AsyncKeyDown(VK_SHIFT)) modifiers |= MOD_SHIFT;
        if (state.pendingWinKey != 0 || AsyncKeyDown(VK_LWIN) || AsyncKeyDown(VK_RWIN)) {
            modifiers |= MOD_WIN;
        }
        if (modifiers == 0 && key->vkCode != VK_BACK) {
            return CallNextHookEx(nullptr, code, message, data);
        }
        state.suppressingCaptureKey = key->vkCode;
        state.captureTargetWindow = ForegroundFocusWindow();
        state.capturedKey = key->vkCode;
        state.capturedModifiers = modifiers;
        state.capturedKeyReleased = false;
        if (state.pendingWinKey != 0) {
            state.suppressingWinKey = true;
        }
        return 1;
    }

    const UINT configuredModifiers = state.openHotkey.modifiers &
        (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN);
    if (!capturingHotkey && !state.suppressingOpenKey && keyDown && !isModifierKey &&
        (configuredModifiers & MOD_WIN) == 0 &&
        key->vkCode == state.openHotkey.virtual_key) {
        UINT pressed = 0;
        if (AsyncKeyDown(VK_MENU)) pressed |= MOD_ALT;
        if (AsyncKeyDown(VK_CONTROL)) pressed |= MOD_CONTROL;
        if (AsyncKeyDown(VK_SHIFT)) pressed |= MOD_SHIFT;
        if (pressed == configuredModifiers) {
            state.suppressingOpenKey = true;
            PostMessageW(state.owner, AppConstants::kOpenHotkeyMessage, 0,
                         reinterpret_cast<LPARAM>(GetForegroundWindow()));
            return 1;
        }
    }

    if (state.suppressingOpenKey &&
        key->vkCode == state.openHotkey.virtual_key && (keyDown || keyUp)) {
        if (keyUp) {
            state.suppressingOpenKey = false;
        }
        return 1;
    }

    if (isWinKey) {
        if ((configuredModifiers & MOD_WIN) == 0 && !capturingHotkey) {
            return CallNextHookEx(nullptr, code, message, data);
        }
        if (keyDown) {
            if (state.suppressingWinKey || state.pendingWinKey == key->vkCode) {
                return 1;
            }
            if (state.winKeyPassedThrough) {
                return CallNextHookEx(nullptr, code, message, data);
            }
            state.pendingWinKey = key->vkCode;
            state.pendingWinScan = key->scanCode;
            state.pendingWinFlags = key->flags;
            return 1;
        }
        if (keyUp) {
            if (state.suppressingWinKey) {
                state.suppressingWinKey = false;
                state.pendingWinKey = 0;
                state.pendingWinScan = 0;
                state.pendingWinFlags = 0;
                if (state.capturedKeyReleased) {
                    PostCapturedHotkey(state);
                }
                return 1;
            }
            if (state.pendingWinKey == key->vkCode) {
                // A lone Win tap. Windows records this key-up in its raw key state before
                // any low-level hook runs, so the Win-down replayed here lands after it and
                // leaves Win held there. Explorer's Game Bar shortcuts (Win+G, Win+Alt+G/R/B/M/T/W)
                // read that state, so plain G, Alt+G and the rest stopped typing in every app
                // until the next Win+key. Close the replayed down once the rest of the chain
                // has seen the real key-up.
                ReplayPendingWin(state, key->vkCode, false);
                const LRESULT result = CallNextHookEx(nullptr, code, message, data);
                ReplayPendingWin(state, key->vkCode, true);
                state.pendingWinKey = 0;
                state.pendingWinScan = 0;
                state.pendingWinFlags = 0;
                return result;
            }
            if (state.winKeyPassedThrough) {
                state.winKeyPassedThrough = false;
            }
        }
        return CallNextHookEx(nullptr, code, message, data);
    }

    if (state.winKeyPassedThrough) {
        return CallNextHookEx(nullptr, code, message, data);
    }

    if (state.pendingWinKey != 0 && isModifierKey) {
        return CallNextHookEx(nullptr, code, message, data);
    }

    if (!capturingHotkey && !state.suppressingOpenKey && keyDown &&
        state.pendingWinKey != 0 &&
        key->vkCode == state.openHotkey.virtual_key) {
        UINT pressed = MOD_WIN;
        if (AsyncKeyDown(VK_MENU)) pressed |= MOD_ALT;
        if (AsyncKeyDown(VK_CONTROL)) pressed |= MOD_CONTROL;
        if (AsyncKeyDown(VK_SHIFT)) pressed |= MOD_SHIFT;
        if (pressed == configuredModifiers && state.owner != nullptr) {
            // Toggle on key-down. suppressingOpenKey swallows auto-repeat until
            // this V is released, so a second press while Win is held toggles again.
            state.suppressingOpenKey = true;
            state.suppressingWinKey = true;
            PostMessageW(state.owner, AppConstants::kOpenHotkeyMessage, 0,
                         reinterpret_cast<LPARAM>(GetForegroundWindow()));
            return 1;
        }
    }

    if (keyDown && state.pendingWinKey != 0) {
        // Let ordinary Win+key shortcuts pass through after replaying the delayed Win-down.
        // The real Win key-up arrives later and closes it.
        ReplayPendingWin(state, state.pendingWinKey, false);
        state.pendingWinKey = 0;
        state.pendingWinScan = 0;
        state.pendingWinFlags = 0;
        state.winKeyPassedThrough = true;
    }

    return CallNextHookEx(nullptr, code, message, data);
}

// Windows silently removes a low-level hook whose thread does not answer
// within LowLevelHooksTimeout (about 1 s) and never tells the app. The UI
// thread blocks on clipboard capture and storage round-trips, so the hook gets
// a thread that does nothing but pump messages for it.
void RunHookThread(HookState state, std::promise<bool> ready) {
    MSG message{};
    // Create the message queue before reporting ready, so the WM_QUIT that
    // UninstallOpenHotkeyHook posts can never arrive before the queue exists.
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    // The chord is time-critical and the thread is idle in GetMessage otherwise.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    t_hookState = &state;
    const HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, OpenHotkeyHookProc,
                                         GetModuleHandleW(nullptr), 0);
    ready.set_value(hook != nullptr);
    if (hook != nullptr) {
        // Hook callbacks are delivered while this thread waits in GetMessage.
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        }
        UnhookWindowsHookEx(hook);
    }
    t_hookState = nullptr;
}

} // namespace

void KeyboardHandler::SetHotkeyCaptureActive(bool active) noexcept {
    s_hotkeyCaptureActive.store(active, std::memory_order_release);
}

bool KeyboardHandler::IsHotkeyCaptureActive() noexcept {
    return s_hotkeyCaptureActive.load(std::memory_order_acquire);
}

KeyboardHandler::KeyboardHandler(AppSettings& settings)
    : m_settings(settings) {}

KeyboardHandler::~KeyboardHandler() {
    Shutdown();
}

bool KeyboardHandler::Initialize(CWindow owner, CEdit search, CListBox historyList,
                                 CListBox pinsList,
                                 const std::array<CButton, AppConstants::UI::kFooterButtonCount>& footerButtons) {
    m_owner = owner;
    m_search = search;
    m_historyList = historyList;
    m_pinsList = pinsList;
    m_footerButtons = footerButtons;
    return true;
}

void KeyboardHandler::Shutdown() {
    UninstallOpenHotkeyHook(AppConstants::HotKey::kOpenPopup);
    SetHotkeyCaptureActive(false);
    m_callbacks = {};
}

bool KeyboardHandler::InstallOpenHotkeyHook(UINT hotkeyId) {
    UninstallOpenHotkeyHook(hotkeyId);
    const KeyboardHandler* expected = nullptr;
    if (!g_hookOwner.compare_exchange_strong(expected, this)) {
        return false;
    }

    HookState state;
    state.openHotkey = m_settings.open_hotkey;
    state.owner = m_owner.m_hWnd;
    std::promise<bool> ready;
    std::future<bool> installed = ready.get_future();
    m_hookThread = std::thread(RunHookThread, state, std::move(ready));
    m_hookThreadId = GetThreadId(m_hookThread.native_handle());
    m_hotkeyRegistered = installed.get();
    if (!m_hotkeyRegistered) {
        m_hookThread.join();
        m_hookThreadId = 0;
        g_hookOwner.store(nullptr);
    }
    return m_hotkeyRegistered;
}

void KeyboardHandler::UninstallOpenHotkeyHook(UINT) {
    if (m_hookThread.joinable()) {
        PostThreadMessageW(m_hookThreadId, WM_QUIT, 0, 0);
        m_hookThread.join();
        m_hookThreadId = 0;
        g_hookOwner.store(nullptr);
    }
    m_hotkeyRegistered = false;
}

bool KeyboardHandler::IsComposing(CWindow window) const {
    HIMC context = ImmGetContext(window.m_hWnd);
    const bool composing = context && ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0) > 0;
    if (context) ImmReleaseContext(window.m_hWnd, context);
    return m_imeComposing || composing;
}

bool KeyboardHandler::MouseCanSelect() {
    POINT position{};
    GetCursorPos(&position);
    if (m_keyboardNavigating && position.x == m_keyboardPointer.x && position.y == m_keyboardPointer.y) {
        return false;
    }
    m_keyboardNavigating = false;
    return true;
}

void KeyboardHandler::TypeToSearch(WPARAM character, CEdit& search) {
    if (!m_settings.show_search) return;
    search.SetFocus();
    const wchar_t text[] = {static_cast<wchar_t>(character), 0};
    search.ReplaceSel(text);
}

void KeyboardHandler::FocusSearchOrPopup(CEdit& search, CWindow mainWindow, bool searchVisible) {
    if (searchVisible) {
        search.SetFocus();
    } else {
        mainWindow.SetFocus();
    }
}

void KeyboardHandler::SetActiveFooter(int index, const std::vector<ClipboardItem>& items) {
    if (index < 0 || index >= static_cast<int>(m_footerButtons.size()) ||
        (m_activeFooter == index && m_activeItemIndex < 0)) {
        return;
    }

    const int previousItem = m_activeItemIndex;
    m_activeFooter = index;
    m_activeItemIndex = -1;
    m_activeItemId = 0;
    ClearHistoryHover();
    SetListSelection(m_historyList, -1);
    SetListSelection(m_pinsList, -1);
    InvalidateHistoryItem(previousItem, items, m_historyList, m_pinsList);
    InvalidateFooterButtons();
}

CListBox KeyboardHandler::ListForItem(int index, const std::vector<ClipboardItem>& items,
                                      CListBox historyList, CListBox pinsList) const {
    if (index < 0 || static_cast<size_t>(index) >= items.size()) return CListBox();
    return items[static_cast<size_t>(index)].pinned ? pinsList : historyList;
}

int KeyboardHandler::RowForItem(int index, const std::vector<ClipboardItem>& items,
                                CListBox historyList, CListBox pinsList) const {
    const CListBox list = ListForItem(index, items, historyList, pinsList);
    if (list.m_hWnd == nullptr) return -1;
    const int count = list.GetCount();
    for (int row = 0; row < count; ++row) {
        if (ItemIndexAtRow(list, row, items) == index) return row;
    }
    return -1;
}

int KeyboardHandler::ItemIndexAtRow(CListBox list, int row,
                                    const std::vector<ClipboardItem>& items) const {
    if (list.m_hWnd == nullptr || row < 0) return -1;
    const DWORD_PTR value = list.GetItemData(row);
    if (value == LB_ERR || static_cast<size_t>(value) >= items.size()) return -1;
    return static_cast<int>(value);
}

int KeyboardHandler::HistoryItemAtPoint(CListBox window, POINT point,
                                        CListBox historyList, CListBox pinsList,
                                        const std::vector<ClipboardItem>& items) const {
    if (window.m_hWnd != historyList.m_hWnd && window.m_hWnd != pinsList.m_hWnd) return -1;
    BOOL outside = FALSE;
    const UINT row = window.ItemFromPoint(point, outside);
    if (outside) return -1;
    return ItemIndexAtRow(window, static_cast<int>(row), items);
}

void KeyboardHandler::InvalidateHistoryItem(int index, const std::vector<ClipboardItem>& items,
                                            CListBox historyList, CListBox pinsList) {
    const int row = RowForItem(index, items, historyList, pinsList);
    if (row < 0) return;
    RECT rect{};
    CListBox list = ListForItem(index, items, historyList, pinsList);
    if (list.m_hWnd != nullptr && list.GetItemRect(row, &rect) != LB_ERR) {
        list.InvalidateRect(&rect, FALSE);
    }
}

void KeyboardHandler::SetListSelection(CListBox list, int row) const {
    if (list.m_hWnd == nullptr) return;
    list.SetCurSel(row);
}

void KeyboardHandler::InvalidateFooterButtons() const {
    for (CButton button : m_footerButtons) {
        if (button.m_hWnd != nullptr) {
            button.InvalidateRect(nullptr, FALSE);
        }
    }
}

void KeyboardHandler::SetActiveHistoryItem(int index, const std::vector<ClipboardItem>& items,
                                           CListBox historyList, CListBox pinsList,
                                           bool scrollIntoView) {
    if (index < 0 || static_cast<size_t>(index) >= items.size()) return;
    const int previous = m_activeItemIndex;
    const int previousFooter = m_activeFooter;
    m_activeItemIndex = index;
    m_activeItemId = items[index].id;
    m_activeFooter = -1;

    if (scrollIntoView) {
        SetListSelection(historyList, -1);
        SetListSelection(pinsList, -1);
        CListBox list = ListForItem(index, items, historyList, pinsList);
        if (list.m_hWnd != nullptr) {
            SetListSelection(list, RowForItem(index, items, historyList, pinsList));
        }
    }

    InvalidateHistoryItem(previous, items, historyList, pinsList);
    InvalidateHistoryItem(index, items, historyList, pinsList);
    if (previousFooter >= 0) {
        InvalidateFooterButtons();
    }
}

void KeyboardHandler::BeginHistoryMouseTracking() {
    TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, m_hoverList.m_hWnd, 0};
    ::TrackMouseEvent(&tracking);
}

void KeyboardHandler::OnHistoryMouseMove(CListBox window, POINT point,
                                        const std::vector<ClipboardItem>& items,
                                        CListBox historyList, CListBox pinsList,
                                        bool popupVisible) {
    if (!MouseCanSelect()) return;
    if (!popupVisible || (window.m_hWnd != historyList.m_hWnd && window.m_hWnd != pinsList.m_hWnd)) {
        return;
    }

    m_hoverList = window;
    BeginHistoryMouseTracking();

    const int index = HistoryItemAtPoint(window, point, historyList, pinsList, items);
    if (index < 0) {
        ClearHistoryHover();
        return;
    }

    const sqlite3_int64 item_id = items[static_cast<size_t>(index)].id;
    if (index == m_hoveredItemIndex && item_id == m_hoveredItemId) {
        if (m_activeFooter >= 0 || m_activeItemIndex != index) {
            SetActiveHistoryItem(index, items, historyList, pinsList, false);
            if (m_callbacks.preview) {
                m_callbacks.preview(item_id, false);
            }
        }
        return;
    }

    const int previous_index = m_hoveredItemIndex;
    m_hoveredItemIndex = index;
    m_hoveredItemId = item_id;
    SetActiveHistoryItem(index, items, historyList, pinsList, false);

    InvalidateHistoryItem(previous_index, items, historyList, pinsList);
    InvalidateHistoryItem(index, items, historyList, pinsList);

    if (m_callbacks.preview) {
                m_callbacks.preview(item_id, false);
            }
}

void KeyboardHandler::OnHistoryMouseLeave() {
    ClearHistoryHover();
}

void KeyboardHandler::UpdateHistoryHoverFromCursor(const std::vector<ClipboardItem>& items,
                                                   CListBox historyList, CListBox pinsList,
                                                   bool popupVisible) {
    if (!popupVisible) return;
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    for (const CListBox& list : {historyList, pinsList}) {
        if (!list.IsWindowVisible()) continue;
        POINT point = cursor;
        list.ScreenToClient(&point);
        RECT rect{};
        list.GetClientRect(&rect);
        if (::PtInRect(&rect, point)) {
            OnHistoryMouseMove(list, point, items, historyList, pinsList, popupVisible);
            return;
        }
    }
    ClearHistoryHover();
}

void KeyboardHandler::ClearHistoryHover() {
    m_hoveredItemIndex = -1;
    m_hoveredItemId = 0;
    if (m_owner.m_hWnd != nullptr) {
        KillTimer(m_owner.m_hWnd, AppConstants::Timer::kPreview);
    }
}

void KeyboardHandler::NavigateHistoryFromSearch(bool forward,
                                               const std::vector<ClipboardItem>& items,
                                               CListBox historyList, CListBox pinsList,
                                               bool showFooter) {
    m_keyboardNavigating = true;
    GetCursorPos(&m_keyboardPointer);
    ClearHistoryHover();

    const int count = static_cast<int>(items.size());
    const int total = count + (showFooter ? AppConstants::UI::kFooterButtonCount : 0);
    if (!total) return;

    int current = m_activeFooter >= 0 ? count + m_activeFooter : m_activeItemIndex;
    const int target = std::clamp(current + (forward ? 1 : -1), 0, total - 1);

    if (target >= count) {
        SetActiveFooter(target - count, items);
    } else {
        SetActiveHistoryItem(target, items, historyList, pinsList);
        if (m_callbacks.preview) {
                m_callbacks.preview(m_activeItemId, true);
            }
    }
}

bool KeyboardHandler::HandlePopupKey(WPARAM key, const CEdit& search, const std::vector<ClipboardItem>& items) {
    if (m_callbacks.search && search.m_hWnd != nullptr &&
        (key == VK_RETURN || key == VK_UP || key == VK_DOWN || key == VK_PRIOR || key == VK_NEXT ||
         (GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000))) {
        m_callbacks.search();
    }

    if (HandlePopupShortcut(key, items)) return true;

    const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (key == VK_ESCAPE) {
        if (m_callbacks.hide_window) {
            m_callbacks.hide_window();
        }
        return true;
    }
    if (key == VK_RETURN) {
        if (m_activeFooter >= 0 &&
            m_activeFooter < static_cast<int>(m_footerButtons.size())) {
            CButton &button = m_footerButtons[static_cast<size_t>(m_activeFooter)];
            if (button.m_hWnd != nullptr && m_owner.m_hWnd != nullptr) {
                const int control_id = button.GetDlgCtrlID();
                ::SendMessageW(
                    m_owner.m_hWnd,
                    WM_COMMAND,
                    MAKEWPARAM(static_cast<WORD>(control_id), BN_CLICKED),
                    reinterpret_cast<LPARAM>(button.m_hWnd)
                );
            }
        } else if (m_callbacks.paste) {
            m_callbacks.paste(m_activeItemIndex);
        }
        return true;
    }
    if (key == VK_UP || key == VK_DOWN || key == VK_TAB) {
        const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool forward = key == VK_DOWN || (key == VK_TAB && !shift);
        NavigateHistoryFromSearch(forward, items, m_historyList, m_pinsList, m_settings.show_footer);
        return true;
    }
    if ((ctrl && (key == VK_HOME || key == VK_END)) || key == VK_PRIOR || key == VK_NEXT) {
        m_keyboardNavigating = true;
        GetCursorPos(&m_keyboardPointer);
        ClearHistoryHover();
        if (!items.empty()) {
            const int target = (key == VK_HOME || key == VK_PRIOR) ? 0 : static_cast<int>(items.size()) - 1;
            SetActiveHistoryItem(target, items, m_historyList, m_pinsList);
            if (m_callbacks.preview) {
                m_callbacks.preview(m_activeItemId, true);
            }
        }
        return true;
    }
    if (ctrl && key == 'F' && m_settings.show_search) {
        return true;
    }
    if (ctrl && key == 'U') {
        return true;
    }
    return false;
}

bool KeyboardHandler::HandlePopupShortcut(WPARAM key, const std::vector<ClipboardItem>& items) {
    if (IsHotKeyPressed(m_settings.pin_hotkey, key)) {
        if (m_callbacks.toggle_pin) {
            m_callbacks.toggle_pin();
        }
        return true;
    }
    if (IsHotKeyPressed(m_settings.delete_hotkey, key)) {
        if (m_callbacks.delete_item) {
            m_callbacks.delete_item();
        }
        return true;
    }
    if (IsHotKeyPressed(m_settings.preview_hotkey, key)) {
        if (m_callbacks.toggle_preview) {
            m_callbacks.toggle_preview();
        }
        return true;
    }

    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

    if (ctrl && alt && key == VK_BACK) {
        if (m_callbacks.clear_history) {
            m_callbacks.clear_history(shift);
        }
        return true;
    }
    if (ctrl && !alt && key == VK_OEM_COMMA) {
        if (m_callbacks.open_settings) {
            m_callbacks.open_settings();
        }
        return true;
    }
    if (ctrl && !alt && key == 'Q') {
        if (m_callbacks.exit) {
            m_callbacks.exit();
        }
        return true;
    }

    // Keep native Edit shortcuts and global popup navigation available.
    if (ctrl && !alt && std::wstring_view(L"ACFUVXYZ").find(static_cast<wchar_t>(key)) != std::wstring_view::npos) {
        return false;
    }

    if ((ctrl != alt) && !(GetKeyState(VK_LWIN) & 0x8000) && !(GetKeyState(VK_RWIN) & 0x8000)) {
        // Main-row and numpad digits are the same shortcut (L-21).
        WPARAM shortcut_key = key;
        if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) {
            shortcut_key = static_cast<WPARAM>('0' + (key - VK_NUMPAD0));
        }
        const auto paste_item = [&](size_t index) {
            SetActiveHistoryItem(static_cast<int>(index), items, m_historyList, m_pinsList);
            if (m_callbacks.paste) {
                m_callbacks.paste(static_cast<int>(index));
            }
            return true;
        };

        // Precedence: Ctrl/Alt+1..9 always quick-paste the Nth unpinned item,
        // whether pins sit above or below the history. New pins never get 1-9
        // (PinKeys.cpp); a pin keyed 1-9 by an older build stays clickable but
        // is not reachable from the keyboard until it is given another key.
        if (shortcut_key >= '1' && shortcut_key <= '9') {
            const int wanted = static_cast<int>(shortcut_key - '0');
            int number = 0;
            for (size_t i = 0; i < items.size(); ++i) {
                if (!items[i].pinned && ++number == wanted) {
                    return paste_item(i);
                }
            }
            return false;
        }
        for (size_t i = 0; i < items.size(); ++i) {
            const auto& item = items[i];
            if (item.pinned && item.pin.size() == 1 &&
                static_cast<WPARAM>(towupper(item.pin[0])) == shortcut_key) {
                return paste_item(i);
            }
        }
    }
    return false;
}

bool KeyboardHandler::ShouldIgnoreShortcutChar(WPARAM character) const {
    const auto chord_down = [&](const HotKeyConfig &hotkey) {
        if (!IsHotKeyPressed(hotkey, hotkey.virtual_key)) {
            return false;
        }
        if (hotkey.virtual_key == VK_SPACE) {
            return character == L' ';
        }
        if (hotkey.virtual_key >= 'A' && hotkey.virtual_key <= 'Z') {
            const wchar_t upper = static_cast<wchar_t>(towupper(static_cast<wint_t>(character)));
            return upper == static_cast<wchar_t>(hotkey.virtual_key);
        }
        if (hotkey.virtual_key >= '0' && hotkey.virtual_key <= '9') {
            return character == static_cast<wchar_t>(hotkey.virtual_key);
        }
        return (hotkey.modifiers & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN)) != 0;
    };
    // TranslateMessage already queued WM_CHAR before the keydown was eaten.
    // Ctrl+Space stays a space, so the search box would insert it after preview opens.
    return chord_down(m_settings.preview_hotkey) || chord_down(m_settings.pin_hotkey) ||
        chord_down(m_settings.delete_hotkey);
}

bool KeyboardHandler::OnChar(WPARAM character) {
    return m_settings.show_search && character >= 0x20 && character != 0x7f &&
        !ShouldIgnoreShortcutChar(character);
}

void KeyboardHandler::SetCallbacks(Callbacks callbacks) {
    m_callbacks = std::move(callbacks);
}
