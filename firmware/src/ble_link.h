// BLE link to the Mac relay (docs/spec/ble-protocol.md): the fallback transport when
// HTTP over Wi-Fi is not usable. The BLE stack runs in its own task; received
// messages wait in a mutex-guarded inbox until the app takes them.
#pragma once
#include <Arduino.h>

// watch -> Mac
static constexpr uint8_t BLE_HELLO = 0x01, BLE_START = 0x10, BLE_AUDIO = 0x11, BLE_END = 0x12,
                         BLE_CANCEL = 0x13, BLE_RESET = 0x14, BLE_HISTORY_REQ = 0x15;
// Mac -> watch
static constexpr uint8_t BLE_AUTH = 0x81, BLE_PARTIAL = 0x90, BLE_HEARD = 0x91, BLE_REPLY_PARTIAL = 0x92,
                         BLE_REPLY_DONE = 0x93, BLE_RESET_OK = 0x94, BLE_HISTORY = 0x95;

void bleBegin();   // init the stack and start advertising (voice mode only)
bool bleReady();   // a central is connected, subscribed and authenticated

// Sending blocks until every fragment is queued; false when the link is gone.
bool bleSend(uint8_t type, const uint8_t* data = nullptr, size_t len = 0);
bool bleStartAudio();                             // empties the inbox, resets ADPCM, sends START
bool bleSendAudio(const int16_t* pcm, size_t n);  // one AUDIO message; n must be even

// Takes the newest complete message of a Mac -> watch type (JSON text) if one
// arrived since the last take.
bool bleTake(uint8_t type, String& payload);
// Request / answer round trip (RESET -> RESET_OK, HISTORY_REQ -> HISTORY).
bool bleRequest(uint8_t req, uint8_t answer, String& payload, uint32_t timeoutMs);
