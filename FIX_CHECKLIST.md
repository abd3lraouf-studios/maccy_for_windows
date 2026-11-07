# Audit Fix Checklist

> **Purpose**: tracks the fix progress of each finding in [`AUDIT_LOGIC_REPORT.md`](AUDIT_LOGIC_REPORT.md).  
> **Details**: for the full description, root cause, and fix direction of each `ID`, follow the "Details" link to the audit anchor (for example [`C-01`](AUDIT_LOGIC_REPORT.md#C-01)).  
> **Update rules**: see `.cursor/rules/audit-fix-checklist.mdc`. Agents / Goals must update this file in step with their fixes.

## Status conventions

| Status | Meaning |
|------|------|
| `pending` | Not started |
| `in_progress` | Being fixed |
| `done` | Merged and self-checked |
| `deferred` | Intentionally postponed (the reason must be written in Notes) |
| `wontfix` | Will not fix (the product/technical reason must be written in Notes) |

## Progress summary

| Batch | Topic | pending | in_progress | done | deferred | wontfix |
|------|------|---------|-------------|------|----------|---------|
| P0 | Shutdown / preview thread affinity | 0 | 0 | 4 | 0 | 0 |
| P1 | Clipboard echo dedup | 0 | 0 | 4 | 0 | 0 |
| P2 | Paste reliability | 0 | 0 | 7 | 0 | 0 |
| P3 | UI / settings consistency | 0 | 0 | 6 | 0 | 0 |
| P4 | Capture correctness | 0 | 0 | 4 | 0 | 0 |
| P5 | Performance and stutter | 0 | 0 | 3 | 0 | 0 |
| P6 | Remaining Medium | 0 | 0 | 9 | 0 | 0 |
| P7 | Low / engineering debt | 0 | 0 | 22 | 0 | 0 |

> After changing an item's status, **recount every column of this table** (each ID counts once).

---

## Checklist maintenance protocol (for Agents / Goals)

1. Before starting: mark the target IDs `in_progress`; several items in one batch can run in parallel, but one Goal should preferably focus on one batch.
2. Read the details: open `AUDIT_LOGIC_REPORT.md#<ID>` and implement according to "Problem / Impact / Fix direction"; do not only fix the symptoms.
3. When fixed: status → `done`, fill in `Notes`; **create a git commit that follows Conventional Commits for that fix** (see "Commit conventions" below); several related IDs may share one commit, with every ID listed in the message.
4. Not doing it: `deferred` / `wontfix` + the reason in Notes; do not delete rows; do not create empty commits.
5. Related items: when fixing C-01, check M-17; when fixing H-01, check H-02/H-08; write "related" in Notes.
6. Keep the progress summary table consistent with the item statuses below.

### Commit conventions (Conventional Commits)

- After each fix (or each group of clearly related fixes) is merged, you must `git commit`, following Conventional Commits:
  - Format: `<type>(<scope>): <description>`
  - Common types: `fix` (defects), `refactor`, `perf`, `test`, `docs` (only for checklist/audit documents)
  - Scope examples: `clipboard`, `paste`, `preview`, `storage`, `hotkey`, `tray`, `settings`, `ui`
  - The description is an English imperative sentence with the audit ID, for example: `fix(clipboard): order-invariant paste echo fingerprint (H-01)`
- The body is optional: a short why; `Refs: H-01, H-02`
- **Commit once for every completed item (or group of related items)**; do not pile everything into one giant commit.
- Follow the repository's git safety conventions: do not change git config, no `--no-verify`, no force push; on Windows, pass the message with a PowerShell here-string.

---

## P0 — Shutdown / preview thread affinity

| ID | Status | Summary | Details | Notes |
|----|--------|---------|------|-------|
| C-01 | done | PreviewWorker not stopped before the main window is destroyed | [C-01](AUDIT_LOGIC_REPORT.md#C-01) | ExitApplication stops the preview before DestroyWindow |
| C-02 | done | Preview UI runs on a non-UI thread | [C-02](AUDIT_LOGIC_REPORT.md#C-02) | PreviewWindow belongs to the UI thread; PreviewWorker only loads/decodes |
| M-17 | done | Shutdown does not stop the preview (widens C-01) | [M-17](AUDIT_LOGIC_REPORT.md#M-17) | ApplicationController::Shutdown calls PreviewWorker::Stop |
| H-05 | done | Asynchronous `[this]` callbacks vs. exit | [H-05](AUDIT_LOGIC_REPORT.md#H-05) | m_exiting/m_shutdown guards; Shutdown clears the UI callback queue |

## P1 — Clipboard echo dedup

| ID | Status | Summary | Details | Notes |
|----|--------|---------|------|-------|
| H-01 | done | Fingerprint order + synthesized formats let paste echoes into history | [H-01](AUDIT_LOGIC_REPORT.md#H-01) | GetClipboardSequenceNumber suppresses self-writes; HashCapture is order-independent |
| H-02 | done | A failed capture clears the expected fingerprint | [H-02](AUDIT_LOGIC_REPORT.md#H-02) | A matching sequence number is skipped; open/capture failures no longer clear the suppression state |
| H-03 | done | Half-submitted SetClipboardData | [H-03](AUDIT_LOGIC_REPORT.md#H-03) | After a Set failure, EmptyClipboard and drop the self-write marker |
| H-08 | done | The single-slot expected fingerprint is overwritten | [H-08](AUDIT_LOGIC_REPORT.md#H-08) | Echoes are suppressed based on the latest clipboard sequence |

## P2 — Paste reliability

| ID | Status | Summary | Details | Notes |
|----|--------|---------|------|-------|
| H-04 | done | Silent foreground failure after the clipboard has changed | [H-04](AUDIT_LOGIC_REPORT.md#H-04) | Shows an error on PasteResult::Failed |
| H-09 | done | Window between writing the clipboard and grabbing focus | [H-09](AUDIT_LOGIC_REPORT.md#H-09) | Grab focus first, then WriteClipboard, then SendInput |
| M-13 | done | Modifier wait abandons the paste after a 3s timeout | [M-13](AUDIT_LOGIC_REPORT.md#M-13) | On timeout, still force one Ctrl+V |
| M-14 | done | SendInput return value is not checked | [M-14](AUDIT_LOGIC_REPORT.md#M-14) | Checks the SendInput count |
| M-16 | done | Paste target HWND stale/not captured | [M-16](AUDIT_LOGIC_REPORT.md#M-16) | Validate/recapture before pasting; report an error if invalid |
| M-23 | done | m_pasteInProgress cleared too early | [M-23](AUDIT_LOGIC_REPORT.md#M-23) | Kept until the deferred paste ends; cleared when the timer ends |
| M-24 | done | Shift treated as a blocking modifier | [M-24](AUDIT_LOGIC_REPORT.md#M-24) | Only Alt triggers the wait timer |

## P3 — UI / settings consistency

| ID | Status | Summary | Details | Notes |
|----|--------|---------|------|-------|
| M-01 | done | Alt+tray pause is not persisted | [M-01](AUDIT_LOGIC_REPORT.md#M-01) | ToggleIgnoreEvents→SetIgnorePolicy persists/refreshes in one place |
| M-02 | done | Settings window copy overwrites tray changes | [M-02](AUDIT_LOGIC_REPORT.md#M-02) | SyncOpenSettingsWindow syncs the snapshot after ignore changes |
| M-03 | done | m_loadingList gets stuck and blocks pasting | [M-03](AUDIT_LOGIC_REPORT.md#M-03) | Only the latest generation clears loading; loading is cleared when the latest result is rejected |
| M-12 | done | Scattered ignore state machine | [M-12](AUDIT_LOGIC_REPORT.md#M-12) | SetIgnorePolicy is the single entry point; related to M-01/M-02 |
| M-21 | done | ignore_next has no RC control | [M-21](AUDIT_LOGIC_REPORT.md#M-21) | Added IDC_X_IGNORE_NEXT + localization to the Advanced page |
| M-22 | done | ignore_next depends on ignore_events | [M-22](AUDIT_LOGIC_REPORT.md#M-22) | OnClipboardUpdate skips the one-shot independently |

## P4 — Capture correctness

| ID | Status | Summary | Details | Notes |
|----|--------|---------|------|-------|
| M-19 | done | CaptureBitmapAsDib rejects negative heights | [M-19](AUDIT_LOGIC_REPORT.md#M-19) | Size computed with abs(bmHeight) |
| M-20 | done | Ignoring one format drops the whole copy | [M-20](AUDIT_LOGIC_REPORT.md#M-20) | Non-ViewerIgnore formats only skip that format |
| M-25 | done | With save_text off, regex does not look at HTML/RTF | [M-25](AUDIT_LOGIC_REPORT.md#M-25) | Ignore probe + SearchPartsFromFormats |
| M-26 | done | Default ignored format name may fail to match | [M-26](AUDIT_LOGIC_REPORT.md#M-26) | Matches by registered name/alias/format ID |

## P5 — Performance and stutter

| ID | Status | Summary | Details | Notes |
|----|--------|---------|------|-------|
| M-04 | done | Synchronous Invoke blocks the UI thread | [M-04](AUDIT_LOGIC_REPORT.md#M-04) | Persist/MarkCopied/Delete/Pin hot paths are asynchronous; clearing history is still synchronous |
| M-10 | done | SearchHistory loads the whole table into memory | [M-10](AUDIT_LOGIC_REPORT.md#M-10) | List preview/search body truncated with substr |
| M-11 | done | User/search regex ReDoS | [M-11](AUDIT_LOGIC_REPORT.md#M-11) | RegexSearchBounded 64K haystack |

## P6 — Remaining Medium

| ID | Status | Summary | Details | Notes |
|----|--------|---------|------|-------|
| H-06 | done | CallNextHookEx(nullptr) | [H-06](AUDIT_LOGIC_REPORT.md#H-06) | Passes m_keyboardHook |
| H-07 | done | Delayed Win key replay timing | [H-07](AUDIT_LOGIC_REPORT.md#H-07) | Replay keeps the scan/extended flags |
| M-05 | done | Two SQLite connections, Storage+Preview | [M-05](AUDIT_LOGIC_REPORT.md#M-05) | busy_timeout=5s + GetItem retry |
| M-06 | done | Preview cancel does not call back into the UI | [M-06](AUDIT_LOGIC_REPORT.md#M-06) | Cancel/replace paths always PostResult |
| M-07 | done | Silent Thumbnail COM failure | [M-07](AUDIT_LOGIC_REPORT.md#M-07) | m_comAvailable; returns an empty result on failure |
| M-08 | done | Cross-thread HBITMAP painting | [M-08](AUDIT_LOGIC_REPORT.md#M-08) | CopyImage on the UI thread before painting |
| M-09 | done | Settings Save: DB and registry disagree | [M-09](AUDIT_LOGIC_REPORT.md#M-09) | SetLaunchAtLogin before Commit |
| M-15 | done | Fragile GetFocus in the LL hook | [M-15](AUDIT_LOGIC_REPORT.md#M-15) | Explicit SetHotkeyCaptureActive flag |
| M-18 | done | No message when the global hotkey fails to install | [M-18](AUDIT_LOGIC_REPORT.md#M-18) | MessageBox warning when the hook fails |

> Note: H-06/H-07 are High severity, but they are implemented in batch P6 (after paste/echo) so they do not compete for attention with P0–P2; a separate Goal can take them first.

## P7 — Low / engineering debt

| ID | Status | Summary | Details | Notes |
|----|--------|---------|------|-------|
| L-01 | done | Preview reposition stale owner | [L-01](AUDIT_LOGIC_REPORT.md#L-01) | C-02: reposition moved to the UI thread's MainWindow/PreviewWindow |
| L-02 | done | StorageWorker::Stop drops UI callbacks | [L-02](AUDIT_LOGIC_REPORT.md#L-02) | Shutdown drains first; Stop uses DrainUiCallbacks instead of clear |
| L-03 | done | Resize border is not DPI-aware | [L-03](AUDIT_LOGIC_REPORT.md#L-03) | OnNcHitTest MulDiv resize border |
| L-04 | done | Incorrect tray icon mask | [L-04](AUDIT_LOGIC_REPORT.md#L-04) | PNG alpha→1bpp AND mask |
| L-05 | done | UpdateChecker Start blocks on join | [L-05](AUDIT_LOGIC_REPORT.md#L-05) | join only reaps while holding the lock and when !m_checking |
| L-06 | done | Same fingerprint does not refresh formats/FTS | [L-06](AUDIT_LOGIC_REPORT.md#L-06) | SaveClipboard always calls ReplaceFormats/FTS |
| L-07 | done | main exception path relies on destruction order | [L-07](AUDIT_LOGIC_REPORT.md#L-07) | catch explicitly stops preview/storage |
| L-08 | done | 64-bit fingerprint collision risk | [L-08](AUDIT_LOGIC_REPORT.md#L-08) | Double FNV + metadata → 128-bit hex fingerprint |
| L-09 | done | CF_BITMAP→DIB palette/bit depth | [L-09](AUDIT_LOGIC_REPORT.md#L-09) | When GetDIBits fails, BitBlt into a 32bpp DIB section |
| L-10 | done | FilesPreview has no total length limit | [L-10](AUDIT_LOGIC_REPORT.md#L-10) | 4096-character cap + StoredPreview |
| L-11 | done | Allowlist mode drops an empty application | [L-11](AUDIT_LOGIC_REPORT.md#L-11) | Not ignored when the source process is unknown (allowed into history) |
| L-12 | done | RegisterGlobalHotKey does not do what its name says | [L-12](AUDIT_LOGIC_REPORT.md#L-12) | Renamed to InstallOpenHotkeyHook |
| L-13 | done | Tray does not NIM_SETVERSION | [L-13](AUDIT_LOGIC_REPORT.md#L-13) | NIM_SETVERSION_4 + OnTrayIcon parses by NIN_SELECT/HIWORD |
| L-14 | done | Limits of HasPendingKeyboardInput peeking | [L-14](AUDIT_LOGIC_REPORT.md#L-14) | Imm composition + IME message Peek |
| L-15 | done | Custom message ID gaps | [L-15](AUDIT_LOGIC_REPORT.md#L-15) | Constants.h WM_APP allocation table comment |
| L-16 | done | Fixed PinKeys set | [L-16](AUDIT_LOGIC_REPORT.md#L-16) | Added 0–9; a/q/v/w/z still excluded |
| L-17 | done | Crude HTML/RTF stripping | [L-17](AUDIT_LOGIC_REPORT.md#L-17) | Drops script/style, numeric entities; RTF \\u/\\'hh |
| L-18 | done | KeyboardHandler C callback style | [L-18](AUDIT_LOGIC_REPORT.md#L-18) | std::function Callbacks; removed the void* static trampolines |
| L-19 | done | Test coverage gaps | [L-19](AUDIT_LOGIC_REPORT.md#L-19) | CoreRulesTests adds strip/regex/ignore/pin digits |
| L-20 | done | Footer Enter does nothing | [L-20](AUDIT_LOGIC_REPORT.md#L-20) | Enter sends WM_COMMAND to activate the footer button |
| L-21 | done | Numeric keypad numbered paste | [L-21](AUDIT_LOGIC_REPORT.md#L-21) | VK_NUMPAD0–9 normalized to digit keys |
| L-22 | done | OpenClipboard has no NULL fallback | [L-22](AUDIT_LOGIC_REPORT.md#L-22) | ClipboardGuard falls back to nullptr when the owner fails |

---

## Goal prompt examples

### Fix every finding (main Goal)

Can be used directly as the `objective` when creating a Goal:

```text
Fix every audit finding in FIX_CHECKLIST.md (P0–P7, every pending/in_progress item) until every item in the checklist is done, deferred, or wontfix, and the progress summary counts are correct.

Working protocol:
1. Single source of truth: update progress and Status/Notes only in FIX_CHECKLIST.md; for each item, first read AUDIT_LOGIC_REPORT.md#<ID> (Problem/Impact/Fix direction), then change the code. Follow .cursor/rules/audit-fix-checklist.mdc, AGENTS.md, and WTL.md.
2. Proceed in batch order: P0 → P1 → P2 → P3 → P4 → P5 → P6 → P7. Within a batch, items may be combined by dependency (such as C-01+M-17; H-01+H-02+H-08). Do not skip a higher-priority batch to do Low items first, unless blocked and noted in Notes.
3. State machine: start fixing an ID → in_progress; merged and self-checked → done + Notes (one sentence on the key file or behavior); intentionally not doing it → deferred/wontfix + a mandatory reason. Never delete checklist rows. Recount the progress summary table at the start and end of each batch.
4. Every completed fix (or group of clearly related IDs) must get a git commit that follows Conventional Commits before starting the next item:
   - Format: <type>(<scope>): <description>
   - Prefer type=fix; scopes such as clipboard/paste/preview/storage/hotkey/tray/settings/ui
   - The description contains the audit ID, e.g.: fix(clipboard): order-invariant paste echo fingerprint (H-01)
   - Several related items may share one commit, with every Refs ID listed in the message/body
   - Include the Status/Notes/progress summary changes to FIX_CHECKLIST.md in the same commit (or in a docs commit right after it)
   - Do not change git config; no --no-verify; no force push; no empty commits; on Windows, pass the message with a PowerShell here-string
5. Root cause first, KISS/YAGNI; do not bring in new UI frameworks such as Qt/Electron; use WTL where WTL can do it. Avoid unrelated refactors.
6. After each batch: briefly report the completed IDs, the matching commit hashes, how they were verified, and this batch's deferred/wontfix items; then continue with the next batch until everything is wrapped up.
7. Once no item is pending/in_progress: read FIX_CHECKLIST.md end to end to confirm nothing was missed, with pending/in_progress at zero in the progress summary, and then end the Goal.
8. Compile the relevant targets where possible; describe the manual verification points for critical paths (exit/paste echoes/paste focus, etc.).
```

How to start it:

```text
Create a Goal with the following objective:
<paste the whole "Fix every finding" block above>
```

It is also recommended to attach: `@FIX_CHECKLIST.md` `@AUDIT_LOGIC_REPORT.md`

### Other optional Goals (narrow scope)

**A single batch, P0:**

```text
Following FIX_CHECKLIST.md, fix only P0. Read AUDIT_LOGIC_REPORT.md#ID; handle C-01 together with M-17; update Status/progress summary; do not expand into P1+.
```

**A single item:**

```text
Fix H-01 in FIX_CHECKLIST.md; details in AUDIT_LOGIC_REPORT.md#H-01; check H-02/H-08; update the checklist when done.
```

---

## Revision history

| Date | Description |
|------|------|
| 2026-11-03 | First version: every P0–P7 item, linked to the audit anchors; with Goal examples and the maintenance protocol |
| 2026-11-03 | The full Goal requires a Conventional Commits git commit for every fix |
