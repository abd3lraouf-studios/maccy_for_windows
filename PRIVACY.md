# Privacy and Data

Maccy for Windows is a local clipboard history tool. It has to read the Windows clipboard to provide history, search, and paste.

## Data that is saved

Depending on the settings and the clipboard contents, the app may save:

- text bodies and text formats;
- image data;
- paths from file drop lists;
- clipboard format names and raw format data;
- the copy time, the copy count, and information about the source application;
- pinned items, ignore rules, and app settings.

By default the app uses this path:

```text
%LOCALAPPDATA%\maccy\maccy.db
```

SQLite WAL mode may additionally create:

```text
%LOCALAPPDATA%\maccy\maccy.db-wal
%LOCALAPPDATA%\maccy\maccy.db-shm
```

## Copy notifications

On every captured copy, Maccy shows a Windows notification with the source app's icon and name and a short snippet of the copied text, or a thumbnail when the copy is an image. The notification is shown locally by Windows; nothing is sent over the network. Each new copy replaces the previous notification.

To build these notifications, Maccy writes the following files:

```text
%LOCALAPPDATA%\maccy\notification-images\   (the most recent image thumbnail)
%LOCALAPPDATA%\maccy\notification-icons\    (cached icons of source applications)
%LOCALAPPDATA%\maccy\notification-icon.png  (Maccy's own notification icon)
```

The notification registration is stored in the registry under:

```text
HKCU\Software\Classes\AppUserModelId\Maccy.ClipboardHistory
```

Windows Settings > System > Notifications > Maccy turns these notifications, or their sound, off.

## Network behavior

The app contains no telemetry, no clipboard upload, no cloud sync, and no background auto-download or auto-install service. When you check for updates manually, or when "Check for updates automatically" is enabled and the app checks at startup, the app requests the GitHub Releases API of this fork (`abd3lraouf-studios/maccy_for_windows`) over HTTPS. It sends only the app version and the connection information that any network request requires; it never sends clipboard contents, history, or database data. When a new version is found, the app opens the GitHub Releases page only after you confirm, and never downloads or replaces the program on its own.

## Security boundaries

The database currently has no application-level encryption. Any process that can read the current Windows user's local data may be able to read the clipboard history. Do not treat this program as a password manager or a secure vault.

Recommendations:

- configure ignore rules for passwords, tokens, and other sensitive content;
- turn on Windows account and disk protection;
- never upload the database, WAL files, or real clipboard contents anywhere public;
- remove paths, text bodies, account names, and personal information before sharing logs and screenshots.

## Deletion and uninstallation

Delete, Clear, and Clear on exit in the app control the history during normal use. They do not promise a forensically secure wipe of the disk, especially while SQLite WAL files still exist.

By default the uninstaller keeps local data. To delete the clipboard history, pinned items, ignore rules, settings, the WAL/SHM files, and the cached notification images and icons when uninstalling, tick the data deletion option on the uninstall page. The data lives in `%LOCALAPPDATA%\maccy`; deletion cannot be undone and does not promise a forensically secure wipe. The uninstaller always removes the notification registration under `HKCU\Software\Classes\AppUserModelId\Maccy.ClipboardHistory`, whether or not the data is kept.

To clean up local data by hand:

1. Exit the app and make sure the tray icon is gone;
2. Delete `maccy.db`, `maccy.db-wal`, and `maccy.db-shm`, and, if you want, the `notification-images` and `notification-icons` folders and `notification-icon.png`;
3. If launch at sign-in is enabled, uninstalling the app removes the current Windows user's sign-in startup entry; keeping the data or only deleting the database by hand does not remove that entry.

Back up any history you want to keep before uninstalling the app or deleting the database.

## Security issues

Do not disclose security vulnerabilities publicly. Contact the maintainers through the private reporting process in [SECURITY.md](SECURITY.md).
