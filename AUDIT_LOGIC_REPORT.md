# Maccy for Windows — Logic Audit Report

- **Audit target**: `C:\Users\user\CLionProjects\maccy` (Windows clipboard history / tray app)
- **Audit perspective**: native Windows development (Win32 / WTL / clipboard / low-level keyboard hooks / multithreading / SQLite)
- **Audit method**: static code review (no integration tests run / no runtime reproduction)
- **Scope**: core business and window logic in `src/` (excluding packaging script details and documentation wording)
- **Fix tracking**: status and batches are in [`FIX_CHECKLIST.md`](FIX_CHECKLIST.md); every finding in this file has a stable anchor (`#C-01`, `#H-01` …) for the checklist to link back to

---

## 1. Overview

The overall architecture is clear: UI thread + a single `StorageWorker` write connection + `PreviewWorker` / `ThumbnailWorker` / `UpdateChecker` background threads; clipboard monitoring uses `AddClipboardFormatListener`, and the popup hotkey uses `WH_KEYBOARD_LL`.

The main risks fall into five categories:

1. **Window/thread lifecycles** (the preview window across threads, shutdown order)
2. **Deduplicating clipboard self-write echoes** (fingerprints and system-synthesized formats)
3. **Paste focus restore and `SendInput`**
4. **Asynchronous callbacks capturing `this`, and UI-blocking `Invoke`**
5. **Multiple copies of settings state / inconsistent persistence**

All findings are listed below by severity. IDs are globally unique for tracking.

**Severity levels**

| Level | Meaning |
|------|------|
| Critical | Can cause crashes, hangs, obvious data corruption, or systematic failure of core features |
| High | Errors on high-frequency user paths, history pollution, failed pastes, significant races |
| Medium | Edge-case / performance / consistency problems that are reproducible or affect the experience |
| Low | Engineering debt, DPI/visuals, edge paths, maintainability problems |

---

## 2. Critical

<a id="C-01"></a>
### C-01 The preview worker thread is not yet stopped when the main window is destroyed

- **Location**: `MainWindow::ExitApplication` (around 1468–1485); `main.cpp` calls `preview.Stop()` only after the message loop exits (around 358–362); `ApplicationController::Shutdown` does not stop the preview
- **Problem**: `PreviewWindow` is created on, and pumps messages on, the `PreviewWorker` thread; on exit the UI thread first destroys the owner with `DestroyWindow()`, while the preview's `Stop()` runs only after the message loop ends.
- **Impact**: destroying owner/owned windows across threads is a classic Win32 lifecycle trap — the exit phase can hang, hit random access violations, or leave the preview window behind.
- **Fix direction**: call `m_previewWorker.Stop()` synchronously before `DestroyWindow()` (handle the installer-exit and initialization-failure paths the same way); or make `ApplicationController::Shutdown` responsible for stopping the preview.

<a id="C-02"></a>
### C-02 The preview UI runs entirely on a non-UI thread

- **Location**: `PreviewWorker::ThreadMain` / `ProcessRequest`; the creation, `SetWindowPos`, `ShowWindow`, and painting of `PreviewWindow`
- **Problem**: HWND creation, showing, and GDI painting all happen on the worker thread, not on the UI thread that owns `MainWindow`.
- **Impact**: violates UI thread affinity; hard-to-reproduce failures appear where it interleaves with the main thread's activation, focus, DPI, `ContainsWindow`, and destruction paths.
- **Fix direction**: keep decoding/database reads in the background; send the result back to the main thread with `PostMessage`, and create/show the owned preview window on the main thread; or keep the preview on the main thread only, with asynchronous decoding.

---

## 3. High

<a id="H-01"></a>
### H-01 Self-write clipboard fingerprint dedup is unreliable (order + synthesized formats → paste echoes enter history)

- **Location**: `ClipboardMonitor::WriteClipboardItem` (around 520–587); `ClipboardFingerprint::Add` / `HashCapture`; `ReadClipboardAndSave` (around 506–512)
- **Problem**:
  1. **Order-sensitive**: the write side hashes in `item.data` order (`sequence ASC` in the database); the read side hashes in `EnumClipboardFormats` order. The two orders are not guaranteed to match → the fingerprints can differ even when the format sets are the same.
  2. **Format-set-sensitive**: the write only covers the formats actively set with `SetClipboardData`; a capture often also contains formats the system renders lazily or synthesizes (for example, writing only `CF_UNICODETEXT` still enumerates `CF_TEXT`).
- **Impact**: `was_written_by_this_process` often fails → paste echoes are saved as if the user had copied them; combined with `MarkCopied`, ordering and counts go wrong.
- **Fix direction**:
  - use an **order-independent** fingerprint (sort by `(format, name)` before hashing, or use an XOR/multiset hash over formats);
  - compute the fingerprint only over a "canonical format subset", or register a private "written by Maccy" clipboard format;
  - or switch to `GetClipboardSequenceNumber` + a self-write marker for this process / a short ignore window.

<a id="H-02"></a>
### H-02 A failed capture clears `m_expectedClipboardFingerprint`

