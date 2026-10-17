# Changelog

All notable changes to CodePortal. Versions follow [Semantic Versioning](https://semver.org).
Each release on GitHub uses the matching section below as its notes.

## [1.4.0] - 2026-10-17

### Added
- `/web` opens the web chat in your browser from the terminal, already signed in. The server runs in the background and stops when you quit CodePortal.
- `/web phone` makes it reachable on your Wi-Fi and prints a QR code to scan with your phone. `/web status` and `/web stop` show or stop it.

## [1.3.1] - 2026-10-17

### Added
- Releases can be published from GitHub without git: **Actions → Release → Run workflow** creates the version tag and publishes the release.

## [1.3.0] - 2026-10-17

### Added
- **Installer** (`CodePortal-Setup-1.3.0.exe`): choose the install folder, add CodePortal to your PATH, and Start Menu and desktop shortcuts. CodePortal appears in "Apps & features" so you can uninstall it there. No administrator rights are needed.
- **Updates from GitHub**: `codeportal --update` (or `/update` inside CodePortal) downloads the latest release, checks its SHA-256 checksum and replaces the program. Use `codeportal --check` to only check.
- **Daily update check**: runs in the background, at most once a day, and the banner says when a new version is out. Turn it off with `/update auto off`.
- **Headless mode** with a local HTTP API (`codeportal --headless`), and a **web chat page** that shows live what the agent is doing.
- **27 new agent tools**: line-range reads, search, line edits, PowerShell, background processes, web search, fetching and downloading, clipboard, system info, plan and memory.
- `/compact [focus]`: summarizes the conversation to free up context.
- **Ctrl+O** shows a cut-off tool output in full without stopping the agent.
- A CodePortal look in the terminal and in the web page, plus an app icon and version details on the executable.
- `codeportal --help` and `codeportal --version`.

### Changed
- `codeportal.exe` is fully static: `libwinpthread-1.dll` is no longer needed.

### Fixed
- Answers in the web chat were sometimes cut off.
