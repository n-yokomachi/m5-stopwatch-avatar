// BLE peripheral for the Mac relay; see docs/spec/ble-protocol.md.
// Uses the BLE library bundled with Arduino-ESP32 (NimBLE backend on the S3).
// Stack callbacks run in the NimBLE host task: they only touch the state below
// (under inboxMutex where the app reads it) and never call M5 APIs.
#include "ble_link.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include "host/ble_hs.h"
#include <mbedtls/md.h>
#include <esp_random.h>
#include <string>
#include "env_config.h"

#define LOGF(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)

static const char* SERVICE_UUID = "7c3e0001-5a1d-4b8e-9f3a-2d6c1e0b7a01";
static const char* TX_UUID = "7c3e0002-5a1d-4b8e-9f3a-2d6c1e0b7a01";
static const char* RX_UUID = "7c3e0003-5a1d-4b8e-9f3a-2d6c1e0b7a01";
static constexpr uint16_t MTU = 247;
static constexpr size_t RX_MAX = 64 * 1024;          // larger messages are dropped
static constexpr uint32_t AUTH_TIMEOUT_MS = 10000;  // a silent central must not hold the only slot
static constexpr uint32_t SEND_TIMEOUT_MS = 3000;   // per packet, while the stack has no buffers

static BLEServer* server;
static BLECharacteristic* txChr;
static SemaphoreHandle_t txMutex;     // one message at a time (fragments are never interleaved)
static SemaphoreHandle_t inboxMutex;

static volatile uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool subscribed = false, authed = false, helloPending = false;
static volatile uint32_t connectedAt = 0;
static uint8_t nonce[16];
static bool nonceValid = false;  // host task only; a nonce answers one AUTH

// host task only: the message being reassembled
static uint8_t rxType = 0;
static std::string rxBuf;

// newest complete message per Mac -> watch type 0x90..0x95 (guarded by inboxMutex)
static constexpr int INBOX_N = 6;
static String inbox[INBOX_N];
static bool inboxNew[INBOX_N];

static int inboxSlot(uint8_t type) { return type >= BLE_PARTIAL && type < BLE_PARTIAL + INBOX_N ? type - BLE_PARTIAL : -1; }

bool bleReady() { return connHandle != BLE_HS_CONN_HANDLE_NONE && subscribed && authed; }

// ---------- sending ----------
static size_t payloadSize(uint16_t conn) {
  uint16_t mtu = ble_att_mtu(conn);
  if (mtu < 23) mtu = 23;
  if (mtu > MTU) mtu = MTU;
  return mtu - 3 - 2;
}