- **Location**: `ClipboardMonitor::ReadClipboardAndSave` (around 501–503)
- **Problem**: when the `OpenClipboard` retries fail or `CaptureClipboard` returns `nullopt`, the expected fingerprint is `reset()` straight away.
- **Impact**: if the clipboard briefly cannot be opened right after a write, the next successful capture no longer has an expected fingerprint → self-written content enters history; more visible combined with H-01.
- **Fix direction**: clear it only after "confirming that content different from the expected one was read", or after a timeout; keep the expected fingerprint when opening fails.

<a id="H-03"></a>
### H-03 A mid-way failure in `WriteClipboardItem` leaves the clipboard incomplete

- **Location**: `ClipboardMonitor::WriteClipboardItem` (around 576–584)
- **Problem**: after `EmptyClipboard` succeeds, the `SetClipboardData` loop returns `false` if it fails mid-way; formats already submitted stay on the clipboard, the unsubmitted `UniqueGlobal`s are destroyed and freed, and no expected fingerprint is set.
- **Impact**: the user's clipboard is emptied or left with only some formats; after the paste failure message, the system clipboard is already damaged.
- **Fix direction**: prepare every HGLOBAL first and only `EmptyClipboard` + submit once all of them succeed; on failure, restore the pre-write snapshot as far as possible, or at least do not return in a half-submitted state.

<a id="H-04"></a>
### H-04 Paste path: focus is back on the target, but `SendInput` silently gives up

- **Location**: `PasteController::RestoreTargetFocusAndPaste` (around 101–104)
- **Problem**: after `SetForegroundWindow`, if `GetForegroundWindow` is still not the target (common with UIPI / the foreground lock), the function just returns, even though `WriteClipboardItem` may already have overwritten the system clipboard.
- **Impact**: "the history item went onto the clipboard but was never pasted"; the user thinks the paste failed, while the clipboard has in fact been replaced.
- **Fix direction**: distinguish "copy only" from "copy + paste"; report a failed paste, or offer a rollback; strengthen foreground rights (`AllowSetForegroundWindow`, retries, the timing of `AttachThreadInput`).

<a id="H-05"></a>
### H-05 Asynchronous callbacks capture `MainWindow*` / may still run after exit

- **Location**: `MainWindow::PasteItem`, `RefreshHistory`, `LoadImageThumbnails`; `ApplicationController::SaveCapturedClipboard`
- **Problem**: the lambdas capture `this`; `Shutdown` only clears the UI HWND and does not cancel callbacks that are already queued and about to run through `DrainUiCallbacks`; `storage.Stop()` comes after the message loop.
- **Impact**: during exit/destruction, `WriteClipboardItem`, `HideMainWindow`, `MessageBox`, or list state changes can still run → logic errors or crashes on half-destroyed objects.
- **Fix direction**: a window epoch / `std::weak_ptr` guard; check `m_exiting` at the callback entry; cancel and drain the asynchronous queue before `DestroyWindow`.

<a id="H-06"></a>
### H-06 `CallNextHookEx` in `WH_KEYBOARD_LL` passes `nullptr` as the first argument

- **Location**: `KeyboardHandler::LowLevelKeyboardProc` (several places)
- **Problem**: every call is `CallNextHookEx(nullptr, ...)`. The documentation allows this, but it is inconsistent with the idiom of holding the `m_keyboardHook` handle; in some environments chain behavior becomes harder to diagnose.
- **Impact**: usually works, but problems are harder to pin down when it coexists with injected replays and antivirus hooks.
- **Fix direction**: pass `handler->m_keyboardHook` (take care to get the current hook handle inside the static callback).

<a id="H-07"></a>
### H-07 Delayed Win key replay and the `SendInput` injected flag

- **Location**: `KeyboardHandler::LowLevelKeyboardProc` (around 203–250)
- **Problem**: when Win is released alone, or Win + another key, a Win-down is re-sent with `SendInput`; the injected event is let through by this hook because of `LLKHF_INJECTED`, but the timing the system and other hooks see differs from the real keystrokes.
- **Impact**: the Start menu occasionally opens by mistake, Win + key combinations stop working, conflicts with games or other global hooks; antivirus heuristic risk (the project README already warns about it).
- **Fix direction**: evaluate switching to `RegisterHotKey` (when there is no Win modifier) or more precise scan code / extended key flags; replay with the original `scanCode`/`flags`; an explicit state machine for pairing Win down/up.

<a id="H-08"></a>
### H-08 The single-slot `m_expectedClipboardFingerprint` can be overwritten by consecutive writes

- **Location**: `ClipboardMonitor.h` (around 71); `WriteClipboardItem` (around 586)
- **Problem**: only one expected fingerprint is kept; if a second write happens before the first paste's `WM_CLIPBOARDUPDATE` arrives, the expected value is overwritten.
- **Impact**: the first paste's echo may be saved; compounds with H-01/H-02.
- **Fix direction**: a pending self-write queue / counter, or a private clipboard format carrying a generation id.

<a id="H-09"></a>
### H-09 The window between changing the clipboard first and then grabbing focus for `SendInput`

