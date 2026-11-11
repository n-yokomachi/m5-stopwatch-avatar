"""M5StopWatch voice relay.

POST /talk    body: 16-bit LE mono PCM, header X-Sample-Rate. Transcribes with
              mlx-whisper, queues a turn to the resident Claude session, returns {"id", "text"}.
POST /stream/start            -> {"sid"}  (audio sent in pieces while recording)
POST /stream/chunk?sid=...    body: next PCM piece -> {"text": transcript so far}
POST /stream/end?sid=...      body: last PCM piece -> {"id", "text"} like /talk
Header X-Dry-Run: 1 on /talk or /stream/end returns the transcript without asking Claude.
GET  /result?id=...  -> {"status": "pending", "partial"?} | {"status": "done"|"error", "reply"}
GET  /history -> {"items": [{"heard", "reply"}, ...]} (last 20 turns of this session)
POST /reset   start a new StopWatch session (new session ID, history cleared).
Every request must carry header X-Token matching RELAY_TOKEN.
The same is available over BLE when the watch has no Wi-Fi (ble_link.py, docs/spec/ble-protocol.md).
"""

import json
import os
import subprocess
import sys
import threading
import time
import uuid
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import mlx_whisper
import numpy

import ble_link

ROOT = Path(__file__).resolve().parent
WORKSPACE = ROOT / "workspace"


def load_env() -> None:
    """Settings come from the repository's .env (see .env.example); relay/.env also works.
    Values may be quoted; \\n in a value becomes a newline."""
    for env in (ROOT.parent / ".env", ROOT / ".env"):
        if not env.exists():
            continue
        for line in env.read_text().splitlines():
            if "=" in line and not line.lstrip().startswith("#"):
                k, v = line.split("=", 1)
                v = v.strip()
                if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
                    v = v[1:-1]
                os.environ.setdefault(k.strip(), v.replace("\\n", "\n"))


load_env()

# whisper-small: about 0.3 s for a few seconds of speech on a MacBook Air M2 (8 GB).
# large-v3-turbo is more accurate but takes 1.2-2 s, and mlx-whisper reloads the
# weights whenever the model changes, so partial and final use the same model.
WHISPER_MODEL = os.environ.get("WHISPER_MODEL", "mlx-community/whisper-small-mlx")
PORT = int(os.environ.get("RELAY_PORT", "8765"))
MAX_BODY = 2 * 1024 * 1024
TURN_TIMEOUT = 120
MODEL = os.environ.get("CLAUDE_MODEL", "claude-haiku-5-5")
# "default" refuses anything that would need a confirmation (nobody can answer one on
# the watch). "bypassPermissions" runs every tool without asking: only on a machine and
# network you trust, since anyone with RELAY_TOKEN can then run commands on it.
PERMISSION_MODE = os.environ.get("CLAUDE_PERMISSION_MODE", "default")
EXTRA_DIRS = [d for d in os.environ.get("CLAUDE_ADD_DIRS", "").split(",") if d.strip()]
OUTPUT_STYLE = os.environ.get("CLAUDE_OUTPUT_STYLE", "")  # ~/.claude/output-styles/<name>.md
# Working directory of the Claude session (its CLAUDE.md and project files apply).
# The relay's own state (session id, history) always stays in WORKSPACE.
CLAUDE_CWD = Path(os.path.expanduser(os.environ.get("CLAUDE_CWD", ""))) if os.environ.get("CLAUDE_CWD") else None

DEFAULT_SYSTEM_PROMPT = (
    "This is a spoken conversation through an M5Stack StopWatch, a small device whose "
    "replies appear on a round screen. "
    "The user's words come from speech recognition and may contain mistakes: answer what they meant. "
    "Each message starts with [現在 ...], the local date and time added by the relay. "
    "Reply in the user's language in one or two short sentences. "
    "No Markdown, lists, tables, code or emoji."
)
SYSTEM_PROMPT = os.environ.get("SYSTEM_PROMPT", DEFAULT_SYSTEM_PROMPT)

TOKEN = os.environ.get("RELAY_TOKEN", "")

state_lock = threading.Lock()
jobs: dict[str, dict] = {}
HISTORY_LIMIT = 50


def history_file() -> Path:
    return WORKSPACE / "history.json"


