<div align="center">

<img src="src/assets/app/maccy.png" alt="Maccy for Windows" width="120" height="120" />

# Maccy for Windows

A clipboard history tool for Windows, inspired by [Maccy](https://github.com/p0deje/Maccy) for macOS.

Forked from [halifox/maccy_for_windows](https://github.com/halifox/maccy_for_windows).

[![Platform: Windows x64](https://img.shields.io/badge/platform-Windows%20x64-0078D4)](#building-from-source)
[![Built with C++20](https://img.shields.io/badge/built%20with-C%2B%2B20-00599C)](#building-from-source)
[![Storage: SQLite](https://img.shields.io/badge/storage-SQLite-003B57)](#data-privacy-and-deletion)
[![License: MIT](https://img.shields.io/badge/license-MIT-lightgrey)](LICENSE)
[![No telemetry](https://img.shields.io/badge/telemetry-none-brightgreen)](#data-privacy-and-deletion)

[Features](#features) · [Installation](#installation) · [Building](#building-from-source) · [Data and privacy](#data-privacy-and-deletion)

</div>

---

> [!WARNING]
>
> **This project is not the official Windows version of Maccy, and it is not affiliated with, endorsed by, or authorized by the official Maccy project.**

> [!CAUTION]
>
> **About antivirus false positives:** to provide a global shortcut and paste across applications, this project uses `WH_KEYBOARD_LL` to listen to low-level keyboard events, and it may block keystrokes and then replay them with `SendInput`. `WH_KEYBOARD_LL + blocked keystrokes + SendInput replay` is exactly the heuristic that antivirus software commonly associates with keyloggers, automation tools, and trojans, so some antivirus products may report Maccy as a trojan or a high-risk program. The behavior itself is not malicious, but the pattern really does look similar. Download the program from [GitHub Releases](https://github.com/abd3lraouf-studios/maccy_for_windows/releases) and check the installer's SHA256 against the one listed on the GitHub Release page.

## Features

- Access clipboard history from the Windows system tray and a global shortcut;
- Save text, image, and file clipboard formats;
- Exact, fuzzy, regex, and mixed search;
- Exact search uses SQLite FTS5; images themselves are not part of text search;
- Pin items, edit pinned text, and ignore applications, clipboard formats, and regex content;
- Honor the Windows clipboard history markers, so content explicitly marked as not belonging in history is not saved;
- Show a Windows notification for every captured copy, with the source app's icon and name and a short text snippet, or a thumbnail for images;
- Check GitHub Releases manually or at startup; the app only announces a new version and never downloads or replaces itself;
- Local SQLite storage; clipboard contents are never uploaded.

## Screenshots

<div align="center">
  <img src="screenshot/screenshot_3.png" width="60%" />
  <img src="screenshot/screenshot_5.png" width="60%" />
</div>

## Differences from the original Maccy

- Maccy uses the macOS menu bar; this project uses the Windows system tray and a global shortcut.
- Maccy polls the clipboard on a timer; this project listens for the Windows `WM_CLIPBOARDUPDATE` event.
- Maccy mainly searches the generated item titles; this project also searches the saved text body and file paths.
- Maccy tries to generate image titles with OCR; this project does no OCR, and images are only saved and previewed as image items.
- Maccy uses the macOS Pasteboard and SwiftData; this project uses native Windows clipboard formats and SQLite BLOBs.
- This project has no macOS integrations such as Universal Clipboard, iCloud, or macOS App Intents.

## Memory usage

|  History items | Content                                  | Background memory |
|---------------:|------------------------------------------|------------------:|
|              0 | Fresh start, no history                  |           3.07 MB |
|            200 | Short text, 55 bytes each                |           3.99 MB |
|            200 | Long text, 50 KB each                    |          10.84 MB |
|            200 | Images, 800 × 600 pixels each            |          11.54 MB |
|            200 | Mix of short text, long text, and images |          12.04 MB |
|           1000 | Short text, 55 bytes each                |           8.38 MB |
|           1000 | Long text, 50 KB each                    |          36.05 MB |
|           1000 | Images, 800 × 600 pixels each            |          14.43 MB |
|           1000 | Mix of short text, long text, and images |          18.46 MB |

## Installation

Download the versioned NSIS installer from [GitHub Releases](https://github.com/abd3lraouf-studios/maccy_for_windows/releases) and check its SHA256 against the one listed on the GitHub Release page.

The NSIS installer installs for the current Windows user only, into `%LOCALAPPDATA%\Programs\maccy`, and needs no administrator rights; the Start menu shortcut and the sign-in startup entry also belong to the current user only. The final installer page offers to launch Maccy, and a desktop shortcut can be selected during installation.

To upgrade, use the same NSIS installer: open GitHub Releases from the app's update prompt, download the new installer, and run it. The installer closes the running Maccy, replaces the program files in the existing per-user installation folder, and keeps your clipboard history, settings, and sign-in startup entry. An older per-machine installation must first be uninstalled once by an administrator before the per-user version is installed.

By default, uninstalling keeps the clipboard history and settings in `%LOCALAPPDATA%\maccy`. To also delete the history, pinned items, settings, and other local Maccy data, tick the delete option on the uninstall page; this cannot be undone.

User-visible changes are recorded in [`CHANGELOG.md`](CHANGELOG.md).

## Copy notifications

Every time Maccy captures a copy, it shows a Windows notification with the source app's icon and name and a short snippet of the copied text, or a thumbnail when the copy is an image. Each new copy replaces the previous notification.

Maccy registers itself under **Settings > System > Notifications > Maccy**, where Windows controls whether these notifications are shown and whether they play a sound.

To build notifications, Maccy caches the most recent image thumbnail and the source apps' icons under `%LOCALAPPDATA%\maccy`, in `notification-images`, `notification-icons`, and `notification-icon.png`. The notification registration is stored under `HKCU\Software\Classes\AppUserModelId\Maccy.ClipboardHistory`; the uninstaller removes it.

## Building from source

Requirements:

- Windows x64;
- Visual Studio 2022 or 2026 (or their Build Tools) with the C++ desktop development workload and the ATL component (WTL is built on ATL);
- CMake 3.25 or later;
- Ninja;
- vcpkg, with `VCPKG_ROOT` pointing at its root folder (the vcpkg bundled with Visual Studio works);
- NSIS, with `makensis.exe` on `PATH` (only needed to build the installer).

Dependencies (SQLite with FTS5, Boost.Beast, Boost.JSON, Boost.Locale, OpenSSL, WTL) come from [`vcpkg.json`](vcpkg.json) and are installed by vcpkg on the first configure.

### Installer

From the repository root in a Visual Studio x64 Developer PowerShell, run this single command to configure and compile a Release build, then produce the NSIS installer and its SHA-256 checksum file:

```powershell
cmake -DVCPKG_ROOT=C:/dev/vcpkg -P cmake/package-x64.cmake
```

`VCPKG_ROOT` can be passed as shown or taken from the environment. The script must run from a Developer PowerShell (it needs `VSINSTALLDIR`). The version is read from `version.txt` in the repository root by default; pass `-DMACCY_VERSION="X.Y.Z"` to override it temporarily. The script uses the `windows-x64-release` preset (triplet `x64-windows-static-md`), requires the MSVC runtime to be bundled, and writes the installer and `SHA256SUMS.txt` to `build/packages/`. The version number is written into both the program and the installer.

### Development builds

The [`CMakePresets.json`](CMakePresets.json) presets build with Ninja into `build/<preset>-vcpkg`:

```powershell
$env:VCPKG_ROOT = 'C:/path/to/vcpkg'
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug
ctest --preset windows-x64-debug --output-on-failure
```

Use `windows-x64-release` for a Release build. See [`CONTRIBUTING.md`](CONTRIBUTING.md) for more details.

## Data, privacy, and deletion

Clipboard history can contain passwords, tokens, private text, images, and file paths that you may not want kept for long. By default the app stores its data in:

```text
%LOCALAPPDATA%\maccy\maccy.db
```

SQLite's `-wal` and `-shm` files are also part of the database. The storage currently has no application-level encryption; any process with access to the current Windows user's data may be able to read it. Configure ignore rules when you use password managers, banking information, or other sensitive data, and read the [privacy and data notes](PRIVACY.md).

The app contains no telemetry, no clipboard upload, and no background auto-download or auto-install service. The update check only requests public release information from this fork's GitHub Releases (`abd3lraouf-studios/maccy_for_windows`); when a new version is found, the app opens the browser only after you confirm, and never downloads or replaces the program on its own.

Copy notifications are shown locally by Windows; their cached thumbnail and app icons stay under `%LOCALAPPDATA%\maccy` (see [Copy notifications](#copy-notifications)).

Deleting history is not the same as a forensically secure wipe. For a thorough cleanup, exit the app and then delete the database and its WAL files as described in the [privacy and data notes](PRIVACY.md).

## License and third-party content

Except for files explicitly marked as third-party, the project's source code is licensed under the MIT License; see [`LICENSE`](LICENSE).

Third-party components and visual assets follow their own licenses; see [`THIRD_PARTY_NOTICES/README.md`](THIRD_PARTY_NOTICES/README.md) for their sources and distribution notes:

- Maccy icon and visual assets: MIT License, see [`THIRD_PARTY_NOTICES/Maccy-LICENSE.txt`](THIRD_PARTY_NOTICES/Maccy-LICENSE.txt);
- WTL, SQLite, Boost.Beast, Boost.JSON, Boost.Locale, OpenSSL, and their transitive dependencies: licenses and copyright notices are in the `licenses/<port>/copyright` folders of the release package; the exact file locations are listed in the third-party notes.

The Maccy name and logo still belong to their respective owners; this project's license does not grant any trademark or brand rights.

## Contributing

Improvement suggestions and pull requests are welcome. Before submitting, please read:

- the [contributing guide](CONTRIBUTING.md);
- the [security policy](SECURITY.md).

Do not post security vulnerabilities, passwords, tokens, or unredacted clipboard contents anywhere public.

## Acknowledgements

Thanks to [Alex Rodionov](https://github.com/p0deje) and the Maccy project for an excellent clipboard tool for macOS, and for the MIT license that lets the community study, modify, and reuse the software and its assets.

Thanks also to [halifox](https://github.com/halifox), the author of [maccy_for_windows](https://github.com/halifox/maccy_for_windows), which this fork is based on.