- **Location**: `MainWindow::PasteItem` (around 1168–1193) and `PasteController::RestoreTargetFocusAndPaste`
- **Problem**: `WriteClipboardItem` runs while Maccy still has focus; Ctrl+V is injected only after the window is hidden and the target's focus is restored.
- **Impact**: in between, other programs can read or overwrite the clipboard, or extra listener noise is triggered.
- **Fix direction**: write as close as possible to just before `SendInput`; or enable a short ignore window + fingerprint during the write (together with H-01).

---

## 4. Medium

<a id="M-01"></a>
### M-01 Alt+click on the tray "pause" is not persisted

- **Location**: `MainWindow::OnTrayIcon` (around 2271–2278) compared with `OnTrayIgnoreCommand` (around 2163–2171)
- **Problem**: toggling `ignore_events` with Alt+left-click does not call `PersistSettings()`; the menu item path does persist.
- **Impact**: the pause state is lost after a restart; inconsistent with the clipboard path, which calls SaveSettings whenever it detects an ignore change.
- **Fix direction**: both paths share one toggle + persist + refresh tray/footer helper.

<a id="M-02"></a>
### M-02 The settings window holds a copy of the settings, which can overwrite tray state when saved

- **Location**: `m_settings` inside `SettingsWindow`; the tray toggling ignore in `MainWindow`
- **Problem**: while the settings window is open, the tray changes `MainWindow::m_settings`; when the user clicks save in the settings window, the old snapshot may be written back to the DB.
- **Impact**: switches such as pause/ignore are "changed and then overwritten by the settings page".
- **Fix direction**: `SetSettingsSnapshot` on external changes; or a single settings source + a dirty-field merge strategy.

<a id="M-03"></a>
### M-03 `m_loadingList` can get stuck when the search callback returns early

- **Location**: the `MainWindow::RefreshHistory` callback (around 809–813); `PasteItem` depends on `!m_loadingList` (around 1140)
- **Problem**: when the generation / query text / window check fails, it just `return`s without setting `m_loadingList` to false. A later search usually repairs it; but if "the latest generation is discarded because the query differs and nothing else completes afterwards", loading stays true.
- **Impact**: pasting and list selection are blocked permanently, and the popup looks dead.
- **Fix direction**: tie loading to the generation; only the latest generation clears loading; ignore stale results, and if the latest result is rejected, clear loading or search again.

<a id="M-04"></a>
### M-04 Synchronous `StorageWorker::Invoke` on the UI thread blocks

- **Location**: `Invoke` in `StorageWorker.h`; `PersistSettings`, `TogglePin`, `DeleteItem`, `ClearHistory`, `MarkCopied`, clearing history on exit, etc.
- **Problem**: the UI thread waits on the storage thread with `future.get()`; when storage is busy saving a large image or searching the whole table, the popup stutters.
- **Impact**: the UI freezes for hundreds of milliseconds to several seconds when dragging, pasting, pinning, or deleting items.
- **Fix direction**: make hot paths asynchronous; debounce settings saves; startup can still use the synchronous API.

<a id="M-05"></a>
### M-05 Two SQLite connections (Storage + Preview)

- **Location**: `StorageWorker::ThreadMain`; `Database database(m_path)` in `PreviewWorker::ThreadMain`; `busy_timeout = 1000`
- **Problem**: the preview's read-only connection runs concurrently with the storage write connection; when busy, the preview's `GetItem` may throw.
- **Impact**: the preview fails intermittently; the debug output shows "Preview worker stopped".
- **Fix direction**: route the preview through `StorageWorker::GetItemAsync`; or a read-only connection + explicit retry/degradation.

<a id="M-06"></a>
### M-06 A cancelled preview request does not call back into the UI

- **Location**: several early `return`s in `PreviewWorker::ProcessRequest`; `Request` clears the request when `PostThreadMessage` fails (around 230–238)
- **Problem**: the cancel/stop paths do not `PostResult`; the UI still believes the request is in flight.
- **Impact**: stale preview candidate state; the state flickers when quickly moving through the list.
- **Fix direction**: always call back with `displayed=false` / `cancelled`; bump the main window's generation when `Request` returns false.

<a id="M-07"></a>
### M-07 ThumbnailWorker silently drops jobs after COM initialization fails

- **Location**: `ThumbnailWorker::ThreadMain` (around 75–96)
- **Problem**: when `CoInitializeEx` hard-fails, the loop does not process jobs, but `Submit` still accepts them.
- **Impact**: image thumbnails stay blank forever, with no error message.
- **Fix direction**: a failure flag; reject Submit; optionally a one-time UI message.

<a id="M-08"></a>
### M-08 An `HBITMAP` created on another thread is painted on the UI thread

- **Location**: `ThumbnailWorker` decoding; `StretchBlt` in `HistoryRenderer`
- **Problem**: using GDI bitmaps across threads is unstable in some environments.
- **Impact**: blank thumbnails, occasional GDI errors.
- **Fix direction**: the worker passes only DIB bytes and the UI thread calls `CreateDIBSection`; or materialize the WIC bitmap on the UI side.

<a id="M-09"></a>
### M-09 `AppSettings::Save`: the registry fails after the DB has committed