def load_history() -> list[dict]:
    try:
        return json.loads(history_file().read_text())
    except (FileNotFoundError, json.JSONDecodeError):
        return []


def append_history(heard: str, reply: str) -> None:
    items = (load_history() + [{"heard": heard, "reply": reply}])[-HISTORY_LIMIT:]
    history_file().write_text(json.dumps(items, ensure_ascii=False))


class ClaudeSession:
    """One long-lived `claude -p` process in stream-json mode, bound to a fixed session ID.

    Keeping the process alive removes CLI start-up from every turn; the fixed ID
    keeps the StopWatch conversation in its own session across relay restarts.
    """

    def __init__(self) -> None:
        self.proc: subprocess.Popen | None = None
        self.turn_lock = threading.Lock()  # one turn in flight at a time
        self.current_job: str | None = None
        self.done = threading.Event()

    def _session_file(self) -> Path:
        return WORKSPACE / ".session_id"

    def _session_id(self) -> tuple[str, bool]:
        # The ID is saved only after the first successful turn: resuming an ID
        # that never got a transcript fails with "No conversation found".
        f = self._session_file()
        if f.exists():
            return f.read_text().strip(), True
        return str(uuid.uuid4()), False

    def start(self) -> None:
        sid, existing = self._session_id()
        cmd = [
            "claude", "-p",
            "--input-format", "stream-json",
            "--output-format", "stream-json",
            "--include-partial-messages",
            "--verbose",
            "--model", MODEL,
            "--effort", "low",
            "--permission-mode", PERMISSION_MODE,
            # user settings are skipped so hooks meant for interactive use never block a turn
            "--setting-sources", "project",
            "--append-system-prompt", SYSTEM_PROMPT,
        ]
        for d in EXTRA_DIRS:
            cmd += ["--add-dir", os.path.expanduser(d.strip())]
        if OUTPUT_STYLE:  # an output style from ~/.claude, without the rest of the user settings
            cmd += ["--settings", json.dumps({"outputStyle": OUTPUT_STYLE})]
        cmd += ["--resume", sid] if existing else ["--session-id", sid]
        self.sid = sid
        self.proc = subprocess.Popen(
            cmd, cwd=CLAUDE_CWD or WORKSPACE, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            text=True, bufsize=1,
        )
        threading.Thread(target=self._read, args=(self.proc,), daemon=True).start()
        print(f"claude session {sid} ({'resumed' if existing else 'new'}), model {MODEL}", flush=True)

    def reset(self) -> None:
        with self.turn_lock:
            self._stop()
            self._session_file().unlink(missing_ok=True)
            history_file().unlink(missing_ok=True)
            self.start()

    def _stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()

    def _read(self, proc: subprocess.Popen) -> None:
        for line in proc.stdout:
            try:
                ev = json.loads(line)
            except json.JSONDecodeError:
                continue
            job = self.current_job
            if not job:
                continue
            t = ev.get("type")
            if t == "stream_event":
                e = ev.get("event", {})
                if e.get("type") == "message_start":  # a new assistant message after a tool call
                    with state_lock:
                        jobs[job]["partial"] = ""
                d = e.get("delta", {})
                if d.get("type") == "text_delta":
                    with state_lock:
                        jobs[job]["partial"] = jobs[job].get("partial", "") + d.get("text", "")
            elif t == "result":
                ok = not ev.get("is_error")
                reply = ev.get("result") or jobs[job].get("partial") or f"(エラー: {ev.get('subtype')})"
                if ok and not self._session_file().exists():
                    self._session_file().write_text(self.sid)
                with state_lock:
                    jobs[job].update(status="done" if ok else "error", reply=reply)
                    if ok:
                        append_history(jobs[job].get("heard", ""), reply)
                print(f"[{job}] {ev.get('duration_ms')}ms: {reply[:80]!r}", flush=True)
                self.done.set()
        # process exited: fail the in-flight turn so the device does not wait forever
        if self.current_job:
            with state_lock:
                jobs[self.current_job].update(status="error", reply="Claudeのプロセスが終了しました")
            self.done.set()

    def ask(self, job_id: str, text: str) -> None:
        with self.turn_lock:
            if not self.proc or self.proc.poll() is not None:
                self.start()
            self.current_job = job_id
            self.done.clear()
            # The model has no clock: prefix the local time to each utterance.
            stamp = time.strftime("%Y-%m-%d (%a) %H:%M")
            msg = {"type": "user", "message": {"role": "user", "content": f"[現在 {stamp}] {text}"}}
            try:
                self.proc.stdin.write(json.dumps(msg, ensure_ascii=False) + "\n")
                self.proc.stdin.flush()
            except (BrokenPipeError, OSError) as e:
                with state_lock:
                    jobs[job_id].update(status="error", reply=f"Claudeに送れませんでした: {e}")
                self.current_job = None
                return
            if not self.done.wait(TURN_TIMEOUT):
                with state_lock:
                    jobs[job_id].update(status="error", reply="時間切れです")
                self._stop()  # next turn restarts and resumes the same session
            self.current_job = None


