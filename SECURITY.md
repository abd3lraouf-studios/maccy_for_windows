# Security Policy

This project handles users' clipboard contents. Clipboard text, images, file paths, source windows, and database files must all be treated as sensitive data.

## Supported versions

Unless the release notes say otherwise, the project gives priority to:

- the latest released version;
- security issues in the current main branch;
- issues that can still be reproduced in a clean environment.

Older versions may not receive separate fixes.

## Reporting a vulnerability

Do not report security vulnerabilities publicly through pull requests, discussions, or chat.

Report them privately through GitHub Security Advisories on this fork:

[Report a security vulnerability](https://github.com/abd3lraouf-studios/maccy_for_windows/security/advisories/new)

You can also use the **Report a vulnerability** button on the repository's [Security](https://github.com/abd3lraouf-studios/maccy_for_windows/security) tab.

A report should ideally include:

- the affected version or commit;
- the impact of the vulnerability;
- minimal reproduction steps;
- any sample input that is needed;
- temporary mitigations;
- whether you want public credit.

Do not upload real clipboard databases, passwords, tokens, personal files, or other sensitive data.

## Local data handling

The app stores history and settings in the current Windows user's local application data folder:

```text
%LOCALAPPDATA%\maccy\maccy.db
```

SQLite's `maccy.db-wal` and `maccy.db-shm` may also exist. The same folder holds the cached notification thumbnail and source-app icons (`notification-images`, `notification-icons`, `notification-icon.png`). The project currently has no application-level database encryption, no telemetry upload, and no background auto-download or auto-install feature. Deleting records is an app feature and must not be understood as a secure wipe; to clean up sensitive data, exit the app and then delete the database and its WAL files.

Contributions and pull requests must not contain real clipboard contents. Reproduce problems with synthetic text, synthetic images, and redacted paths.

If the private vulnerability reporting form is unavailable, contact the maintainers of [abd3lraouf-studios](https://github.com/abd3lraouf-studios) privately first, and do not disclose vulnerability details publicly.

## How reports are handled

The maintainers first confirm whether the report is a security issue, then assess its impact, the fix, and the release timing.

Do not share vulnerability details with third parties before a fix or public statement. The maintainers will thank reporters where appropriate and give public credit with their consent.