- **Location**: `AppSettings::Save` in `Settings.cpp` (around 249–304)
- **Problem**: `SetLaunchAtLogin` runs only after the transaction `Commit`; a registry failure throws, but the DB has already been written.
- **Impact**: the DB and the "launch at sign-in" registry entry disagree.
- **Fix direction**: registry first, then DB; or write back a compensation on failure.

<a id="M-10"></a>
### M-10 `SearchHistory` loads the whole table into memory every time before filtering

- **Location**: `Database::SearchHistory` (around 880–901 and after)
- **Problem**: `SELECT ... FROM history_items ORDER BY id DESC` pulls the metadata of the whole table, then filters exact/fuzzy/regex in-process.
- **Impact**: with thousands of history items + large previews, the storage thread is heavily loaded and the UI stutters indirectly through `Invoke`/asynchronous callbacks; regex mode is worse (see M-11).
- **Fix direction**: filter/sort on the SQL side; paginate the list; cap the scan for fuzzy/regex.

<a id="M-11"></a>
### M-11 User regexes / search regexes may cause ReDoS

- **Location**: `ClipboardMonitor::ShouldIgnoreText`; the regex branch of `Database::SearchHistory`; `HistoryRenderer` highlighting
- **Problem**: `std::wregex` + `regex_search` run on clipboard text that can be very long; a malicious or clumsy pattern can cause catastrophic backtracking.
- **Impact**: the UI/storage thread sits at 100% CPU for a long time, and the app looks frozen.
- **Fix direction**: timeout cancellation, length limits, RE2 or a restricted engine; compilation failures are already swallowed, but runtime still needs a time limit.

<a id="M-12"></a>
### M-12 `ignore_only_next_event` and the ignore state machine are scattered

- **Location**: `ClipboardMonitor::OnClipboardUpdate`; `ApplicationController::HandleClipboardUpdate`; the settings page's `IDC_X_IGNORE_NEXT`; the tray toggle
- **Problem**: several places modify fields of an `AppSettings` reference; the persistence conditions differ (some paths Save, some wait for a clipboard event).
- **Impact**: hard to reason about when the settings page's "ignore only the next one" is combined with the tray pause; the state after a restart is surprising.
- **Fix direction**: a single `SetIgnorePolicy(...)` that changes memory + optionally persists + notifies the UI.

<a id="M-13"></a>
### M-13 Paste timer: if a modifier key stays held, the paste is abandoned after at most 3 seconds

- **Location**: `PasteController::StartPasteTimer` / `OnPasteTimer` (deadline +3000ms)
- **Problem**: if the user holds Ctrl/Alt/Shift for more than 3 seconds, the timer stops and never calls `SendInput`.
- **Impact**: when the user opens with the shortcut and immediately presses Enter to paste before releasing the modifiers, it occasionally "copies but does not paste".
- **Fix direction**: still try to paste on timeout; or force a wait for key-up when hiding the popup; document the behavior.

<a id="M-14"></a>
### M-14 `SendInput` return values are not checked / modifier key state is not restored

- **Location**: `SendControlVPaste` / `SendPasteWithHeldControl` in `PasteController.cpp`
- **Problem**: ignores the `SendInput` return value; does not handle extended keys; does not consistently key-up leftover modifier keys.
- **Impact**: pasting fails without feedback for high-integrity targets, Remote Desktop, and some IMEs.
- **Fix direction**: check the return value; report failures; use scan codes where needed.

<a id="M-15"></a>
### M-15 `GetFocus()` in the LL hook only reflects the installing thread's focus

- **Location**: `KeyboardHandler::LowLevelKeyboardProc` (around 102–107), hotkey capture detection
- **Problem**: the LL hook runs on the installing thread, and `GetFocus()` is only meaningful for Maccy's UI thread. That is correct for the settings page's in-process controls; if a hotkey control ever lives on another thread, it will break.
- **Impact**: mostly Low under the current architecture; but hotkey capture relying on "visible + control ID" is fragile.
- **Fix direction**: an explicit `m_capturingHotkey` flag set/cleared by the settings window, instead of guessing with `GetFocus`.

<a id="M-16"></a>
### M-16 `CaptureTargetWindow()` may have already lost the target when the popup opens

- **Location**: `ShowMainWindow` (around 2379–2380); the hotkey message carries the foreground window (around 159–160)
- **Problem**: if the hotkey path did not carry the correct foreground window, or the shell / this window has already taken focus, `CaptureTargetWindow` just returns and keeps the old target — which may be a stale HWND.
- **Impact**: pastes into the wrong window, or `IsWindow` fails and nothing is pasted (compounds with H-04).
- **Fix direction**: the hotkey message always carries the foreground window captured at that moment; report a clear failure when the target is invalid.

<a id="M-17"></a>
### M-17 `ApplicationController::Shutdown` does not stop the preview (widens the C-01 window)

- **Location**: `ApplicationController.cpp` (around 60–76)
- **Problem**: it only calls `SetUiWindow(nullptr)`; the thread and the preview window are still there.
- **Fix direction**: fold it, together with C-01, into a single shutdown order.

<a id="M-18"></a>
### M-18 No user-visible error when registering the open hotkey fails

