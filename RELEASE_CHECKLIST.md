# Release Checklist

This document is for maintainers to check the results of the automated release and the installer's behavior. The official release target is Windows x64; a successful automated build does not replace install-and-run verification in a clean environment.

## Before the release

- [ ] The working tree is clean and contains no databases, real clipboard contents, debug symbols, or local IDE configuration.
- [ ] The repository name, app name, update page, and every security reporting link are consistent and point to this fork (`abd3lraouf-studios/maccy_for_windows`).
- [ ] `version.txt`, the Git tag, the CMake project version, and the version in the app's About page match.
- [ ] Database, persistence, and third-party license changes in this release have been reviewed.
- [ ] README, SECURITY, PRIVACY, and CHANGELOG/release notes are updated.
- [ ] The release's known limitations and data impact are confirmed.

## Build and test

- [ ] The GitHub Actions x64/x86 Debug and Release CI checks pass.
- [ ] The release workflow's Windows x64 Release build succeeds and does not ship a Debug folder as the release package.
- [ ] The release workflow's `ctest --output-on-failure` completes, and the number of tests and their results are recorded.
- [ ] Start the program with a clean user folder and verify the tray, opening the window, exiting, and launch at sign-in.
- [ ] Verify text, images, file paths, long text, and rapid consecutive copies.
- [ ] Verify the copy notification: source app icon and name, text snippet, image thumbnail, and that turning Maccy off in Settings > System > Notifications silences it.
- [ ] Verify search, regex, fuzzy, pinning, deletion, clearing, and the capacity limit.
- [ ] Verify history, settings, and database files after a restart.
- [ ] Verify high DPI, multiple monitors, window position, and the system theme.
- [ ] Verify error handling when the database is locked, permissions are insufficient, or the app exits abnormally.

## Installer

- [ ] The NSIS installer is generated.
- [ ] Test install, launch, upgrade, and uninstall on a clean Windows environment without any development tools.
- [ ] The installer contains the project `LICENSE`, the README, the security notes, and all third-party notices.
- [ ] The installer contains only Release binaries and runtime libraries that may legally be redistributed.
- [ ] Check the executable's architecture, file version, icon, and installation folder.
- [ ] `SHA256SUMS.txt` is generated.
- [ ] If the build is signed, verify the Authenticode signature and timestamp.

## GitHub Release

- [ ] Release Please has created the `vX.Y.Z` tag and a draft release.
- [ ] The tag uses `vX.Y.Z` and matches the version in the build configuration exactly.
- [ ] The release notes describe new features, fixes, known issues, compatibility, and data impact.
- [ ] The release workflow has uploaded the NSIS installer and `SHA256SUMS.txt`, and published the release after the upload succeeded.
- [ ] Download the assets again from the release page and re-check the checksums.
- [ ] After publishing, verify again that the README and the in-app update check point to this repository.