claude = ClaudeSession()


# Phrases Whisper emits on silence or noise (learned from video subtitles).
HALLUCINATIONS = ("ご視聴ありがとうございました", "ご清聴ありがとうございました", "チャンネル登録", "字幕", "おやすみなさい")
VOICE_RMS = 300        # int16 RMS of a 0.1 s frame that counts as voice
MIN_VOICED_FRAMES = 3  # less than 0.3 s of voice: silence (button clicks are single loud frames)
# MLX slows down badly when two transcriptions run at once, so they are serialised.
# A running partial (~0.3 s) is the most a final transcription waits for.
whisper_lock = threading.Lock()


def voiced_frames(samples: numpy.ndarray, rate: int) -> int:
    n = rate // 10
    frames = samples[: len(samples) // n * n].astype("float32").reshape(-1, n)
    return int((numpy.sqrt((frames**2).mean(axis=1)) >= VOICE_RMS).sum()) if frames.size else 0


def collapse_repeats(text: str) -> str:
    """Whisper sometimes loops on one phrase ("私のおかげで私のおかげで..."): keep one."""
    for size in range(2, 16):
        i = 0
        while i + size * 3 <= len(text):
            unit = text[i : i + size]
            if text[i + size : i + size * 3] == unit * 2:
                j = i + size
                while text[j : j + size] == unit:
                    j += size
                text = text[: i + size] + text[j:]
            i += 1
    return text


CLICK_TRIM_SEC = 0.2  # the button press/release click at both ends of a recording


def transcribe(pcm: bytes, rate: int, final: bool = True) -> str:
    samples = numpy.frombuffer(pcm, dtype="<i2")
    trim = int(rate * CLICK_TRIM_SEC)
    # clicks are left out of the voice check only; Whisper gets everything so word ends survive
    core = samples[trim:-trim] if final and len(samples) > 3 * trim else samples[trim:]
    voiced = voiced_frames(core, rate)
    if voiced < MIN_VOICED_FRAMES:
        if final:
            print(f"skipped: no voice ({voiced} voiced frames)", flush=True)
        return ""
    rms = voiced
    if final:  # kept for debugging the device microphone
        with wave.open(str(WORKSPACE / "last.wav"), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(pcm)
    audio = samples.astype("float32") / 32768.0
    if rate != 16000:
        idx = numpy.linspace(0, len(audio) - 1, int(len(audio) * 16000 / rate))
        audio = numpy.interp(idx, numpy.arange(len(audio)), audio).astype("float32")
    with whisper_lock:
        result = mlx_whisper.transcribe(
            audio, path_or_hf_repo=WHISPER_MODEL, language="ja", condition_on_previous_text=False
        )
    text = collapse_repeats(result.get("text", "").strip())
    if any(h in text for h in HALLUCINATIONS) and len(text) <= 20:
        if final:
            print(f"skipped: likely hallucination {text!r} ({rms} voiced frames)", flush=True)
        return ""
    return text


class AudioStream:
    """Audio arriving in chunks while the owner is still talking.

    Each chunk kicks off a transcription of everything received so far (unless
    one is already running), so the device can show the words as they come.
    """

    def __init__(self, rate: int) -> None:
        self.rate = rate
        self.pcm = bytearray()
        self.partial = ""
        self.busy = False
        self.created = time.time()
        self.lock = threading.Lock()

    def add(self, data: bytes) -> str:
        with self.lock:
            self.pcm += data
            start = not self.busy and len(self.pcm) >= self.rate  # >= 0.5 s of audio
            if start:
                self.busy = True
                snapshot = bytes(self.pcm)
            partial = self.partial
        if start:
            threading.Thread(target=self._partial, args=(snapshot,), daemon=True).start()
        return partial

    def _partial(self, pcm: bytes) -> None:
        text = transcribe(pcm, self.rate, final=False)
        with self.lock:
            if text:
                self.partial = text
            self.busy = False


streams: dict[str, AudioStream] = {}


def start_job(text: str) -> str:
    job_id = uuid.uuid4().hex[:8]
    with state_lock:
        jobs[job_id] = {"status": "pending", "heard": text}
    threading.Thread(target=claude.ask, args=(job_id, text), daemon=True).start()
    return job_id


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"  # keep-alive: the device reuses one connection

    def _send(self, code: int, obj: dict) -> None:
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _authorized(self) -> bool:
        if TOKEN and self.headers.get("X-Token") == TOKEN:
            return True
        self._body()
        self._send(401, {"error": "unauthorized"})
        return False

    def _body(self) -> bytes | None:
        length = int(self.headers.get("Content-Length", "0"))
        if length < 0 or length > MAX_BODY:
            return None
        return self.rfile.read(length) if length else b""

    def do_POST(self) -> None:
        if not self._authorized():
            return
        url = urlparse(self.path)
        sid = parse_qs(url.query).get("sid", [""])[0]
        body = self._body()
        if body is None:
            self._send(413, {"error": "bad length"})
            return
        try:
            rate = int(self.headers.get("X-Sample-Rate", "16000"))
        except ValueError:
            rate = 0
        if not 8000 <= rate <= 48000:
            self._send(400, {"error": "bad sample rate"})
            return
        dry_run = self.headers.get("X-Dry-Run") == "1"  # tests: transcribe only, no Claude turn

        if url.path == "/reset":
            claude.reset()
            self._send(200, {"ok": True})
        elif url.path == "/stream/start":
            now = time.time()
            with state_lock:
                for k in [k for k, v in streams.items() if now - v.created > 120]:
                    streams.pop(k)
                sid = uuid.uuid4().hex[:8]
                streams[sid] = AudioStream(rate)
            self._send(200, {"sid": sid})
        elif url.path == "/stream/chunk":
            stream = streams.get(sid)
            if not stream:
                self._send(404, {"error": "unknown stream"})
                return
            self._send(200, {"text": stream.add(body)})
        elif url.path in ("/stream/end", "/talk"):
            if url.path == "/talk":
                pcm = body
            else:
                with state_lock:
                    stream = streams.pop(sid, None)
                if not stream:
                    self._send(404, {"error": "unknown stream"})
                    return
                with stream.lock:
                    stream.pcm += body
                    pcm = bytes(stream.pcm)
            t0 = time.time()
            text = transcribe(pcm, rate)
            print(f"heard ({len(pcm) / 2 / rate:.1f}s audio, final stt {time.time() - t0:.2f}s): {text!r}", flush=True)
            self._send(200, {"id": start_job(text) if text and not dry_run else "", "text": text})
        else:
            self._send(404, {"error": "not found"})

    def do_GET(self) -> None:
        if not self._authorized():
            return
        url = urlparse(self.path)
        if url.path == "/history":
            with state_lock:
                items = load_history()
            self._send(200, {"items": items[-20:]})
            return
        if url.path != "/result":
            self._send(404, {"error": "not found"})
            return
        job_id = parse_qs(url.query).get("id", [""])[0]
        with state_lock:
            job = dict(jobs.get(job_id) or {"status": "error", "reply": "不明なジョブです"})
            job.pop("heard", None)
            if job["status"] != "pending":
                jobs.pop(job_id, None)
        self._send(200, job)

    def log_message(self, fmt, *args) -> None:
        pass


def main() -> None:
    if not TOKEN:
        raise SystemExit("RELAY_TOKEN is not set (.env at the repository root, see .env.example)")
    WORKSPACE.mkdir(exist_ok=True)
    print(f"loading {WHISPER_MODEL} ...", flush=True)
    mlx_whisper.transcribe(  # warm up so the first talk is not slow
        numpy.zeros(16000, dtype="float32"), path_or_hf_repo=WHISPER_MODEL, language="ja"
    )
    claude.start()
    ble_link.start(sys.modules[__name__])  # this module, not a second copy imported as "relay"
    print(f"relay listening on :{PORT}", flush=True)
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
