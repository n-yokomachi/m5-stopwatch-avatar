"""BLE link to the StopWatch (docs/spec/ble-protocol.md): the Mac side, a bleak central.

The watch uses this instead of HTTP when it has no Wi-Fi. Every message is handled
with the same functions as the HTTP handler (transcribe, AudioStream, start_job,
claude, load_history), passed in as the relay module so this file has no import
cycle with relay.py (which runs as __main__).
"""

import array
import asyncio
import hashlib
import hmac
import json
import sys
import threading
import time
from types import ModuleType

SERVICE_UUID = "7c3e0001-5a1d-4b8e-9f3a-2d6c1e0b7a01"
TX_UUID = "7c3e0002-5a1d-4b8e-9f3a-2d6c1e0b7a01"  # notify, watch -> Mac
RX_UUID = "7c3e0003-5a1d-4b8e-9f3a-2d6c1e0b7a01"  # write, Mac -> watch

# watch -> Mac
HELLO, START, AUDIO, END, CANCEL, RESET, HISTORY = 0x01, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15
# Mac -> watch
AUTH, PARTIAL, HEARD, REPLY_PARTIAL, REPLY_DONE, RESET_OK, HISTORY_ITEMS = (
    0x81, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95,
)
LAST = 0x01  # flags bit 0: last fragment of the message
HEADER = 2  # type + flags
ATT_OVERHEAD = 3
MAX_MESSAGE = 1024 * 1024  # a garbled stream must not grow a buffer forever

SCAN_TIMEOUT = 10.0
RETRY_DELAY = 3.0
POLL = 0.1  # how often partial transcripts and Claude replies are checked


# --- IMA ADPCM -------------------------------------------------------------

STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
)
INDEX_TABLE = (-1, -1, -1, -1, 2, 4, 6, 8) * 2


def _diff(step: int, nibble: int) -> int:
    d = step >> 3
    if nibble & 4:
        d += step
    if nibble & 2:
        d += step >> 1
    if nibble & 1:
        d += step >> 2
    return -d if nibble & 8 else d


# Precomputed per (index, nibble) so the per-sample loop is two lookups and a clamp.
_DIFF = [[_diff(STEP_TABLE[i], n) for n in range(16)] for i in range(89)]
_NEXT = [[min(88, max(0, i + INDEX_TABLE[n])) for n in range(16)] for i in range(89)]


class ImaAdpcmDecoder:
    """Standard IMA/DVI ADPCM, low nibble first; state carries across packets."""

    def __init__(self) -> None:
        self.predictor = 0
        self.index = 0

    def decode(self, data: bytes) -> bytes:
        """ADPCM bytes -> 16-bit little-endian PCM (two samples per byte)."""
        out = array.array("h", bytes(4 * len(data)))
        p, idx = self.predictor, self.index
        diff, nxt = _DIFF, _NEXT
        k = 0
        for b in data:
            for n in (b & 0x0F, b >> 4):
                p += diff[idx][n]
                if p > 32767:
                    p = 32767
                elif p < -32768:
                    p = -32768
                idx = nxt[idx][n]
                out[k] = p
                k += 1
        self.predictor, self.index = p, idx
        if sys.byteorder != "little":
            out.byteswap()
        return out.tobytes()


# --- packets ---------------------------------------------------------------


def fragment(msg_type: int, payload: bytes, mtu: int) -> list[bytes]:
    """Split one message into packets of at most mtu - 3 bytes (one ATT write each)."""
    size = max(1, mtu - ATT_OVERHEAD - HEADER)
    chunks = [payload[i : i + size] for i in range(0, len(payload), size)] or [b""]
    return [
        bytes((msg_type, LAST if i == len(chunks) - 1 else 0)) + c
        for i, c in enumerate(chunks)
    ]


class Reassembler:
    """Concatenates fragments of one message until the last-fragment flag."""

    def __init__(self) -> None:
        self.type: int | None = None
        self.buf = bytearray()

    def feed(self, packet: bytes) -> tuple[int, bytes] | None:
        if len(packet) < HEADER:
            return None
        t, flags = packet[0], packet[1]
        if self.type is not None and t != self.type:
            # senders never interleave types, so this is a lost last fragment
            print(f"ble: dropped incomplete message 0x{self.type:02x}", flush=True)
            self.buf.clear()
        self.type = t
        self.buf += packet[HEADER:]
        if len(self.buf) > MAX_MESSAGE:
            print(f"ble: dropped oversized message 0x{t:02x}", flush=True)
            self.type, self.buf = None, bytearray()
            return None
        if not flags & LAST:
            return None
        msg = (t, bytes(self.buf))
        self.type, self.buf = None, bytearray()
        return msg


