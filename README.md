# codeportal-cli
Codeportal-cli

## Build (Windows, MinGW-w64)

```bat
cd CodePortal\codeportal-cli
g++ -std=gnu++17 -O2 -pthread src\main.cpp src\ftxui_impl.cpp -o codeportal.exe -static -lwinhttp -lshell32
```

`build.bat` does the same with CLion's MinGW, and `CMakeLists.txt` works too. The interface uses
[FTXUI](https://github.com/ArthurSonzogni/FTXUI) 7.0.3, vendored in `third_party/ftxui` (nothing to install).

## Interface

- Full-screen: the conversation scrolls (mouse wheel, PgUp/PgDn, End), the agent's live output, status and
  progress bar stay above the input box.
- While the agent works you can type a message: it reads it at its next step. Esc interrupts.
- The agent can ask you questions (`ask_user`) without ending its turn, and show its plan with a progress bar
  (`update_plan`, `/plan`).
- Safeguards (`/safety`): the agent cannot change Windows or system folders, read credentials or CodePortal's
  API keys, or run destructive system commands; writing outside the working directory asks you first.