- **Location**: `MainWindow::InitializeHistoryControls` (around 1840–1842); `RegisterGlobalHotKey`
- **Problem**: when `SetWindowsHookEx` fails, `m_hotkeyRegistered=false`, but initialization is still marked as successful.
- **Impact**: there is no global hotkey and the user can only open the popup from the tray, without an easy way to find out why (permissions / hook refused).
- **Fix direction**: a tray balloon warning at initialization / a status on the settings page.

<a id="M-19"></a>
### M-19 `CaptureBitmapAsDib` rejects negative heights (top-down DIBs)

- **Location**: `CaptureBitmapAsDib` in `ClipboardMonitor.cpp` (around 86–88)
- **Problem**: `source.bmHeight <= 0` fails straight away; a top-down bitmap has a negative height in `BITMAP`.
- **Impact**: when only `CF_BITMAP` is present and there is no DIB/DIBV5, images from some applications cannot be saved.
- **Fix direction**: compute the size with `abs(bmHeight)`; set the sign of `biHeight` as `GetDIBits` requires.

<a id="M-20"></a>
### M-20 The whole copy is dropped if any single format matches the ignore list

- **Location**: `ClipboardMonitor::CaptureClipboard` (around 384–389)
- **Problem**: if any enumerated format name matches an ignore rule, it returns `nullopt`, dropping the text/image along with it.
- **Impact**: when a vendor metadata format is ignored, the user-visible body does not make it into history either.
- **Fix direction**: confirm with product whether this is intended; otherwise skip storing that format instead of abandoning the whole copy.

<a id="M-21"></a>
### M-21 `ignore_only_next` has DDX/a resource ID, but the `.rc` has no matching control

- **Location**: DDX `IDC_X_IGNORE_NEXT` in `SettingsWindow.h`; `resource.h`; the Advanced page in `SettingsWindow.rc` only has `IDC_X_IGNORE_EVENTS`
- **Problem**: the code and the persistence key exist, but the settings UI has no checkbox; users cannot configure "ignore only the next one" in the UI.
- **Impact**: a half-dead feature; it only takes effect through the DB/default values, and the behavior is hard to understand (compounds with M-12 and M-22).
- **Fix direction**: add the RC control, or remove the setting and the DDX.

<a id="M-22"></a>
### M-22 `ignore_only_next_event` only takes effect when `ignore_events==true`

- **Location**: `ClipboardMonitor::OnClipboardUpdate` (around 600–606)
- **Problem**: the outer check tests `ignore_events` first; "ignore only the next one" cannot skip a single copy on its own.
- **Impact**: if the product meaning is "skip the next copy", the current implementation does not deliver it (unless pause is also turned on).
- **Fix direction**: handle the one-shot independently; or document it explicitly as "an auto-release switch for pause" and fix the UI (M-21).

<a id="M-23"></a>
### M-23 `m_pasteInProgress` is cleared too early, before the paste completes

- **Location**: the asynchronous callback of `MainWindow::PasteItem` (around 1163 vs 1193)
- **Problem**: the callback sets `m_pasteInProgress = false` on entry, and only then calls `WriteClipboardItem` / `RestoreTargetFocusAndPaste` (including the 3s timer path).
- **Impact**: the user can trigger a second paste, overwriting the clipboard and scrambling the expected fingerprint (H-08).
- **Fix direction**: keep the flag until the paste/timer finishes or clearly fails.

<a id="M-24"></a>
### M-24 `PasteModifiersDown` treats Shift as a blocking modifier

- **Location**: `PasteController.cpp` (around 116–118, 105–110)
- **Problem**: while Shift is still held (left over from a selection shortcut), it takes the 3s wait-timer path instead of sending Ctrl+V immediately.
- **Impact**: pasting is slower or is abandoned on timeout (compounds with M-13).
- **Fix direction**: wait only for modifiers that break Ctrl+V (such as Alt); or use the short injection only on the "Control already pressed" path.

<a id="M-25"></a>
### M-25 With `save_text` off, regex ignore rules do not check the HTML/RTF body

- **Location**: `ClipboardMonitor::CaptureClipboard` (around 392–403)
- **Problem**: with `save_text==false`, plain text is still extracted for the regex; when there is no Unicode/ANSI text, `ShouldIgnoreText` is not applied to rich text.
- **Impact**: sensitive content that exists only as HTML/RTF bypasses the ignore rules.
- **Fix direction**: run the ignore check with `SearchPartsFromFormats` (decoupled from whether it is saved).

<a id="M-26"></a>
### M-26 The default ignored format name `CF_CLIPBOARD_VIEWER_IGNORE` may never match

- **Location**: `ClipboardRules::DefaultIgnoredFormats`; `FormatName` / `ShouldIgnoreFormat`
- **Problem**: the name that `GetClipboardFormatNameW` returns for a registered format is often not the literal `CF_*` alias.
- **Impact**: the built-in "ignore the viewer-ignore marker" may never take effect.
- **Fix direction**: compare by registered format ID, or maintain a table of known aliases.

---

## 5. Low

