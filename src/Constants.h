#pragma once

#include "PlatformConfig.h"
#include "Version.h"
#include <cstdint>

// ============================================================================
// Application-wide constants, kept in one place so they are not scattered
// across source files.
// ============================================================================

namespace AppConstants {
    // Application information
    inline constexpr const wchar_t* kAppVersion = AppVersion::kString;

    // ========================================================================
    // Custom window messages (WM_APP + N)
    // +1 tray, +2 ui update, +3 preview worker, +4 preview command,
    // +5 storage worker, +6 update checker, +7 reserved,
    // +8 installer shutdown, +9 open hotkey, +10 capture hotkey, +11 thumbnail worker.
    // Do not renumber shipped IDs; use kCustomMessageBase for new messages.
    // ========================================================================
    constexpr UINT kTrayIconMessage = WM_APP + 1;
    constexpr UINT kUiUpdateMessage = WM_APP + 2;
    constexpr UINT kPreviewWorkerResultMessage = WM_APP + 3;
    constexpr UINT kPreviewWorkerCommandMessage = WM_APP + 4;
    constexpr UINT kStorageWorkerResultMessage = WM_APP + 5;
    constexpr UINT kUpdateCheckerResultMessage = WM_APP + 6;
    constexpr UINT kThumbnailWorkerResultMessage = WM_APP + 11;
    constexpr UINT kInstallerShutdownMessage = WM_APP + 8;
    constexpr UINT kOpenHotkeyMessage = WM_APP + 9;
    constexpr UINT kCaptureHotkeyMessage = WM_APP + 10;

    // Base for newer messages, kept well clear of the shipped IDs above.
    constexpr UINT kCustomMessageBase = WM_APP + 100;
    // Posted by the click-outside mouse hook to hide on the UI thread.
    // wParam/lParam are the physical screen point of the button down.
    constexpr UINT kPopupDismissMessage = kCustomMessageBase + 2;
    // Posted by the icon loader when a shell icon is ready to draw.
    constexpr UINT kHistoryIconMessage = kCustomMessageBase + 4;

    // ========================================================================
    // Timer IDs
    // ========================================================================
    namespace Timer {
        constexpr UINT_PTR kSearchResultCommit = 1;
        constexpr UINT_PTR kPreview = 2;
        constexpr UINT_PTR kPaste = 3;
        constexpr UINT_PTR kHistoryHover = 4;
        constexpr UINT_PTR kPopupDeactivated = 5;
        constexpr UINT_PTR kRevealPopup = 6;
    }

    // Bitmask carried by kUiUpdateMessage. Producers only describe what
    // changed; MainWindow decides the order and granularity of the work.
    namespace UiUpdate {
        constexpr std::uint32_t kIgnoreRules = 1u << 0;
        constexpr std::uint32_t kHistory = 1u << 1;
        constexpr std::uint32_t kLayout = 1u << 2;
        constexpr std::uint32_t kTray = 1u << 3;
        constexpr std::uint32_t kFooter = 1u << 4;
    }

    // ========================================================================
    // Hotkey IDs
    // ========================================================================
    namespace HotKey {
        constexpr int kOpenPopup = 1006;
    }

    // ========================================================================
    // UI dimensions
    // ========================================================================
    namespace UI {
        constexpr int kFooterButtonCount = 4;
        constexpr int kHistoryItemHeight = 22;
        constexpr int kMinimumPopupWidth = 320;
        constexpr int kMaximumPopupWidth = 1600;
        constexpr int kMinimumPopupHeight = 150;
        constexpr int kMaximumPopupHeight = 1200;
        constexpr int kDefaultWindowWidth = 450;
        constexpr int kDefaultWindowHeight = 500;
        constexpr int kDefaultImageMaxHeight = 40;
    }

    // ========================================================================
    // Settings window
    // ========================================================================
    namespace SettingsUI {
        constexpr int kPageCount = 6;
        constexpr int kIgnorePageCount = 3;
    }

    // ========================================================================
    // Database
    // ========================================================================
    namespace DB {
        constexpr int kDefaultHistorySize = 200;
        constexpr const wchar_t* kDatabaseFileName = L"maccy.db";
    }
}