// Retries while the host is out of buffers (audio can outrun the link briefly).
static bool sendPacket(uint16_t conn, const uint8_t* p, size_t n) {
  uint32_t t0 = millis();
  for (;;) {
    if (connHandle != conn) return false;
    os_mbuf* om = ble_hs_mbuf_from_flat(p, n);
    if (om) {
      int rc = ble_gatts_notify_custom(conn, txChr->getHandle(), om);  // consumes om
      if (rc == 0) return true;
      if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) { LOGF("ble: notify rc=%d\n", rc); return false; }
    }
    if (millis() - t0 > SEND_TIMEOUT_MS) { LOGF("ble: notify timed out\n"); return false; }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

bool bleSend(uint8_t type, const uint8_t* data, size_t len) {
  if (type == BLE_HELLO ? !subscribed : !bleReady()) return false;
  uint16_t conn = connHandle;
  xSemaphoreTake(txMutex, portMAX_DELAY);
  size_t maxPay = payloadSize(conn);
  uint8_t pkt[MTU];
  bool ok = true;
  size_t off = 0;
  do {
    size_t k = len - off < maxPay ? len - off : maxPay;
    pkt[0] = type;
    pkt[1] = off + k >= len ? 1 : 0;
    if (k) memcpy(pkt + 2, data + off, k);
    off += k;
    ok = sendPacket(conn, pkt, k + 2);
  } while (ok && off < len);
  xSemaphoreGive(txMutex);
  return ok;
}

// ---------- IMA ADPCM ----------
static const int16_t STEPS[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t INDEX_ADJ[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
static int adpcmPred = 0, adpcmIndex = 0;  // upload task only; reset at START

static uint8_t adpcmEncode(int16_t s) {
  int step = STEPS[adpcmIndex];
  int diff = s - adpcmPred;
  uint8_t code = 0;
  if (diff < 0) { code = 8; diff = -diff; }
  int vpdiff = step >> 3;
  if (diff >= step) { code |= 4; diff -= step; vpdiff += step; }
  step >>= 1;
  if (diff >= step) { code |= 2; diff -= step; vpdiff += step; }
  step >>= 1;
  if (diff >= step) { code |= 1; vpdiff += step; }
  adpcmPred += (code & 8) ? -vpdiff : vpdiff;
  adpcmPred = constrain(adpcmPred, -32768, 32767);
  adpcmIndex = constrain(adpcmIndex + INDEX_ADJ[code & 7], 0, 88);
  return code;
}

bool bleStartAudio() {
  xSemaphoreTake(inboxMutex, portMAX_DELAY);
  for (int i = 0; i < INBOX_N; ++i) { inbox[i] = ""; inboxNew[i] = false; }  // drop answers of an older turn
  xSemaphoreGive(inboxMutex);
  adpcmPred = 0; adpcmIndex = 0;
  static const char* START_JSON = "{\"rate\":16000,\"codec\":\"ima-adpcm\"}";
  return bleSend(BLE_START, (const uint8_t*)START_JSON, strlen(START_JSON));
}

// Encodes straight into each packet so no message-sized buffer is needed.
bool bleSendAudio(const int16_t* pcm, size_t n) {
  if (!bleReady()) return false;
  size_t bytes = n / 2;
  if (!bytes) return true;
  uint16_t conn = connHandle;
  xSemaphoreTake(txMutex, portMAX_DELAY);
  size_t maxPay = payloadSize(conn);
  uint8_t pkt[MTU];
  bool ok = true;
  while (ok && bytes) {
    size_t k = bytes < maxPay ? bytes : maxPay;
    for (size_t i = 0; i < k; ++i) {
      uint8_t lo = adpcmEncode(*pcm++);  // first sample in the low nibble
      uint8_t hi = adpcmEncode(*pcm++);
      pkt[2 + i] = lo | hi << 4;
    }
    bytes -= k;
    pkt[0] = BLE_AUDIO;
    pkt[1] = bytes == 0 ? 1 : 0;
    ok = sendPacket(conn, pkt, k + 2);
  }
  xSemaphoreGive(txMutex);
  return ok;
}

// ---------- receiving ----------
bool bleTake(uint8_t type, String& payload) {
  int i = inboxSlot(type);
  if (i < 0) return false;
  xSemaphoreTake(inboxMutex, portMAX_DELAY);
  bool fresh = inboxNew[i];
  if (fresh) { payload = inbox[i]; inbox[i] = ""; inboxNew[i] = false; }
  xSemaphoreGive(inboxMutex);
  return fresh;
}

bool bleRequest(uint8_t req, uint8_t answer, String& payload, uint32_t timeoutMs) {
  String stale;
  bleTake(answer, stale);
  if (!bleSend(req)) return false;
  for (uint32_t t0 = millis(); millis() - t0 < timeoutMs && bleReady();) {
    if (bleTake(answer, payload)) return true;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return false;
}

static void handleAuth(const uint8_t* p, size_t n) {
  uint8_t mac[32];
  bool ok = nonceValid && n == sizeof(mac) &&
            mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t*)RELAY_TOKEN,
                            strlen(RELAY_TOKEN), nonce, sizeof(nonce), mac) == 0;
  uint8_t diff = 0;
  for (size_t i = 0; ok && i < sizeof(mac); ++i) diff |= mac[i] ^ p[i];  // constant time
  nonceValid = false;
  if (ok && diff == 0) {
    authed = true;
    LOGF("ble: authenticated\n");
  } else {
    LOGF("ble: auth failed, disconnecting\n");
    server->disconnect(connHandle);
  }
}

static void deliver(uint8_t type, const std::string& msg) {
  if (type == BLE_AUTH) { if (!authed) handleAuth((const uint8_t*)msg.data(), msg.size()); return; }
  int i = inboxSlot(type);
  if (i < 0) return;
  xSemaphoreTake(inboxMutex, portMAX_DELAY);
  inbox[i] = String(msg.c_str());
  inboxNew[i] = true;
  xSemaphoreGive(inboxMutex);
}

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c, ble_gap_conn_desc* desc) override {
    const uint8_t* p = c->getData();
    size_t n = c->getLength();
    if (n < 2 || desc->conn_handle != connHandle) return;
    uint8_t type = p[0];
    bool last = p[1] & 1;
    if (!authed && type != BLE_AUTH) return;  // nothing but AUTH before authentication
    if (!rxBuf.empty() && type != rxType) rxBuf.clear();  // a message was cut off
    rxType = type;
    if (rxBuf.size() + n - 2 > RX_MAX) { rxBuf.clear(); return; }
    rxBuf.append((const char*)p + 2, n - 2);
    if (!last) return;
    std::string msg;
    msg.swap(rxBuf);
    deliver(type, msg);
  }
};

