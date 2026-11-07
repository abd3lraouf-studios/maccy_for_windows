#pragma once

#include "PlatformConfig.h"

#include "ClipboardData.h"

// Windows notification for every captured copy: the source app's icon and
// name, a one-paragraph snippet, and a thumbnail for images. Registering the
// app's AppUserModelID lists Maccy under Settings > System > Notifications,
// where Windows owns the on/off, banner and sound switches.
//
// Icons, thumbnails and the WinRT calls run on a worker thread: the UI thread
// also owns the low-level keyboard hook, so it must not stall.
namespace CopyNotification {

// Clicking a notification posts kOpenHotkeyMessage to owner.
void Initialize(HWND owner);
void Show(const ClipboardSnapshot &capture);
// Cleared history must not survive in what notifications keep: deletes the
// image files, drops a notification not yet shown and empties Maccy's
// entries in Action Center. Safe while the worker is writing or showing.
void ClearHistory();
void Shutdown() noexcept;

} // namespace CopyNotification