<a id="L-01"></a>
### L-01 `PreviewWorker::Reposition` uses the owner cached at thread start

- **Location**: `kRepositionCommand` in `PreviewWorker::ThreadMain` (around 324–327) compared with `ProcessRequest`, which uses `m_ownerWindow.load()`
- **Problem**: reposition uses the local `owner`, not an atomic load of the current owner.
- **Impact**: the owner does not change today; once it does, a dead handle will be used.
- **Fix direction**: use `m_ownerWindow.load()` everywhere.

<a id="L-02"></a>
### L-02 `StorageWorker::Stop` drops UI callbacks that have not run

- **Location**: `StorageWorker::Stop` (around 56–58)
- **Problem**: it clears `m_uiCallbacks` without running them.
- **Impact**: acceptable on exit; compounds with H-05 when the order is wrong.
- **Fix direction**: an explicit shutdown protocol: stop accepting posts first → drain safe callbacks → join.

<a id="L-03"></a>
### L-03 The resize hit-test border is not scaled for DPI

- **Location**: `kResizeBorder = 8` in `MainWindow.cpp`; `OnNcHitTest`
- **Impact**: the edges are hard to drag at high DPI.
- **Fix direction**: scale with `MulDiv` / `UiFont::DpiForWindow`.

<a id="L-04"></a>
### L-04 The tray icon's mask bitmap may have an incorrect transparency channel

- **Location**: `CreateIconFromPngResource` in `TrayIcon.cpp` (around 189–205)
- **Problem**: the 1-bit mask from `CreateBitmap` is not generated from alpha.
- **Impact**: under some themes the tray icon looks grey or has ragged edges.
- **Fix direction**: generate correct AND/XOR masks from premultiplied alpha, or use an icon creation path that supports alpha.

<a id="L-05"></a>
### L-05 `UpdateChecker::Start` `join`s the previous check on the UI thread

- **Location**: `UpdateChecker.cpp` (around 252–255)
- **Impact**: clicking "Check for updates" repeatedly can hang until the network timeout.
- **Fix direction**: disable the button while checking; join off the UI thread, or join only in Stop.

<a id="L-06"></a>
### L-06 `SaveClipboard` does not update the payload/FTS when the fingerprint matches and formats already exist

- **Location**: `Database::SaveClipboard` (around 806–809)
- **Problem**: the same fingerprint skips `ReplaceFormats` / `ReplaceSearchIndex`.
- **Impact**: when the fingerprint algorithm changes or collides, old blobs are left behind; generally acceptable.
- **Fix direction**: make sure the fingerprint covers the full payload; compare size/checksum where needed.

<a id="L-07"></a>
### L-07 The `main.cpp` exception path relies on destruction order to stop threads

- **Location**: the `wWinMain` catch (around 368–376)
- **Problem**: no explicit `storage.Stop()` / `preview.Stop()`.
- **Impact**: the destruction order of the stack objects is usually correct today; easy to break in a refactor.
- **Fix direction**: a scope_guard / explicit stop.

<a id="L-08"></a>
### L-08 The fingerprint algorithm is a 64-bit FNV variant with theoretical collisions

- **Location**: `ClipboardFingerprint` in `ClipboardMonitor.cpp`
- **Impact**: different content mapping to the same fingerprint wrongly merges history (updates the same row). The probability is low, but not zero at large data volumes.
- **Fix direction**: a cryptographic hash (truncated BLAKE3/SHA-256), or mix in length/format counts.

<a id="L-09"></a>
### L-09 The `CF_BITMAP` → DIB conversion drops palette/special-format bitmaps

- **Location**: `CaptureBitmapAsDib` (forces 32bpp BI_RGB)
- **Impact**: captures of unusual bitmaps fail and are skipped; a DIB/DIBV5 is already preferred when present; combined with M-19 (negative height), images are lost more easily.
- **Fix direction**: prefer an existing DIB; query the real bit count/palette before `GetDIBits`; log conversion failures.

<a id="L-10"></a>
### L-10 `FilesPreview` / enumerated file paths have no total length limit

- **Location**: `ClipboardMonitor::FilesPreview`
- **Problem**: large numbers of file paths are concatenated into the preview string.
- **Impact**: memory and the DB preview grow (partly mitigated by StoredPreview truncation).
- **Fix direction**: truncate in line with `ClipboardRules::Limits`.

<a id="L-11"></a>
### L-11 In "allowlist mode", the ignore-application rule always ignores an empty application

- **Location**: `ShouldIgnoreApplication` (around 206–208)
- **Problem**: when the source process path cannot be obtained, allowlist mode drops that copy.
- **Impact**: history is missing in some UWP/service/delayed-rendering scenarios (possibly an intentional policy).
- **Fix direction**: confirm with product; or fall back to window class name rules.

<a id="L-12"></a>
### L-12 `RegisterGlobalHotKey` does not do what its name says

- **Location**: `KeyboardHandler::RegisterGlobalHotKey`
- **Problem**: it actually calls `SetWindowsHookEx(WH_KEYBOARD_LL)`, not `RegisterHotKey`.
- **Impact**: misleading for readability/maintenance; easily confused with the `WM_HOTKEY` handling that exists alongside it.
- **Fix direction**: rename it to `InstallOpenHotkeyHook`; evaluate switching to `RegisterHotKey` for non-Win modifiers.

