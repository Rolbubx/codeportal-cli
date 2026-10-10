# FTXUI (vendored)

[FTXUI](https://github.com/ArthurSonzogni/FTXUI) **v7.0.3**, MIT license (see `LICENSE`).

These are the official amalgamated files from the release asset
`ftxui-amalgamated.zip` (sha256 `63d13bb65747bdbf0edf04d83ce490708022371cf378f15fad8489249ea4fd9a`).
`src/ftxui_impl.cpp` compiles `ftxui.cpp` with the defines it needs (`UNICODE`, `_UNICODE`, `NOMINMAX`).

## Local patch

`ftxui.cpp`, in `App::Internal::FetchTerminalEvents` (Windows `KEY_EVENT`): consoles that ignore
`ENABLE_VIRTUAL_TERMINAL_INPUT` (legacy conhost, Wine) report Esc, the arrows, Home/End,
Page Up/Down, Insert and Delete only as virtual-key codes with no character, and FTXUI dropped them.
The patch translates those codes into the VT sequences FTXUI's parser understands.
Search for `CodePortal patch` to find it. Re-apply it when updating FTXUI.
