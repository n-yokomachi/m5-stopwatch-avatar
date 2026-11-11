"""Self-checks for ble_link.py (no Bluetooth, no Whisper, no Claude): uv run python -m pytest"""

import asyncio
import hashlib
import hmac
import json
import math
import threading
import types

import numpy

import ble_link as bl


def ima_encode(samples) -> bytes:
    """Reference IMA ADPCM encoder (standard algorithm, low nibble first)."""
    pred, idx = 0, 0
    nibbles = []
    for s in samples:
        step = bl.STEP_TABLE[idx]
        diff = s - pred
        n = 0
        if diff < 0:
            n, diff = 8, -diff
        d = step >> 3
        if diff >= step:
            n |= 4
            diff -= step
            d += step
        if diff >= step >> 1:
            n |= 2
            diff -= step >> 1
            d += step >> 1
        if diff >= step >> 2:
            n |= 1
            d += step >> 2
        pred = max(-32768, min(32767, pred - d if n & 8 else pred + d))
        idx = max(0, min(88, idx + bl.INDEX_TABLE[n]))
        nibbles.append(n)
    if len(nibbles) % 2:
        nibbles.append(0)
    return bytes(nibbles[i] | nibbles[i + 1] << 4 for i in range(0, len(nibbles), 2))


def test_adpcm_round_trip_across_packets():
    rate = 16000
    t = numpy.arange(rate) / rate
    src = (8000 * numpy.sin(2 * math.pi * 440 * t) + 3000 * numpy.sin(2 * math.pi * 1250 * t)).astype(int)
    enc = ima_encode(src.tolist())
    dec = bl.ImaAdpcmDecoder()
    # state must carry over packet boundaries (~200 ms packets, odd split on purpose)
    pcm = b"".join(dec.decode(enc[i : i + 1601]) for i in range(0, len(enc), 1601))
    out = numpy.frombuffer(pcm, dtype="<i2").astype(int)
    assert len(out) == len(src)
    err = out[400:] - src[400:]  # after the step size has adapted
    snr = 10 * math.log10((src[400:] ** 2).mean() / (err**2).mean())
    assert snr > 20, snr


def test_adpcm_known_nibbles():
    # low nibble 7 at index 0 (step 7): +0+7+3+1 = 11, index -> 8;
    # high nibble 0 at index 8 (step 16): +2 = 13, index -> 7
    dec = bl.ImaAdpcmDecoder()
    out = numpy.frombuffer(dec.decode(bytes([0x07])), dtype="<i2")
    assert out.tolist() == [11, 13]
    assert dec.index == 7
    dec = bl.ImaAdpcmDecoder()
    dec.decode(bytes([0x77]))
    assert dec.index == 16


def test_adpcm_clamps():
    dec = bl.ImaAdpcmDecoder()
    out = numpy.frombuffer(dec.decode(bytes([0x77]) * 200), dtype="<i2")
    assert out.max() == 32767
    dec = bl.ImaAdpcmDecoder()
    out = numpy.frombuffer(dec.decode(bytes([0xFF]) * 200), dtype="<i2")
    assert out.min() == -32768


def test_fragment_and_reassemble():
    payload = json.dumps({"text": "こんにちは" * 100}, ensure_ascii=False).encode()
    packets = bl.fragment(bl.PARTIAL, payload, 247)
    assert all(len(p) <= 244 for p in packets)
    assert [p[1] for p in packets] == [0] * (len(packets) - 1) + [1]
    r = bl.Reassembler()
    results = [r.feed(p) for p in packets]
    assert results[:-1] == [None] * (len(packets) - 1)
    assert results[-1] == (bl.PARTIAL, payload)


def test_fragment_empty_and_exact():
    assert bl.fragment(bl.END, b"", 247) == [bytes([bl.END, 1])]
    packets = bl.fragment(bl.AUDIO, bytes(242), 247)  # exactly one packet of payload
    assert len(packets) == 1 and packets[0][1] == 1
    assert len(bl.fragment(bl.AUDIO, bytes(243), 247)) == 2


def test_reassembler_drops_incomplete_message():
    r = bl.Reassembler()
    assert r.feed(bytes([bl.AUDIO, 0]) + b"abc") is None
    assert r.feed(bytes([bl.END, 1])) == (bl.END, b"")
    assert r.feed(b"\x11") is None  # too short


def test_auth_response():
    nonce = bytes(range(16))
    assert bl.auth_response("secret", nonce) == hmac.new(b"secret", nonce, hashlib.sha256).digest()