def auth_response(token: str, nonce: bytes) -> bytes:
    return hmac.new(token.encode(), nonce, hashlib.sha256).digest()


def _json(obj: dict) -> bytes:
    return json.dumps(obj, ensure_ascii=False).encode()


# --- connection ------------------------------------------------------------


class Connection:
    """State of one connected watch: incoming messages, one ordered sender."""

    def __init__(self, relay: ModuleType, client, rx_char) -> None:
        self.relay = relay
        self.client = client
        self.rx = rx_char
        self.no_response = "write-without-response" in rx_char.properties
        self.outbox: asyncio.Queue[tuple[int, bytes]] = asyncio.Queue()
        self.incoming = Reassembler()
        self.stream = None  # relay.AudioStream of the recording in progress
        self.decoder: ImaAdpcmDecoder | None = None
        self.stream_error = ""
        self.tasks: set[asyncio.Task] = set()
        self.partial_task: asyncio.Task | None = None
        self.reply_task: asyncio.Task | None = None

    def spawn(self, coro) -> asyncio.Task:
        task = asyncio.get_running_loop().create_task(coro)
        self.tasks.add(task)
        task.add_done_callback(self._task_done)
        return task

    def _task_done(self, task: asyncio.Task) -> None:
        self.tasks.discard(task)
        if not task.cancelled() and task.exception():
            print(f"ble: task failed: {task.exception()!r}", flush=True)

    def send(self, msg_type: int, payload: bytes) -> None:
        self.outbox.put_nowait((msg_type, payload))

    async def sender(self) -> None:
        # the only writer, so fragments of different messages never interleave
        while True:
            msg_type, payload = await self.outbox.get()
            for packet in fragment(msg_type, payload, self.client.mtu_size):
                await self._write(packet)

    async def _write(self, packet: bytes) -> None:
        if self.no_response:
            try:
                await self.client.write_gatt_char(self.rx, packet, response=False)
                return
            except Exception as e:
                print(f"ble: write without response failed ({e!r}), using write with response", flush=True)
                self.no_response = False
        await self.client.write_gatt_char(self.rx, packet, response=True)

    def on_notify(self, _char, data: bytearray) -> None:
        msg = self.incoming.feed(bytes(data))
        if msg:
            self.handle(*msg)

    def handle(self, msg_type: int, payload: bytes) -> None:
        if msg_type == HELLO:
            self.send(AUTH, auth_response(self.relay.TOKEN, payload))
            print("ble: HELLO received, AUTH sent", flush=True)
        elif msg_type == START:
            self.start_stream(payload)
        elif msg_type == AUDIO:
            if self.stream and self.decoder:
                self.stream.add(self.decoder.decode(payload))
        elif msg_type == END:
            # taken now, not in the task, so a following START cannot be mixed in
            stream, error = self.stream, self.stream_error
            self.drop_stream()
            self.spawn(self.end_stream(stream, error))
        elif msg_type == CANCEL:
            self.drop_stream()
        elif msg_type == RESET:
            self.spawn(self.reset())
        elif msg_type == HISTORY:
            self.spawn(self.history())
        else:
            print(f"ble: unknown message 0x{msg_type:02x}", flush=True)

    def start_stream(self, payload: bytes) -> None:
        self.drop_stream()
        try:
            meta = json.loads(payload or b"{}")
            rate = int(meta.get("rate", 16000))
            codec = meta.get("codec", "ima-adpcm")
        except (ValueError, TypeError, AttributeError):
            rate, codec = 0, ""
        if not 8000 <= rate <= 48000:
            self.stream_error = "bad sample rate"
        elif codec != "ima-adpcm":
            self.stream_error = f"unsupported codec {codec!r}"
        else:
            self.stream = self.relay.AudioStream(rate)
            self.decoder = ImaAdpcmDecoder()
            self.partial_task = self.spawn(self.watch_partial(self.stream))
            return
        print(f"ble: START rejected: {self.stream_error}", flush=True)

    def drop_stream(self) -> None:
        if self.partial_task:
            self.partial_task.cancel()
        self.stream, self.decoder, self.partial_task, self.stream_error = None, None, None, ""

    async def watch_partial(self, stream) -> None:
        # AudioStream updates .partial from its own thread; push each change
        sent = ""
        while True:
            await asyncio.sleep(POLL)
            with stream.lock:
                text = stream.partial
            if text != sent:
                sent = text
                self.send(PARTIAL, _json({"text": text}))

    async def end_stream(self, stream, error: str) -> None:
        if not stream:
            self.send(HEARD, _json({"error": error or "unknown stream"}))
            return
        with stream.lock:
            pcm, rate = bytes(stream.pcm), stream.rate
        loop = asyncio.get_running_loop()
        t0 = time.time()
        try:
            text = await loop.run_in_executor(None, self.relay.transcribe, pcm, rate)
        except Exception as e:
            print(f"ble: transcription failed: {e!r}", flush=True)
            self.send(HEARD, _json({"error": f"transcription failed: {e}"}))
            return
        print(f"ble heard ({len(pcm) / 2 / rate:.1f}s audio, final stt {time.time() - t0:.2f}s): {text!r}", flush=True)
        job_id = self.relay.start_job(text) if text else ""
        self.send(HEARD, _json({"id": job_id, "text": text}))
        if job_id:
            if self.reply_task:  # the watch only follows the newest turn
                self.reply_task.cancel()
            self.reply_task = self.spawn(self.watch_reply(job_id))

    async def watch_reply(self, job_id: str) -> None:
        relay = self.relay
        sent = ""
        while True:
            with relay.state_lock:
                job = dict(relay.jobs.get(job_id) or {"status": "error", "reply": "不明なジョブです"})
                if job["status"] != "pending":
                    relay.jobs.pop(job_id, None)  # like GET /result
            if job["status"] != "pending":
                self.send(REPLY_DONE, _json({"status": job["status"], "reply": job.get("reply", "")}))
                return
            partial = job.get("partial", "")
            if partial and partial != sent:
                sent = partial
                self.send(REPLY_PARTIAL, _json({"partial": partial}))
            await asyncio.sleep(POLL)

    async def reset(self) -> None:
        loop = asyncio.get_running_loop()
        try:
            await loop.run_in_executor(None, self.relay.claude.reset)
            ok = True
        except Exception as e:
            print(f"ble: reset failed: {e!r}", flush=True)
            ok = False
        self.send(RESET_OK, _json({"ok": ok}))

    async def history(self) -> None:
        relay = self.relay

        def read() -> list[dict]:
            with relay.state_lock:
                return relay.load_history()

        items = await asyncio.get_running_loop().run_in_executor(None, read)
        self.send(HISTORY_ITEMS, _json({"items": items[-20:]}))

    def close(self) -> None:
        self.drop_stream()
        for task in list(self.tasks):
            task.cancel()