<a id="L-13"></a>
### L-13 The tray does not set `NOTIFYICON_VERSION_4`

- **Location**: `TrayIcon::Add`
- **Impact**: some Win10/11 behaviors (such as modern notifications) are inconsistent; the feature basically works.
- **Fix direction**: `NIM_SETVERSION` + `NOTIFYICON_VERSION_4`.

<a id="L-14"></a>
### L-14 `HasPendingKeyboardInput` uses `PeekMessage` to look at the keyboard queue

- **Location**: `MainWindow.cpp` (around 89–94)
- **Problem**: it only peeks at that HWND's queue; IME or other input paths may be missed.
- **Impact**: the timing of committing search results jitters slightly.
- **Fix direction**: combine it with the IME composition flag (the project already has some IME handling).

<a id="L-15"></a>
### L-15 Message ID gaps and comments

- **Location**: `Constants.h`: `kInstallerShutdownMessage = WM_APP+8` sits between +6 and +9; `kThumbnailWorkerResultMessage = WM_APP+11`
- **Impact**: no functional problem; easy to cause collisions when extending.
- **Fix direction**: tidy up a contiguous allocation table.

<a id="L-16"></a>
### L-16 The set of available `PinKeys` is fixed and excludes a/q/v/w/z, etc.

- **Location**: `kSupportedKeys` in `PinKeys.cpp`
- **Impact**: with many pinned items, `Next` returns empty (there is already a MessageBox); users have to understand the conflict policy with shortcut letters.
- **Fix direction**: show the available keys on the settings page; support a custom pin key table.

<a id="L-17"></a>
### L-17 The HTML/RTF stripping heuristics are crude

- **Location**: `ClipboardRules::StripHtml` / `StripRtf`
- **Impact**: the quality of the searchable body is mediocre; does not cause crashes.
- **Fix direction**: improve as needed; keep the length limit.

<a id="L-18"></a>
### L-18 Conformance with the WTL guidelines (engineering-oriented)

- **Reference**: `WTL.md`
- **Observation**: the main window already uses `CDialogImpl` + message maps; there is still plenty of direct Win32 (hooks, clipboard, tray) — a reasonable layer under the guidelines. Settings/history are driven by `.rc`, which is the right direction.
- **Problem**: the C-style function pointer callback API of `KeyboardHandler` leans toward "heavy wrapping"; it slightly conflicts with the project's "fewer layers" principle, but it is not a defect.
- **Fix direction**: in the long run, switch to `std::function` or direct friends / `MainWindow` methods, reducing the void* callback table.

<a id="L-19"></a>
### L-19 Test coverage gaps

- **Existing**: `CoreRulesTests`, `FileHistoryTests`, `UpdateCheckerTests`
- **Gaps**: no automated tests for `WriteClipboardItem` echo dedup, `PasteController` focus failures, `m_loadingList`, or the shutdown order.
- **Fix direction**: add unit tests for fingerprints/synthesized formats and the ignore state machine; add a manual test checklist for the critical paths.

<a id="L-20"></a>
### L-20 Enter does nothing when the footer has focus

- **Location**: `KeyboardHandler::HandlePopupKey` (around 510–512)
- **Problem**: when `m_activeFooter >= 0`, the `VK_RETURN` branch is empty.
- **Impact**: footer buttons (clear/settings, etc.) cannot be activated from the keyboard.
- **Fix direction**: call the matching footer command callback.

<a id="L-21"></a>
### L-21 The numeric keypad cannot trigger numbered quick paste

- **Location**: `KeyboardHandler::HandlePopupShortcut` (around 594–600)
- **Problem**: it only matches `'0'+n` and does not map `VK_NUMPAD0`–`9`.
- **Impact**: numeric keypad users cannot quick-paste with digits.
- **Fix direction**: normalize digit keys / add a NUMPAD mapping.

<a id="L-22"></a>
### L-22 `ClipboardGuard` only calls `OpenClipboard(owner)`, with no `NULL` fallback

- **Location**: `ClipboardMonitor.cpp` (around 29–31)
- **Problem**: opening fails slightly more often during shutdown or when the owner is invalid.
- **Impact**: occasional open failures at the edges, which amplify H-02.
- **Fix direction**: fall back to `OpenClipboard(nullptr)` when needed; do not open again after `Shutdown`.

---

## 6. Summary by subsystem

### 6.1 Clipboard monitoring and rules (`ClipboardMonitor` / `ClipboardRules`)

| ID | Level | Summary |
|----|------|------|
| H-01 | High | Self-write fingerprint: order + synthesized formats |
| H-02 | High | A failed capture clears the expected fingerprint |
| H-03 | High | Half-submitted write |
| H-08 | High | The single-slot expected fingerprint is overwritten |
| M-11 | Medium | Ignore regex ReDoS |
| M-12 / M-21 / M-22 | Medium | ignore / ignore-next state and UI |
| M-19 / M-20 / M-25 / M-26 | Medium | Negative bitmap height, whole-copy ignore, rich text regex, default format name |
| L-08–L-11, L-17, L-22 | Low | Hash/bitmap/paths/allowlist/HTML/OpenClipboard |

