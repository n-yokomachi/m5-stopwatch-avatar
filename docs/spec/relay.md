# Relay

`relay/relay.py` runs on the Mac. It receives audio from the StopWatch, transcribes it with mlx-whisper, sends the text to a resident Claude Code session and returns the reply. It serves HTTP for the Wi-Fi link and runs a BLE central for the BLE link (`relay/ble_link.py`, [ble-protocol.md](ble-protocol.md)). Both links use the same transcription and the same Claude session.

Start it in `relay/`:

```sh
uv run python relay.py
```

At start-up the relay loads the Whisper model with one second of silence, starts the Claude session and the BLE central, and then listens on `0.0.0.0` at `RELAY_PORT`. It exits when `RELAY_TOKEN` is empty.

## Settings

The relay reads these values from the environment. Values that are not set in the environment are taken from the repository's `.env`, then from `relay/.env`. Lines starting with `#` are ignored, matching quotes around a value are removed, and `\n` in a value becomes a newline.

| Setting | Default | Meaning |
|---|---|---|
| `RELAY_TOKEN` | none (required) | Shared secret. HTTP requests must send it in `X-Token`. The BLE link uses it as the HMAC key. |
| `RELAY_PORT` | `8765` | HTTP port. |
| `WHISPER_MODEL` | `mlx-community/whisper-small-mlx` | mlx-whisper model (Hugging Face repository or local path). Used for partial and final transcripts. |
| `CLAUDE_MODEL` | `claude-haiku-5-5` | Passed to `claude --model`. |
| `CLAUDE_PERMISSION_MODE` | `default` | Passed to `claude --permission-mode`. With `default`, tools that need a confirmation are refused, since nobody can confirm on the watch. With `bypassPermissions`, every tool runs without asking, so anyone with `RELAY_TOKEN` can run commands on the Mac. |
| `CLAUDE_CWD` | `relay/workspace` | Working directory of the Claude session. `~` is expanded. The relay's own files stay in `relay/workspace` regardless. |
| `CLAUDE_ADD_DIRS` | empty | Comma-separated folders passed as `--add-dir`. `~` is expanded. |
| `CLAUDE_OUTPUT_STYLE` | empty | Name of an output style. Passed as `--settings '{"outputStyle": "<name>"}'`. |
| `SYSTEM_PROMPT` | see [Claude session](#claude-session) | Text passed to `--append-system-prompt`. It replaces the relay's default text, not Claude Code's own system prompt. |

`WIFI_SSID`, `WIFI_PASS`, `RELAY_URL`, `TRANSPORT` and `AVATAR_NAME` in the same `.env` are build-time settings for the watch; see [firmware.md](firmware.md).

### Fixed values

| Item | Value |
|---|---|
| Maximum HTTP request body | 2 MiB |
| Turn timeout | 120 s |
| Turns kept in the history file | 50 |
| Turns returned by `/history` | 20 |
| Unfinished HTTP streams expire after | 120 s (removed when the next stream starts) |

## Speech-to-text

| Item | Value |
|---|---|
| Library | mlx-whisper (Apple Silicon) |
| Model | `WHISPER_MODEL` |
| Language | Japanese (`language="ja"`), fixed |
| `condition_on_previous_text` | `False` |
| Input | 16-bit little-endian mono PCM. Other sample rates are resampled to 16 kHz by linear interpolation. |
| Concurrency | one transcription at a time |

### Partial and final transcripts

- Partial: each uploaded chunk starts a transcription of all audio received so far, if none is running and at least 0.5 s of 16 kHz audio has arrived. The chunk's response carries the newest partial transcript that has finished, so it lags one run behind.
- Final: when the recording ends, all audio is transcribed once more. This text is what Claude receives.
- A final transcription that passes the voice check also writes the audio to `relay/workspace/last.wav`.

### Filters

| Filter | Rule | Result |
|---|---|---|
| Voice check | The audio is cut into 0.1 s frames. A frame with an int16 RMS of 300 or more counts as voiced. Fewer than 3 voiced frames (0.3 s) means no voice. | Empty transcript, Whisper is not run. |
| Click trim | The first 0.2 s, and for final transcripts of more than 0.6 s also the last 0.2 s, are left out of the voice check. Whisper still gets the whole recording. | Key clicks alone do not count as voice. |
| Hallucination | The text is 20 characters or shorter and contains one of `ご視聴ありがとうございました`, `ご清聴ありがとうございました`, `チャンネル登録`, `字幕`, `おやすみなさい`. | Empty transcript. |
| Repetition | A run of 2 to 15 characters that repeats three or more times in a row. | Kept once. |

An empty final transcript starts no Claude turn. The watch then returns to the idle eyes.

## Claude session

The relay keeps one `claude -p` process running and sends every turn to it over stdin. The process is started with:

| Flag | Value |
|---|---|
| `-p` | |
| `--input-format` | `stream-json` |
| `--output-format` | `stream-json` |
| `--include-partial-messages` | |
| `--verbose` | |
| `--model` | `CLAUDE_MODEL` |
| `--effort` | `low` |
| `--permission-mode` | `CLAUDE_PERMISSION_MODE` |
| `--setting-sources` | `project` (user settings such as `~/.claude/settings.json` and their hooks are not loaded) |
| `--append-system-prompt` | `SYSTEM_PROMPT` |
| `--add-dir` | once per entry of `CLAUDE_ADD_DIRS` |
| `--settings` | `{"outputStyle": "<CLAUDE_OUTPUT_STYLE>"}`, only when set |
| `--session-id` or `--resume` | the session ID, see below |

The process runs in `CLAUDE_CWD`, or in `relay/workspace` when that is not set. It uses the Claude Code login of the user who starts the relay.

The default `SYSTEM_PROMPT` is:

> This is a spoken conversation through an M5Stack StopWatch, a small device whose replies appear on a round screen. The user's words come from speech recognition and may contain mistakes: answer what they meant. Each message starts with [現在 ...], the local date and time added by the relay. Reply in the user's language in one or two short sentences. No Markdown, lists, tables, code or emoji.

### Session ID

| Item | Behaviour |
|---|---|
| File | `relay/workspace/.session_id` |
| No file | A new UUID is generated and passed as `--session-id`. |
| After the first successful turn | The ID is written to the file. |
| File present | The ID is passed as `--resume`, so the conversation continues across relay restarts. |

### Turns

- One turn runs at a time. Further turns wait.
- Each message is prefixed with the Mac's local time: `[現在 YYYY-MM-DD (Day) HH:MM] `, for example `[現在 2026-11-11 (Wed) 21:05] `.
- The reply text is collected from `text_delta` events while it streams. It is cleared at each new assistant message, so after a tool call only the text of the latest message is shown.
- The turn ends with the `result` event. The reply is the result text, or the streamed text, or `(エラー: <subtype>)`.
- Successful turns are appended to `relay/workspace/history.json` as `{"heard", "reply"}`. The file keeps the last 50 turns.
- If the process is not running when a turn arrives, it is started (and resumes the saved session).

| Failure | Reply text (status `error`) |
|---|---|
| No `result` within 120 s | `時間切れです`. The process is stopped (terminate, kill after 5 s). The next turn starts it again with `--resume`. |
| The process exits during a turn | `Claudeのプロセスが終了しました` |
| Writing to the process fails | `Claudeに送れませんでした: <error>` |
| Unknown job ID in `/result` | `不明なジョブです` |

### Reset

A reset (`POST /reset` or BLE `RESET`) stops the process, deletes `.session_id` and `history.json`, and starts a new session with a new ID.

## HTTP API

Every request must carry the header `X-Token` with the value of `RELAY_TOKEN`. Otherwise the relay answers `401 {"error": "unauthorized"}`.

Every `POST` reads `X-Sample-Rate` (default `16000`). A value outside 8000 to 48000 is answered with `400 {"error": "bad sample rate"}`. A body larger than 2 MiB is answered with `413 {"error": "bad length"}`. Audio bodies are 16-bit little-endian mono PCM.

Responses are JSON (`application/json; charset=utf-8`). The server speaks HTTP/1.1 with keep-alive. Unknown paths return `404 {"error": "not found"}`.

| Method and path | Request | Response |
|---|---|---|
| `POST /stream/start` | empty body | `{"sid": "<8 hex>"}` |
| `POST /stream/chunk?sid=<sid>` | next piece of PCM | `{"text": "<partial transcript>"}`, or `404 {"error": "unknown stream"}` |
| `POST /stream/end?sid=<sid>` | last piece of PCM (may be empty) | `{"id": "<job id>", "text": "<final transcript>"}`, or `404 {"error": "unknown stream"}`. `id` is empty when nothing was recognised. |
| `POST /talk` | whole recording as PCM | `{"id", "text"}` as for `/stream/end` |
| `GET /result?id=<job id>` | | `{"status": "pending", "partial": "..."}` while Claude is answering (`partial` appears once text has streamed), or `{"status": "done" or "error", "reply": "..."}` when finished. A finished result is removed after it has been read once. |
| `GET /history` | | `{"items": [{"heard": "...", "reply": "..."}, ...]}` with the last 20 turns of the current session |
| `POST /reset` | empty body | `{"ok": true}` after the new session has started |

The watch uses `/stream/start`, `/stream/chunk` (about every 400 ms), `/stream/end`, `/result` (about every 150 ms), `/history` and `/reset`. `/talk` is for testing.

### Dry run

With the header `X-Dry-Run: 1`, `/talk` and `/stream/end` return the transcript with an empty `id` and do not start a Claude turn.

```sh
curl -H "X-Token: $RELAY_TOKEN" -H "X-Dry-Run: 1" -H "X-Sample-Rate: 16000" \
  --data-binary @speech.raw http://localhost:8765/talk
```

`speech.raw` is 16-bit little-endian mono PCM without a header.

## BLE central

| Item | Value |
|---|---|
| Library | bleak |
| Scan | for the service UUID `7c3e0001-5a1d-4b8e-9f3a-2d6c1e0b7a01`, 10 s per attempt |
| Retry | 3 s after a failed scan or a lost connection |
| Push interval | partial transcripts and reply text are checked every 0.1 s and sent when they change |
| Maximum message | 1 MiB; larger reassembled messages are dropped |
| Errors | each new error (for example Bluetooth off or not permitted) is logged once; HTTP keeps working |

On macOS the app that starts the relay (Terminal, iTerm2 and so on) needs the Bluetooth permission. macOS asks on the first scan; the setting is in System Settings > Privacy & Security > Bluetooth.

The packet format, authentication and messages are in [ble-protocol.md](ble-protocol.md).
