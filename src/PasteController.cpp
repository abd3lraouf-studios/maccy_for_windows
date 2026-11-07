#include "PasteController.h"

#include "Constants.h"

namespace {

bool IsKeyDown(int virtual_key) {
    return (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
}

bool SendInputs(INPUT *inputs, UINT count) {
    if (inputs == nullptr || count == 0) {
        return false;
    }
    return ::SendInput(count, inputs, sizeof(INPUT)) == count;
}

bool SendControlVPaste() {
    INPUT inputs[4]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = L'V';
    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = L'V';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInputs(inputs, ARRAYSIZE(inputs));
}

bool SendPasteWithHeldControl() {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = L'V';
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = L'V';
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInputs(inputs, ARRAYSIZE(inputs));
}

} // namespace

bool PasteController::IsOwnWindow(HWND window) const noexcept {
    if (window == nullptr || m_owner == nullptr) {
        return false;
    }
    if (window == m_owner || ::IsChild(m_owner, window)) {
        return true;
    }
    const HWND root = GetAncestor(window, GA_ROOT);
    if (root == m_owner) {
        return true;
    }
    return root != nullptr && GetWindow(root, GW_OWNER) == m_owner;
}

void PasteController::CaptureTargetWindow(HWND candidate) {
    HWND foreground = candidate != nullptr ? candidate : GetForegroundWindow();
    if (foreground == nullptr || m_owner == nullptr || IsOwnWindow(foreground)) {
        return;
    }
    const HWND root = GetAncestor(foreground, GA_ROOT);
    if (root != nullptr) {
        foreground = root;
    }
    if (IsOwnWindow(foreground)) {
        return;
    }

    m_targetWindow = foreground;
    m_targetFocusWindow = foreground;
    const DWORD target_thread = GetWindowThreadProcessId(foreground, nullptr);
    GUITHREADINFO gui_info{sizeof(gui_info)};
    if (target_thread != 0 && GetGUIThreadInfo(target_thread, &gui_info) &&
        gui_info.hwndFocus != nullptr) {
        m_targetFocusWindow = gui_info.hwndFocus;
    }
}

PasteController::PasteResult PasteController::RestoreTargetFocusAndPaste(
    HWND target,
    HWND target_focus,
    bool paste
) {
    if (!::IsWindow(target)) {
        return PasteResult::Failed;
    }
    if (::IsIconic(target)) {
        ::ShowWindow(target, SW_RESTORE);
    }
    const DWORD current_thread = GetCurrentThreadId();
    const DWORD target_thread = GetWindowThreadProcessId(target, nullptr);
    const bool attached = target_thread != 0 && target_thread != current_thread &&
        AttachThreadInput(current_thread, target_thread, TRUE) != FALSE;
    SetForegroundWindow(target);
    if (attached && ::IsWindow(target_focus) && GetAncestor(target_focus, GA_ROOT) == target) {
        ::SetFocus(target_focus);
    }
    if (attached) {
        AttachThreadInput(current_thread, target_thread, FALSE);
    }
    SetForegroundWindow(target);
    if (!paste) {
        return PasteResult::FocusOnly;
    }
    const HWND active_window = GetForegroundWindow();
    if (active_window != target && GetAncestor(active_window, GA_ROOT) != target) {
        return PasteResult::Failed;
    }
    if (PasteModifiersDown()) {
        StartPasteTimer(target, target_focus);
        return PasteResult::Deferred;
    }
    // A physically held Ctrl (Ctrl+1, Ctrl+Enter) already forms the chord;
    // injecting our own Ctrl-up would leave Windows thinking it was released.
    if (IsKeyDown(VK_CONTROL)) {
        return SendPasteWithHeldControl() ? PasteResult::Sent : PasteResult::Failed;
    }
    return SendControlVPaste() ? PasteResult::Sent : PasteResult::Failed;
}

bool PasteController::PasteModifiersDown() {
    // The target must see exactly Ctrl+V. Held Shift turns it into Ctrl+Shift+V
    // (Word: paste formatting only, after the plain-paste chord Ctrl+Shift+Enter),
    // Alt into Ctrl+Alt+V (Paste Special, or AltGr+V on some layouts), Win into a
    // shell shortcut. Injecting their key-ups instead is unsafe: a lone Alt-up
    // activates the target's menu bar and a lone Win-up opens Start, so the
    // paste waits for the user to release them. This deliberately reverses M-24,
    // which stopped waiting on Shift and so let Ctrl+Shift+V through.
    return IsKeyDown(VK_SHIFT) || IsKeyDown(VK_MENU) || IsKeyDown(VK_LWIN) ||
        IsKeyDown(VK_RWIN);
}

void PasteController::StartPasteTimer(HWND target, HWND focus) {
    m_pendingPasteTarget = target;
    m_pendingPasteFocus = focus;
    m_pendingPasteDeadline = GetTickCount64() + 3000;
    ::SetTimer(m_owner, AppConstants::Timer::kPaste, 15, nullptr);
}

void PasteController::OnPasteTimer() {
    const HWND target = m_pendingPasteTarget;
    const HWND focus = m_pendingPasteFocus;
    // The user switched windows while we waited: never pull focus back or
    // paste into whatever is in front now.
    const HWND foreground = GetForegroundWindow();
    if (!::IsWindow(target) ||
        (foreground != target && GetAncestor(foreground, GA_ROOT) != target)) {
        StopPasteTimer();
        return;
    }
    if (!PasteModifiersDown()) {
        StopPasteTimer();
        RestoreTargetFocusAndPaste(target, focus, true);
        return;
    }
    // Still held after the deadline. Forcing Ctrl+V now would send a modified
    // chord (M-13), so give up; the item is already on the clipboard and the
    // user can paste it themselves.
    if (GetTickCount64() > m_pendingPasteDeadline) {
        StopPasteTimer();
    }
}

void PasteController::StopPasteTimer() {
    if (m_owner != nullptr) {
        KillTimer(m_owner, AppConstants::Timer::kPaste);
    }
    m_pendingPasteTarget = nullptr;
    m_pendingPasteFocus = nullptr;
    m_pendingPasteDeadline = 0;
}

bool PasteController::IsPastePending() const noexcept {
    return m_pendingPasteTarget != nullptr;
}