async def _session(relay: ModuleType) -> None:
    from bleak import BleakClient, BleakScanner

    device = await BleakScanner.find_device_by_filter(
        lambda d, ad: SERVICE_UUID in (u.lower() for u in ad.service_uuids),
        timeout=SCAN_TIMEOUT,
        service_uuids=[SERVICE_UUID],
    )
    if device is None:
        return
    disconnected = asyncio.Event()
    loop = asyncio.get_running_loop()
    async with BleakClient(
        device, disconnected_callback=lambda _c: loop.call_soon_threadsafe(disconnected.set)
    ) as client:
        rx = client.services.get_characteristic(RX_UUID)
        if rx is None or client.services.get_characteristic(TX_UUID) is None:
            print(f"ble: {device.name or device.address} lacks the TX/RX characteristics", flush=True)
            return
        conn = Connection(relay, client, rx)
        conn.spawn(conn.sender())
        print(f"ble: connected to {device.name or device.address} (mtu {client.mtu_size})", flush=True)
        try:
            await client.start_notify(TX_UUID, conn.on_notify)  # the watch answers with HELLO
            await disconnected.wait()
        finally:
            conn.close()
            print("ble: disconnected", flush=True)


async def _run(relay: ModuleType) -> None:
    last_error = ""
    while True:
        try:
            await _session(relay)
            last_error = ""
        except asyncio.CancelledError:
            raise
        except Exception as e:  # Bluetooth off or not permitted, connect timeout, link lost mid-setup
            if repr(e) != last_error:  # retried every few seconds: log each new error once
                last_error = repr(e)
                print(f"ble: {last_error}", flush=True)
        await asyncio.sleep(RETRY_DELAY)


def start(relay: ModuleType) -> threading.Thread:
    """Run the BLE central forever in a daemon thread with its own event loop."""
    thread = threading.Thread(target=lambda: asyncio.run(_run(relay)), name="ble", daemon=True)
    thread.start()
    return thread
