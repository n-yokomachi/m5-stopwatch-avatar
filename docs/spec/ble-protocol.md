# BLE link between the StopWatch and the Mac relay

The voice chat normally talks HTTP to `relay/relay.py` over Wi-Fi. When the watch has
no Wi-Fi (or the relay does not answer over HTTP), it uses this BLE link to the same
Mac instead. The Mac does the speech-to-text and talks to Claude exactly as for HTTP;
only the transport differs.

## Roles

- StopWatch: BLE peripheral (GATT server), advertises the service UUID below (the
  device name `<AVATAR_NAME>-StopWatch` is in the scan response). Accepts one central at a time and disconnects
  a central that has not authenticated within 10 s.
- Mac: BLE central (Python `bleak`, inside `relay.py`). Scans for the service UUID,
  connects, subscribes to TX, keeps the connection and reconnects when it drops.

## GATT

| Item | UUID | Properties | Direction |
|---|---|---|---|
| Service | `7c3e0001-5a1d-4b8e-9f3a-2d6c1e0b7a01` | | |
| TX | `7c3e0002-5a1d-4b8e-9f3a-2d6c1e0b7a01` | notify | watch -> Mac |
| RX | `7c3e0003-5a1d-4b8e-9f3a-2d6c1e0b7a01` | write, write without response | Mac -> watch |

The watch requests an MTU of 247. The Mac uses the MTU that macOS negotiates.

## Packets and fragments

Every notification / write is one packet:

```
byte 0   type
byte 1   flags: bit 0 = last fragment of this message (1) / more follow (0)
byte 2.. payload fragment
```

A message larger than one packet (MTU - 3 - 2 bytes of payload) is split into
consecutive packets of the same type; the receiver concatenates payloads until it
sees the last-fragment flag. Messages of different types are never interleaved by a
sender (each side sends from one task / one coroutine, in order). JSON payloads are
UTF-8.

## Authentication

The relay token (`RELAY_TOKEN`, the same value as for HTTP) is never sent over the
air.

1. When the Mac subscribes to TX, the watch sends `HELLO` with a fresh random 16-byte
   nonce.
2. The Mac answers `AUTH` with HMAC-SHA256(key = token as UTF-8 bytes, msg = nonce),
   32 bytes.
3. The watch compares; on success it is authenticated for this connection. On failure
   it disconnects. Before authentication the watch ignores every other write and
   never sends audio.

## Messages

Watch -> Mac (TX, notify)

| Type | Name | Payload |
|---|---|---|
| 0x01 | HELLO | 16-byte nonce |
| 0x10 | START | JSON `{"rate":16000,"codec":"ima-adpcm"}` - a new recording starts |
| 0x11 | AUDIO | IMA ADPCM bytes of the recording, in order |
| 0x12 | END | empty - recording finished, transcribe and ask Claude |
| 0x13 | CANCEL | empty - recording too short, drop it (no Claude turn) |
| 0x14 | RESET | empty - start a new StopWatch session (like POST /reset) |
| 0x15 | HISTORY | empty - request the history (like GET /history) |

Mac -> watch (RX, write)

| Type | Name | Payload |
|---|---|---|
| 0x81 | AUTH | 32-byte HMAC |
| 0x90 | PARTIAL | JSON `{"text": "..."}` - transcript so far, sent whenever it changes |
| 0x91 | HEARD | JSON `{"id": "...", "text": "..."}` like /stream/end; `id` empty = nothing recognised; or `{"error": "..."}` |
| 0x92 | REPLY_PARTIAL | JSON `{"partial": "..."}` - Claude's reply so far, sent whenever it grows |
| 0x93 | REPLY_DONE | JSON `{"status": "done" or "error", "reply": "..."}` |
| 0x94 | RESET_OK | JSON `{"ok": true}` or `{"ok": false}` |
| 0x95 | HISTORY | JSON `{"items": [{"heard","reply"}, ...]}` - same as GET /history |

The Mac pushes PARTIAL / REPLY_PARTIAL / REPLY_DONE by itself; the watch does not poll
over BLE.

## Audio codec: IMA ADPCM

16 kHz mono, 4 bits per sample (4:1 against 16-bit PCM, 8 KB/s), so the stream fits
BLE comfortably. Standard IMA/DVI ADPCM: step table of 89 entries, index table
`{-1,-1,-1,-1,2,4,6,8}` (mirrored), predictor and index start at 0 at every START and
carry across AUDIO packets. Two samples per byte, the first sample in the low nibble.
The watch sends AUDIO about every 200 ms while recording.

## Transport choice on the watch

- Wi-Fi connected: HTTP. If `/stream/start` fails and BLE is authenticated,
  that recording goes over BLE instead.
- No Wi-Fi: BLE if authenticated.
- Neither: the "x" eyes with "Wi-Fiにつながってないよ" while the yellow key is held.
