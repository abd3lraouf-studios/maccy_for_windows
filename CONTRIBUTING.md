# Contributing Guide

Thank you for your interest in Maccy for Windows.

This project is an independent clipboard history tool for Windows. It is inspired by Maccy for macOS, but it is not the official Windows version of Maccy and does not represent the official Maccy project. This repository is a fork of [halifox/maccy_for_windows](https://github.com/halifox/maccy_for_windows).

## Before you start

Before opening a pull request:

1. Search existing pull requests to avoid duplicate work.
2. Read the project's README, LICENSE, and SECURITY.md.
3. Never paste real clipboard history, passwords, tokens, personal file paths, or database files into anything public.
4. If the change touches data structures, the threading model, window lifecycles, clipboard behavior, or the user data format, consider opening a draft pull request that describes the approach before investing in the full change.

Report security vulnerabilities as described in SECURITY.md; never submit them publicly.

## What you can contribute

These kinds of contributions are welcome:

- fixes for reliably reproducible bugs;
- improvements to the build, test, and debugging workflow;
- improvements to clipboard, search, database, preview, and tray behavior;
- improvements to performance, memory usage, and stability;
- improvements to documentation and error messages;
- new features with a clear use case and a well-defined scope;
- reproduction tools and test cases that contain no sensitive data.

Keep each contribution to a single purpose where possible. Do not mix unrelated renames, formatting, folder reorganization, or incidental code cleanup into the same pull request.

## Bug reports

When a pull request fixes a bug, its description should include as much of the following as possible:

- the app version or Git commit;
- the Windows version and system architecture;
- whether you used the installer or a local build;
- clear reproduction steps;
- the actual and expected results;
- how often it reproduces;
- redacted logs, screenshots, or screen recordings where needed;
- the relevant configuration, if the problem involves the clipboard, search, the database, or settings.

Before submitting, remove:

- clipboard text;
- passwords, tokens, and keys;
- real file contents;
- real user folders;
- unredacted database files;
- screenshots and logs that contain personal information.

## Development environment

The project currently uses:

- Windows;
- CMake 3.25 or later;
- C++20;
- the Visual Studio C++ toolchain (Visual Studio 2022 or 2026, or their Build Tools, with the C++ workload and the ATL component, which WTL needs);
- WTL;
- SQLite;
- vcpkg (the vcpkg bundled with Visual Studio works);
- Ninja or another available CMake generator.

The recommended setup is to set `VCPKG_ROOT` in a Visual Studio Developer PowerShell on Windows and then use the CMake presets in the repository:

```powershell
$env:VCPKG_ROOT = 'C:/path/to/vcpkg'
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug
ctest --preset windows-x64-debug --output-on-failure
```

Release build and packaging check:

```powershell
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
ctest --preset windows-x64-release --output-on-failure
cpack --config .\build\windows-x64-release-vcpkg\CPackConfig.cmake -G NSIS
```

The first configure installs WTL, SQLite with FTS5 enabled, and the other dependencies listed in `vcpkg.json` at the repository root. The presets use the `x64-windows-static-md` triplet and write build files to a separate `build/<preset>-vcpkg` folder.

If you use a different CMake generator, Visual Studio version, configuration, or architecture, say so in the pull request. Release builds must use a clean Release build folder; never distribute Debug build output.

Do not commit build folders, generated executables, debug symbols, installers, database files, or local configuration files. The repository's `.gitignore` covers the common outputs, but still check `git status` before committing.

## Testing

The project does not currently have a complete automated CTest suite. After a change, at least do the manual verification relevant to it.

CTest currently mainly verifies the build; until more tests are added, a passing `ctest` does not mean the clipboard, the database, or window lifecycles are fully covered.

Verify these areas in addition when a change touches them:

- the tray icon and window close behavior;
- the global shortcut;
- search, filtering, and sorting;
- pinning, deletion, clearing, and the capacity limit;
- text, image, and file clipboard formats;
- persistence of history and settings across restarts;
- high DPI and different window positions;
- long text, large images, and rapid consecutive copies;
- error handling when the database fails or is locked.

## Code and architecture conventions

- Use C++20 and the project's existing naming, ownership, and error handling style.
- Prefer putting fixed dialog layouts, static control text, and menus in `.rc` resources; keep runtime state and dynamic layout in C++.
- The UI thread must not do unpredictable database, BLOB, or heavy preview work.
- SQLite writes must go through the existing storage worker thread and transaction model.
- Do not bypass StorageWorker to share a SQLite connection directly across threads.
- When changing history data, keep format data, the search index, pin state, and deletion rules consistent.
- Load clipboard BLOBs only when actually needed; avoid reading large objects for no reason.
- Asynchronous search and preview must correctly handle cancellation, stale results, and window destruction.
- Hiding the window, staying in the tray, and explicitly exiting are different lifecycle behaviors; do not conflate them.
- Do not mask a root cause with duplicated logic, temporary switches, dead code, or special-case branches.
- If a change alters the database schema, the persistence format, or upgrade behavior, state the compatibility and data impact explicitly in the pull request.

## Third-party code and assets

Do not submit code, fonts, images, icons, or other assets of unknown origin or with an incompatible license.

When adding third-party content, also state:

- its source;
- its version or commit;
- its original license;
- whether its copyright notice must be kept;
- whether THIRD_PARTY_NOTICES needs to be updated.

Do not imply that this project has official authorization for the Maccy trademark, logo, or project.

## Pull requests

A pull request should include:

- the purpose of the change;
- how it is implemented;
- the scope of its impact;
- potential risks;
- the builds and tests that were run;
- screenshots or screen recordings for UI changes;
- a description of database, storage, or migration changes;
- a description of third-party content and license changes.

The PR title and the title of every commit in the PR must follow the Conventional Commits format. Maintainers review the commit format; merge with squash and keep the conforming PR title as the squash commit title.

Opening a pull request does not guarantee that it will be merged. Maintainers may ask you to narrow the scope, rewrite the implementation, or add tests, or may close the change if it does not fit the project's direction.

## Automated releases and commit messages

Releases are managed by Release Please. After a PR is merged into `master`, only `feat`, `fix`, or commits with a breaking-change marker produce a release PR:

| Commit type | Version change | Example |
| --- | --- | --- |
| `fix` | Patch | `fix(search): correct empty-result recovery` |
| `feat` | Minor | `feat(search): add history filters` |
| `!` on any type, or a `BREAKING CHANGE:` footer | Major | `feat!: replace the storage format` |
| `docs`, `ci`, `chore`, `build`, `test`, `refactor`, `style`, `perf` | No release of their own | `docs(guide): clarify setup` |

Regular PR titles and every commit title must follow this format. Mark a breaking change with `!` after the type, or add `BREAKING CHANGE: ...` to the commit body. Do not add `BREAKING CHANGE:` to commits that only contain documentation, CI, or maintenance work, unless the change really affects user compatibility.

Release Please updates `version.txt`, `vcpkg.json`, and `CHANGELOG.md` on `master` and opens a release PR for the maintainers to review and merge. PRs no longer run Windows CI; builds and tests run in the release workflow. After the merge, Release Please creates the `vX.Y.Z` tag and a draft release, and directly calls the reusable `build.yml` to build, test, and produce the x64 NSIS installer and the SHA-256 file; the release is published only after the checksummed assets have been uploaded. If the build fails, you can run the Windows release workflow manually and enter the tag of the existing draft release to retry; leaving it empty only builds and uploads the installer and checksum file as an Actions artifact, without publishing a release. Documentation, CI, or maintenance commits do not create a release PR on their own.

Releases use the repository's built-in `GITHUB_TOKEN`; no PAT is needed. Maintainers must allow GitHub Actions to create pull requests in Settings → Actions → General. If branch protection or a ruleset still requires `Conventional Commits`, `Debug x64`, `Release x64`, `Debug x86`, or `Release x86`, remove these retired required checks. Dependabot checks for GitHub Actions updates weekly and opens at most 10 PRs at a time.


To let release PRs merge without manual approval while keeping approval and required CI for regular PRs, put the approval requirement in a separate branch ruleset that contains only approval rules, and add the automation app only to that ruleset's pull-request-only bypass; keep the required status checks in branch protection or in another ruleset that has no bypass for that app. If you currently use classic branch protection's required approvals, first move the approval requirement into this separate ruleset. Never give the app a bypass on a ruleset that contains CI checks; if the repository settings cannot separate approvals from checks, keep the existing protection and accept that release PRs need manual approval.

## Releasing

The version tag, draft release, build, tests, installer and SHA-256 upload, and publishing the release are all done automatically by the process above. You still need to complete the manual post-release install and run checks in [`RELEASE_CHECKLIST.md`](RELEASE_CHECKLIST.md). At a minimum, confirm that:

- the Release x64 build succeeded;
- CTest and a manual smoke test have been completed;
- the NSIS installer installs and launches in a clean environment;
- the project license and all third-party licenses are included;
- `version.txt`, the version tag, the CMake version, the installer file name, and the version in the app's About page match;
- the release notes include known limitations, data impact, and the SHA-256 checksums.

## Contribution license

By submitting a contribution, you confirm that:

1. you have the right to submit it;
2. it does not knowingly include unlicensed third-party content;
3. you agree that it may be published under this repository's MIT License;
4. you keep the copyright to your contribution;
5. if your contribution is subject to an employer or another party, you have obtained the necessary permission.

This file does not replace LICENSE and does not change the individual licenses of third-party components.
