# codeportal-cli
Codeportal-cli

## Headless mode and local API

CodePortal can run without its console interface, as a local server that other programs call:

```
codeportal.exe --headless --port 8765
```

It listens on `127.0.0.1` only. On first launch it creates an access token in `%APPDATA%\CodePortal\api.token`; every request must send it as `Authorization: Bearer <token>`. An API key must already be configured (start CodePortal once interactively to add one).

| Method | Route | Body | Result |
| --- | --- | --- | --- |
| GET | `/api/v1/status` | | provider, model, working directory, `busy` |
| POST | `/api/v1/ask` | `{"message": "..."}` | `answer`, `steps` (each tool call with its arguments and result), `cancelled` |
| POST | `/api/v1/cancel` | | stops the turn in progress |
| POST | `/api/v1/reset` | | clears the conversation |

One turn runs at a time: `ask` returns `409` while another is in progress. The agent can read and edit files and run shell commands in the working directory, so anything that holds the token can change your project. Keep the token private.

Example with curl:

```
curl -H "Authorization: Bearer $(cat "$APPDATA/CodePortal/api.token")" \
     -H "Content-Type: application/json" \
     -d '{"message":"Add a comment at the top of src/main.cpp explaining what it does"}' \
     http://127.0.0.1:8765/api/v1/ask
```
