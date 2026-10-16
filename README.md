# codeportal-cli
Codeportal-cli

## Screenshots

| Dark | Light |
| --- | --- |
| ![Welcome screen, dark](CodePortal/codeportal-cli/docs/screenshots/dark-1-welcome.png) | ![Welcome screen, light](CodePortal/codeportal-cli/docs/screenshots/light-1-welcome.png) |
| ![The agent working, steps shown live](CodePortal/codeportal-cli/docs/screenshots/dark-2-live.png) | ![Finished answer with web results and the / command menu](CodePortal/codeportal-cli/docs/screenshots/light-4-answer.png) |
| ![Expanded steps: plan, search and a diff](CodePortal/codeportal-cli/docs/screenshots/dark-3-steps.png) | ![Phone layout](CodePortal/codeportal-cli/docs/screenshots/dark-7-mobile-chat.png) |
| ![Terminal, dark](CodePortal/codeportal-cli/docs/screenshots/cli-dark.png) | ![Terminal, light](CodePortal/codeportal-cli/docs/screenshots/cli-light.png) |

## Headless mode and local API

CodePortal can run without its console interface, as a local server that other programs call:

```
codeportal.exe --headless --port 8765
```

It listens on `127.0.0.1` by default; `--bind 0.0.0.0` makes it reachable from other computers on the network (only do this on a network you trust). On first launch it creates an access token in `%APPDATA%\CodePortal\api.token`; every request must send it as `Authorization: Bearer <token>`. An API key must already be configured (start CodePortal once interactively to add one).

| Method | Route | Body | Result |
| --- | --- | --- | --- |
| GET | `/api/v1/status` | | provider, model, working directory, `busy` |
| POST | `/api/v1/ask` | `{"message": "...", "conversation": "id"}` | `answer`, `steps` (each tool call with its arguments and result), `cancelled`. Each conversation id has its own history (default `"default"`) |
| GET | `/api/v1/events?after=N` | | live activity: numbered events (`turn_start`, `model`, `thinking`, `text`, `tool_start`, `tool_end`, `turn_end`) after N. Without `after`, starts at the latest turn |
| POST | `/api/v1/cancel` | | stops the turn in progress |
| POST | `/api/v1/reset` | `{"conversation": "id"}` | clears that conversation, or every conversation without a body |

One turn runs at a time: `ask` returns `409` while another is in progress. The agent can read and edit files and run shell commands in the working directory, so anything that holds the token can change your project. Keep the token private.

Example with curl:

```
curl -H "Authorization: Bearer $(cat "$APPDATA/CodePortal/api.token")" \
     -H "Content-Type: application/json" \
     -d '{"message":"Add a comment at the top of src/main.cpp explaining what it does"}' \
     http://127.0.0.1:8765/api/v1/ask
```

### Chat page

`CodePortal/codeportal-cli/web/codeportal-chat.html` is a chat interface for the server. Open it in a browser, enter the server address (`http://127.0.0.1:8765` by default, or the address of the computer running CodePortal) and the token from `api.token`. Each answer shows the tool calls the agent made, with their inputs and results. The page is a single file with no dependencies. CodePortal also serves it itself: at startup it prints a link such as `http://127.0.0.1:8765/#token=...`. Open that link and the page connects by itself and keeps retrying until the server is up. The token is saved in the browser and removed from the address bar.

While the agent works, the page shows what it is doing as it happens: the current phase, thinking and text as they stream, each tool call with its arguments, its diff and its result, and a raw event log. The finished turn stays in the conversation with the same details.

## Agent tools

The agent calls these tools by itself. File changes can be undone (`/undo` in the terminal, or the agent's `undo_last_change`).

| Group | Tools |
| --- | --- |
| Read and explore | `read_file` (500 lines at a time, with `start_line`), `list_dir`, `tree`, `find_files` (wildcards), `search_files` (text or regex, like grep), `file_info` |
| Change files | `create_file`, `write_file`, `edit_file` (exact replacement), `replace_lines`, `insert_lines`, `append_file`, `copy_path`, `move_path`, `delete_file`, `create_dir`, `delete_dir`, `undo_last_change` |
| Run programs | `run_command` (cmd.exe), `run_powershell`, `start_process` (background, output logged to a file), `stop_process`, `list_processes`, `wait` |
| Web | `web_search` (DuckDuckGo, then Bing; no API key), `fetch_url` (page as readable text), `download_file` |
| Windows | `open_path` (open a file, folder or website), `clipboard_read`, `clipboard_write`, `system_info`, `get_env` |
| Agent | `calculate`, `update_plan` (a checklist shown on the chat page), `save_memory` (notes kept in `%APPDATA%\CodePortal\memory.md` and given to the agent in every conversation), `get_context` |

Safety: `delete_dir` refuses drive roots, your main user folders, system folders and the working folder; `get_env` never shows values whose names look like secrets (keys, tokens, passwords).