class FakeChar:
    properties = ["write", "write-without-response"]


class FakeClient:
    mtu_size = 23  # smallest MTU, so replies fragment

    def __init__(self):
        self.packets = []

    async def write_gatt_char(self, char, data, response):
        self.packets.append(bytes(data))


def fake_relay(transcript: str):
    relay = types.SimpleNamespace(TOKEN="tok", state_lock=threading.Lock(), jobs={}, calls=[])

    class Stream:
        def __init__(self, rate):
            self.rate, self.pcm, self.partial, self.lock = rate, bytearray(), "", threading.Lock()

        def add(self, data):
            self.pcm += data
            self.partial = "途中"
            return self.partial

    def start_job(text):
        relay.jobs["j1"] = {"status": "pending", "heard": text, "partial": "やっ"}
        return "j1"

    relay.AudioStream = Stream
    relay.transcribe = lambda pcm, rate: relay.calls.append(len(pcm)) or transcript
    relay.start_job = start_job
    relay.claude = types.SimpleNamespace(reset=lambda: relay.calls.append("reset"))
    relay.load_history = lambda: [{"heard": str(i), "reply": "r"} for i in range(30)]
    return relay


def messages(packets):
    r, out = bl.Reassembler(), []
    for p in packets:
        m = r.feed(p)
        if m:
            out.append((m[0], json.loads(m[1]) if m[0] != bl.AUTH else m[1]))
    return out


def test_connection_turn():
    async def run():
        relay = fake_relay("こんにちは")
        client = FakeClient()
        conn = bl.Connection(relay, client, FakeChar())
        conn.spawn(conn.sender())
        notify = lambda t, payload=b"": [conn.on_notify(None, bytearray(p)) for p in bl.fragment(t, payload, 247)]
        notify(bl.HELLO, bytes(16))
        notify(bl.START, b'{"rate":16000,"codec":"ima-adpcm"}')
        notify(bl.AUDIO, bytes(800))
        await asyncio.sleep(0.25)
        notify(bl.END)
        await asyncio.sleep(0.25)
        with relay.state_lock:
            relay.jobs["j1"].update(status="done", reply="やっほー")
        await asyncio.sleep(0.25)
        notify(bl.RESET)
        notify(bl.HISTORY)
        await asyncio.sleep(0.2)
        conn.close()
        return relay, messages(client.packets)

    relay, msgs = asyncio.run(run())
    assert msgs[0] == (bl.AUTH, bl.auth_response("tok", bytes(16)))
    assert (bl.PARTIAL, {"text": "途中"}) in msgs
    assert (bl.HEARD, {"id": "j1", "text": "こんにちは"}) in msgs
    assert (bl.REPLY_PARTIAL, {"partial": "やっ"}) in msgs
    assert (bl.REPLY_DONE, {"status": "done", "reply": "やっほー"}) in msgs
    assert (bl.RESET_OK, {"ok": True}) in msgs
    hist = [m for t, m in msgs if t == bl.HISTORY_ITEMS][0]["items"]
    assert len(hist) == 20 and hist[0]["heard"] == "10"
    assert relay.calls == [3200, "reset"]  # 800 ADPCM bytes -> 1600 samples
    assert relay.jobs == {}  # popped like GET /result
    types_in_order = [t for t, _ in msgs]
    assert types_in_order.index(bl.HEARD) < types_in_order.index(bl.REPLY_DONE)


def test_connection_end_without_voice_and_cancel():
    async def run():
        relay = fake_relay("")
        client = FakeClient()
        conn = bl.Connection(relay, client, FakeChar())
        conn.spawn(conn.sender())
        notify = lambda t, payload=b"": [conn.on_notify(None, bytearray(p)) for p in bl.fragment(t, payload, 247)]
        notify(bl.END)  # no START
        notify(bl.START, b'{"rate":16000,"codec":"ima-adpcm"}')
        notify(bl.CANCEL)
        notify(bl.START, b'{"rate":16000,"codec":"ima-adpcm"}')
        notify(bl.END)
        await asyncio.sleep(0.2)
        conn.close()
        return relay, messages(client.packets)

    relay, msgs = asyncio.run(run())
    assert msgs[0] == (bl.HEARD, {"error": "unknown stream"})
    assert msgs[1] == (bl.HEARD, {"id": "", "text": ""})
    assert relay.jobs == {}