### 6.2 Paste (`PasteController` + `MainWindow::PasteItem`)

| ID | Level | Summary |
|----|------|------|
| H-04 | High | Silent foreground failure after the clipboard has changed |
| H-05 | High | Lifetime of the asynchronous paste callback |
| H-09 | High | Window between writing the clipboard and grabbing focus |
| M-13 / M-24 | Medium | Modifier wait timeout / Shift blocking by mistake |
| M-14 | Medium | SendInput is not checked |
| M-16 | Medium | Stale paste target HWND |
| M-23 | Medium | pasteInProgress cleared too early |

### 6.3 Keyboard hook (`KeyboardHandler`)

| ID | Level | Summary |
|----|------|------|
| H-06 | High | CallNextHookEx(nullptr) |
| H-07 | High | Win key replay timing |
| M-15 | Medium | GetFocus hotkey capture |
| M-18 | Medium | No message when the hook fails to install |
| L-12 / L-20 / L-21 | Low | API naming; footer Enter; numeric keypad numbered paste |

### 6.4 Main window / history UI (`MainWindow` / `HistoryListControls` / `HistoryRenderer`)

| ID | Level | Summary |
|----|------|------|
| M-01 | Medium | Tray pause is not persisted |
| M-02 | Medium | Settings copy overwrites changes |
| M-03 | Medium | loadingList gets stuck and blocks pasting |
| M-04 | Medium | UI-blocking Invoke |
| L-03, L-14 | Low | DPI border / PeekMessage |

### 6.5 Storage and search (`Database` / `StorageWorker`)

| ID | Level | Summary |
|----|------|------|
| M-05 | Medium | Two connections, SQLITE_BUSY |
| M-10 | Medium | Whole-table search |
| M-11 | Medium | Search regex ReDoS |
| L-02, L-06 | Low | Stop drops callbacks / formats not updated |

### 6.6 Preview and thumbnails (`PreviewWorker` / `ThumbnailWorker`)

| ID | Level | Summary |
|----|------|------|
| C-01 | Critical | Destruction order |
| C-02 | Critical | Cross-thread UI |
| M-06 | Medium | No callback on cancel |
| M-07 | Medium | Silent COM failure |
| M-08 | Medium | Cross-thread HBITMAP |
| M-17 | Medium | Shutdown does not stop the preview |
| L-01 | Low | reposition stale owner |

### 6.7 Settings / tray / updates / startup

| ID | Level | Summary |
|----|------|------|
| M-09 | Medium | Registry and DB disagree |
| L-04 | Low | Tray mask |
| L-05 | Low | UpdateChecker join blocks |
| L-07 | Low | main exception path |
| L-13 | Low | NIM version |
| L-15–L-16, L-18–L-19 | Low | Constants/pin keys/WTL/tests |

---

## 7. Recommended fix priority (implementation order)

1. **Shutdown and the preview thread**: C-01, C-02, M-17, H-05 (remove the exit crash surface first)
2. **Clipboard echoes**: H-01 (order-independent fingerprint), H-02, H-03, H-08 (stop pastes from polluting history)
3. **Paste reliability**: H-04, H-09, M-13, M-14, M-16, M-23, M-24
4. **UI state consistency**: M-01, M-02, M-03, M-12, M-21, M-22
5. **Capture correctness**: M-19, M-20, M-25, M-26
6. **Performance and stutter**: M-04, M-10, M-11
7. **Remaining Medium/Low**: slot in according to product pain points (including L-20 footer Enter and L-21 numeric keypad)

---

## 8. Conclusion

The code shows clear engineering awareness in its single instance, the storage thread serializing SQLite, search generations, the owner-drawn history list, ignore rules, and more; it is not a rough prototype.

What most needs attention first: **the cross-thread lifecycle of the preview window**, **deduplicating self-written clipboard echoes (including enumeration order)**, **the clipboard already being changed when a paste fails**, and **races between asynchronous callbacks and exit**. These four categories show up directly as crashes, duplicate history, and pastes that "don't work".

This report is the result of a static audit; for H-01 / C-01 / H-04, a runtime confirmation with a minimal reproduction case is recommended before fixing (whether a new history row appears after pasting text, whether exit occasionally hangs, and whether pasting into a foreground-locked application only changes the clipboard).

---

## 9. Revision history

| Date | Description |
|------|------|
| 2026-11-03 | First version: main paths + combined UI/storage/preview audit |
| 2026-11-03 | Merged the dedicated clipboard supplement: H-01 makes order independence explicit; added H-08/H-09, M-19–M-26, L-20–L-22; verified that `SettingsWindow.rc` really has no `IDC_X_IGNORE_NEXT` control |
| 2026-11-03 | Added HTML anchors to every finding; fix progress is now tracked in `FIX_CHECKLIST.md` |

*Generation note: compiled from a static analysis of the repository source (including cross-checking with a dedicated sub-audit); if the code changes later, compare against the git history to see whether each finding in this report still applies.*