class TxCallbacks : public BLECharacteristicCallbacks {
  void onSubscribe(BLECharacteristic*, ble_gap_conn_desc* desc, uint16_t subValue) override {
    if (desc->conn_handle != connHandle) return;
    authed = false;
    subscribed = subValue & 1;
    if (!subscribed) return;
    esp_fill_random(nonce, sizeof(nonce));
    nonceValid = true;
    helloPending = true;  // sent from bleTask: the host task must not wait for buffers
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*, ble_gap_conn_desc* desc) override {
    subscribed = false; authed = false; helloPending = false; nonceValid = false;
    rxBuf.clear();
    connectedAt = millis();
    connHandle = desc->conn_handle;
    ble_gattc_exchange_mtu(desc->conn_handle, nullptr, nullptr);
    LOGF("ble: connected\n");
  }
  void onDisconnect(BLEServer*, ble_gap_conn_desc*) override {
    connHandle = BLE_HS_CONN_HANDLE_NONE;
    subscribed = false; authed = false; helloPending = false; nonceValid = false;
    rxBuf.clear();
    LOGF("ble: disconnected\n");  // advertising restarts (advertiseOnDisconnect)
  }
};

// Sends HELLO and drops centrals that never authenticate.
static void bleTask(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(20));
    uint16_t conn = connHandle;
    if (conn == BLE_HS_CONN_HANDLE_NONE) continue;
    if (helloPending) {
      helloPending = false;
      uint8_t n[sizeof(nonce)];
      memcpy(n, nonce, sizeof(n));
      bleSend(BLE_HELLO, n, sizeof(n));
    }
    if (!authed && millis() - connectedAt > AUTH_TIMEOUT_MS) {
      LOGF("ble: no auth, disconnecting\n");
      server->disconnect(conn);
      connectedAt = millis();  // do not repeat while the disconnect is in progress
    }
  }
}

void bleBegin() {
  txMutex = xSemaphoreCreateMutex();
  inboxMutex = xSemaphoreCreateMutex();
  BLEDevice::init(AVATAR_NAME "-StopWatch");  // the relay finds the watch by service UUID, not by name
  BLEDevice::setMTU(MTU);
  server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  server->advertiseOnDisconnect(true);
  BLEService* svc = server->createService(SERVICE_UUID);
  txChr = svc->createCharacteristic(TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  txChr->setCallbacks(new TxCallbacks());
  BLECharacteristic* rx = svc->createCharacteristic(RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCallbacks());
  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);  // the name does not fit next to the 128-bit UUID
  adv->setMinInterval(800);    // 500 ms (0.625 ms units): slow while idle to save power
  adv->setMaxInterval(1600);   // 1000 ms
  BLEDevice::startAdvertising();
  xTaskCreatePinnedToCore(bleTask, "ble", 3072, nullptr, 2, nullptr, 0);
  LOGF("ble: advertising\n");
}
