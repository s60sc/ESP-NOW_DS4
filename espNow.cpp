
/********************** ESP-NOW sender or receiver ************************/
/*

ESP-NOW is a connectionless WiFi communication protocol defined by Espressif with a longer range 
than standard WiFi, only needing the MAC Address of another ESP to connect to it.
It only works on ESP devices and can run concurrently with standard Wifi. 
Any WiFi mode can be used to open a channel - AP, STA, AP+STA, but ESP-NOW does not need the SSID

It is useful for sending small packets of information with low latency, eg to send inputs from 
a game controller to a drone.

Both sides of the connection must use the same WiFi channel and must know each others MAC address
The Sender automatically sets up a single peer to peer connection by broadcasting on each WiFi channel in turn,
if another device responds correctly then each now knows the others MAC Address and the connection is established.

A heartbeat is used to confirm that the connection is active. If a connection is dropped eg out of range,
it will automatically reconnect when in range.
If the Receiver is power cycled it may restart with a different channel if WiFi mode STA or STA+AP is used as the channel is
selected by the router. If Wifi mode AP is used then the channel number can be fixed.
Note that each device has a separate MAC address for AP and STA.

Optional security: HMAC-SHA256 challenge-response handshake + PMK/LMK link encryption
- USE_SECURE true  : authenticated discovery + encrypted link
- USE_SECURE false :  plaintext behaviour (no PSK, no mbedTLS needed)
- Both sender and receiver must be built with the same settings

If using USE_SECURE true:
 In Windows PowerShell administrator mode, run following command to generate own private 32 byte key 
 to clipboard and overwrite the #define ESPNOW_PSK_BYTES with the result 
 
$b = New-Object byte[] 32
$rng = [System.Security.Cryptography.RNGCryptoServiceProvider]::new()
$rng.GetBytes($b)
$rng.Dispose()
$h = $b | ForEach-Object { '0x{0:X2}' -f $_ }
"#define ESPNOW_PSK_BYTES { \`n  $($h[0..15] -join ', '), \`n  $($h[16..31] -join ', ') }" | Set-Clipboard

s60sc 2026

*/

#include "appGlobals.h"

#if defined(ESPNOW_SENDER) || defined(ESPNOW_RECEIVER)

#include <esp_now.h>
#include <esp_wifi.h>
#if USE_SECURE 
#include <esp_random.h>
#include "mbedtls/build_info.h"
#if MBEDTLS_VERSION_MAJOR >= 4
#include "psa/crypto.h"  // mbedTLS 4.x (Arduino-ESP32 core 4.x / IDF 6.x): PSA Crypto API
#define SEC_USE_PSA 1
#else
#include "mbedtls/md.h"  // mbedTLS 3.x (core 3.x / IDF 5.x): legacy API
#define SEC_USE_PSA 0
#endif
#endif

// configuration

#define CH_MIN 1
#define CH_MAX 14 // ESP may be limited to 11 by default
#define CH_PROBE   0xF2   // sender   -> broadcast : (secure: nonceS)
#define CH_REPLY   0xF3   // receiver -> sender    : (secure: nonceR, AP MAC, channel, HMAC)
#define CH_CONFIRM 0xF4   // secure only: sender   -> broadcast : HMAC bound to both nonces
#define CH_ACK     0xF5   // secure only: receiver -> broadcast : HMAC, "key installed"
#define PROBES_PER_CHANNEL 3
#define REPLY_WAIT_MS 200 // 60
#define HEARTBEAT_INTERVAL 250  // ms, sender: max silence before a heartbeat
#define HEARTBEAT_TIMEOUT  750  // ms, receiver: silence before link is lost
#define RX_QUEUE_LEN  32
#define PKT_LEN 6
#define MAC_LEN 6
#define KEY_LEN 16        // PMK / LMK (AES-128)

#if USE_SECURE 
// security configuration
#define NONCE_LEN      16
#define TAG_LEN        32     // HMAC-SHA256
#define HS_QUEUE_LEN   8
#define HS_MAX_LEN     64
#define PENDING_MAX    4      // receiver: simultaneous open handshakes
#define PENDING_TTL_MS 15000  // receiver: nonceR validity
#define MIN_PROBE_GAP_MS 100  // receiver: rate-limit replies to probes
#define CONFIRM_TRIES  10     // sender: CONFIRM retransmissions
#define CONFIRM_GAP_MS 50
#define REKEY_AFTER_MS 2000   // sender: link down this long -> renegotiate
#define REKEY_RETRY_MS 5000   // sender: pause between renegotiation attempts

#if !defined(ESPNOW_PSK_BYTES) && USE_SECURE
#error "Define ESPNOW_PSK_BYTES as 32 random bytes in your private config"
#endif
static const uint8_t PSK[] = ESPNOW_PSK_BYTES;
static_assert(sizeof(PSK) == 32, "ESPNOW_PSK_BYTES must be exactly 32 bytes");
static_assert(sizeof(AppDataPacket) == PKT_LEN, "AppDataPacket must be PKT_LEN bytes");

// handshake frames are all longer than AppDataPacket, so frames are told apart by length + type byte
typedef struct __attribute__((packed)) { uint8_t type; uint8_t nonceS[NONCE_LEN]; } ProbeMsg;
typedef struct __attribute__((packed)) { uint8_t type; uint8_t nonceR[NONCE_LEN]; uint8_t apMac[MAC_LEN]; uint8_t channel; uint8_t tag[TAG_LEN]; } ReplyMsg;
typedef struct __attribute__((packed)) { uint8_t type; uint8_t nonceR[NONCE_LEN]; uint8_t apMac[MAC_LEN]; uint8_t channel; uint8_t tag[TAG_LEN]; } ConfirmMsg;
typedef struct __attribute__((packed)) { uint8_t type; uint8_t tag[TAG_LEN]; } AckMsg;
typedef struct { uint8_t len; uint8_t data[HS_MAX_LEN]; } HsItem;
static_assert(sizeof(ProbeMsg) > sizeof(AppDataPacket) && sizeof(AckMsg) > sizeof(AppDataPacket), "handshake frames must differ in length from data frames");
static_assert(sizeof(ReplyMsg) <= HS_MAX_LEN && sizeof(ConfirmMsg) <= HS_MAX_LEN, "HS_MAX_LEN too small");
#endif // USE_SECURE 

static const uint8_t BROADCAST_ADDR[PKT_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static uint8_t otherMAC[PKT_LEN] = {0};
static uint8_t ch = 0;
static bool firstOK = true;
static bool firstFail = true;
volatile bool gotReply = false;
TaskHandle_t espNowHandle = NULL;

static QueueHandle_t rxQueue = NULL;   // application data (AppDataPacket)
static volatile uint32_t lastReceive = 0;
static bool linkUp = false;
#if USE_SECURE 
static QueueHandle_t hsQueue = NULL;   // handshake frames (HsItem)
static volatile bool secureUp = false; // true once a verified, encrypted peer is installed
#define PEER_READY() (secureUp)
#else
#define PEER_READY() (otherMAC[0] != 0)
#endif

/************************ crypto helpers ************************/
#if USE_SECURE

// HMAC-SHA256 keyed with the PSK. out must hold 32 bytes.
static bool secHmac(const uint8_t* msg, size_t len, uint8_t* out) {
#if SEC_USE_PSA
  static bool psaReady = false;
  if (!psaReady) {
    if (psa_crypto_init() != PSA_SUCCESS) return false;
    psaReady = true;
  }
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
  psa_set_key_bits(&attr, sizeof(PSK) * 8);
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
  psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
  psa_key_id_t key = 0;
  if (psa_import_key(&attr, PSK, sizeof(PSK), &key) != PSA_SUCCESS) return false;   // volatile key
  size_t olen = 0;
  psa_status_t st = psa_mac_compute(key, PSA_ALG_HMAC(PSA_ALG_SHA_256), msg, len, out, 32, &olen);
  psa_destroy_key(key);
  return st == PSA_SUCCESS && olen == 32;
#else
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return mbedtls_md_hmac(info, PSK, sizeof(PSK), msg, len, out) == 0;
#endif
}

// Domain-separated HMAC over (label, both nonces, a MAC, channel).
static bool secTag(const char* label, const uint8_t* nS, const uint8_t* nR,
                   const uint8_t* mac, uint8_t channel, uint8_t* out) {
  uint8_t buf[3 + NONCE_LEN + NONCE_LEN + MAC_LEN + 1];
  size_t o = 0;
  memcpy(buf + o, label, 3);        o += 3;
  memcpy(buf + o, nS, NONCE_LEN);   o += NONCE_LEN;
  memcpy(buf + o, nR, NONCE_LEN);   o += NONCE_LEN;
  memcpy(buf + o, mac, MAC_LEN);    o += MAC_LEN;
  buf[o++] = channel;
  return secHmac(buf, o, out);
}

// Constant-time compare.
static bool secEq(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
  return d == 0;
}

// Static PMK derived from the PSK (never transmitted).
static bool secDerivePmk(uint8_t* out) {
  static const uint8_t label[] = "ESPNOW-PMK";
  uint8_t full[32];
  if (!secHmac(label, sizeof(label) - 1, full)) return false;
  memcpy(out, full, KEY_LEN);
  memset(full, 0, sizeof(full));
  return true;
}

// Per-session LMK bound to both fresh nonces, both AP MACs and the channel (never transmitted).
static bool secDeriveLmk(const uint8_t* nS, const uint8_t* nR, const uint8_t* txApMac,
                         const uint8_t* rxApMac, uint8_t channel, uint8_t* out) {
  static const uint8_t label[] = "ESPNOW-LMK";
  uint8_t buf[sizeof(label) - 1 + NONCE_LEN + NONCE_LEN + MAC_LEN + MAC_LEN + 1];
  size_t o = 0;
  memcpy(buf + o, label, sizeof(label) - 1); o += sizeof(label) - 1;
  memcpy(buf + o, nS, NONCE_LEN);            o += NONCE_LEN;
  memcpy(buf + o, nR, NONCE_LEN);            o += NONCE_LEN;
  memcpy(buf + o, txApMac, MAC_LEN);         o += MAC_LEN;
  memcpy(buf + o, rxApMac, MAC_LEN);         o += MAC_LEN;
  buf[o++] = channel;
  uint8_t full[32];
  if (!secHmac(buf, o, full)) return false;
  memcpy(out, full, KEY_LEN);
  memset(full, 0, sizeof(full));
  return true;
}

#endif // USE_SECURE 

/************************ general ************************/

void espNowCommand(const AppDataPacket &p) {
  // Normal commands (not heartbeat) 
  esp_err_t result = esp_now_send(otherMAC, (uint8_t*)&p, PKT_LEN);
}

static void macToString(const uint8_t* mac, char* out) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool parseMAC(const char* macstr = "") {
  // parse MAC string into array
  unsigned int b[6];
  if (sscanf(macstr, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return false;
  else for (int i = 0; i < 6; i++) otherMAC[i] = static_cast<uint8_t>(b[i]);
  return true;
}

static void OnDataSent(const wifi_tx_info_t* info, esp_now_send_status_t status) {
  char macstr[18];
  // broadcast frames (discovery / handshake) are not interesting
  if (memcmp(info->des_addr, BROADCAST_ADDR, MAC_LEN) == 0) return;
  // suppress multiple messages when state changes
  if (status == ESP_NOW_SEND_SUCCESS ) {
    if (firstOK) {
      macToString(info->des_addr, macstr);
      LOG_INF("Sent to %s", macstr);
      firstOK = false;
      firstFail = true;
    }
  } else {
    if (firstFail) {
      macToString(info->des_addr, macstr);
      LOG_WRN("Failed to send to %s", macstr);
      firstOK = true;
      firstFail = false;
    }
  }
}

static bool setChannel(uint8_t ch) {
  return esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) == ESP_OK;
}

// lmk == nullptr -> plain peer; otherwise CCMP-encrypted peer using the LMK (secure mode only)
static bool addPeer(const uint8_t* mac, const uint8_t* lmk = nullptr) {
  if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, MAC_LEN);
  peerInfo.channel = 0; // 0 = current channel
  peerInfo.ifidx = WIFI_IF_AP;
  peerInfo.encrypt = (lmk != nullptr);
  if (lmk) memcpy(peerInfo.lmk, lmk, KEY_LEN);
  esp_err_t err = esp_now_add_peer(&peerInfo);
  memset(&peerInfo, 0, sizeof(peerInfo));   // wipe LMK copy
  if (err != ESP_OK) {
    LOG_WRN("Failed to add ESP-NOW peer: %s", esp_err_to_name(err) );
    return false;
  }
  char macstr[18];
  macToString(mac, macstr);
  if (lmk) LOG_INF("ESP-NOW secure peer %s", macstr);
#if !USE_SECURE 
  else LOG_INF("ESP-NOW Sender AP MAC: %s using channel: %d", macstr, ch);
#endif
  return true;
}

static void onReceive(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
#if USE_SECURE 
  // Runs in the WiFi task: validate, timestamp and queue only.
  if (len == (int)sizeof(AppDataPacket)) {
    // application data: accepted only from the authenticated peer
    if (!secureUp || memcmp(info->src_addr, otherMAC, MAC_LEN) != 0) return;
    lastReceive = millis();
    xQueueSend(rxQueue, data, 0);   // drops the packet if the queue is full
    return;
  }
  if (len >= 1 && len <= HS_MAX_LEN && data[0] >= CH_PROBE && data[0] <= CH_ACK) {
    HsItem it;
    it.len = (uint8_t)len;
    memcpy(it.data, data, len);
    xQueueSend(hsQueue, &it, 0);
  }
#else
  // validate, timestamp and queue received data
  if (len != sizeof(AppDataPacket)) return;
  if (!otherMAC[0]) memcpy(otherMAC, info->src_addr, PKT_LEN);
  lastReceive = millis();
  xQueueSend(rxQueue, data, 0);   // drops the packet if the queue is full
#endif
}

/************************ sender ************************/
#ifdef ESPNOW_SENDER

static void processHeartbeat() {
  // Heartbeat deliberately not sent through sendCommand().

  if (PEER_READY()) {
    uint32_t now = millis();
    if (now - lastReceive < HEARTBEAT_INTERVAL) return;
    AppDataPacket p;
    p.command = HB_PING;
    esp_now_send(otherMAC, (uint8_t*)&p, PKT_LEN);

    // Treat heartbeat as the start of the next heartbeat period.
    lastReceive = now;
  }
}

#if USE_SECURE 
// ---- authenticated discovery ----

static uint8_t hsNonceS[NONCE_LEN];
static uint8_t lockedCh = CH_MIN;   // last channel with a working link (rekey starts here)

// Probe one channel. True when a REPLY verifies against OUR nonce.
static bool probeChannel(uint8_t c, uint8_t* rxMac, uint8_t* nonceR) {
  ProbeMsg pm;
  pm.type = CH_PROBE;
  memcpy(pm.nonceS, hsNonceS, NONCE_LEN);
  for (uint8_t i = 0; i < PROBES_PER_CHANNEL; i++) {
    esp_now_send(BROADCAST_ADDR, (uint8_t*)&pm, sizeof(pm));
    uint32_t start = millis();
    while (millis() - start < REPLY_WAIT_MS) {
      HsItem it;
      if (xQueueReceive(hsQueue, &it, pdMS_TO_TICKS(5)) != pdTRUE) continue;
      if (it.len != sizeof(ReplyMsg) || it.data[0] != CH_REPLY) continue;
      ReplyMsg r;
      memcpy(&r, it.data, sizeof(r));
      if (r.channel != c) continue;
      uint8_t expect[TAG_LEN];
      if (!secTag("RPL", hsNonceS, r.nonceR, r.apMac, r.channel, expect)) continue;
      if (!secEq(expect, r.tag, TAG_LEN)) continue;      // spoofed / replayed
      memcpy(rxMac, r.apMac, MAC_LEN);
      memcpy(nonceR, r.nonceR, NONCE_LEN);
      return true;
    }
  }
  return false;
}

// Prove knowledge of the PSK and wait for the receiver's authenticated ACK.
static bool confirmReceiver(uint8_t c, const uint8_t* nonceR, const uint8_t* myAp) {
  ConfirmMsg cm;
  cm.type = CH_CONFIRM;
  memcpy(cm.nonceR, nonceR, NONCE_LEN);
  memcpy(cm.apMac, myAp, MAC_LEN);
  cm.channel = c;
  secTag("CNF", hsNonceS, nonceR, myAp, c, cm.tag);
  uint8_t expectAck[TAG_LEN];
  secTag("ACK", hsNonceS, nonceR, myAp, c, expectAck);

  for (uint8_t i = 0; i < CONFIRM_TRIES; i++) {
    esp_now_send(BROADCAST_ADDR, (uint8_t*)&cm, sizeof(cm));
    uint32_t start = millis();
    while (millis() - start < CONFIRM_GAP_MS) {
      HsItem it;
      if (xQueueReceive(hsQueue, &it, pdMS_TO_TICKS(5)) != pdTRUE) continue;
      if (it.len != sizeof(AckMsg) || it.data[0] != CH_ACK) continue;
      AckMsg a;
      memcpy(&a, it.data, sizeof(a));
      if (secEq(expectAck, a.tag, TAG_LEN)) return true;
    }
  }
  return false;
}

// Scan channels (starting at startCh), authenticate a receiver, install an encrypted peer.
static bool scanForReceiver(uint8_t startCh) {
  uint8_t myAp[MAC_LEN];
  esp_wifi_get_mac(WIFI_IF_AP, myAp);
  esp_fill_random(hsNonceS, NONCE_LEN);          // fresh nonce per scan session
  xQueueReset(hsQueue);
  addPeer(BROADCAST_ADDR);

  const uint8_t span = CH_MAX - CH_MIN + 1;
  for (uint8_t k = 0; k < span; k++) {
    ch = CH_MIN + ((startCh - CH_MIN + k) % span);
    if (!setChannel(ch)) continue;
    LOG_INF("Try channel %u", ch);

    uint8_t rxMac[MAC_LEN], nonceR[NONCE_LEN];
    if (!probeChannel(ch, rxMac, nonceR)) continue;
    char macstr[18];
    macToString(rxMac, macstr);
    LOG_INF("Verified reply on channel %u from %s", ch, macstr);

    if (!confirmReceiver(ch, nonceR, myAp)) {
      LOG_WRN("Receiver did not acknowledge, continuing scan");
      continue;
    }

    uint8_t lmk[KEY_LEN];
    bool ok = secDeriveLmk(hsNonceS, nonceR, myAp, rxMac, ch, lmk);
    esp_now_del_peer(BROADCAST_ADDR);
    if (ok && otherMAC[0]) esp_now_del_peer(otherMAC);        // drop previous session peer
    ok = ok && addPeer(rxMac, lmk);
    memset(lmk, 0, sizeof(lmk));
    if (!ok) return false;

    memcpy(otherMAC, rxMac, MAC_LEN);
    lockedCh = ch;
    secureUp = true;
    return true;
  }
  esp_now_del_peer(BROADCAST_ADDR);
  setChannel(lockedCh);
  return false;
}

// Link lost for a while (e.g. receiver rebooted and lost its keys) -> renegotiate.
static void processRekey() {
  static uint32_t downSince = 0, lastTry = 0;
  if (linkUp) { downSince = 0; return; }
  uint32_t now = millis();
  if (!downSince) { downSince = now; return; }
  if (now - downSince < REKEY_AFTER_MS || now - lastTry < REKEY_RETRY_MS) return;
  lastTry = now;
  LOG_INF("Link down, renegotiating keys");
  secureUp = false;
  if (scanForReceiver(lockedCh)) LOG_INF("Secure link re-established");
}

#else
// ---- plaintext discovery ----

static bool scanForReceiver(uint8_t startCh) {
  (void)startCh;
  AppDataPacket p;
  p.command = CH_PROBE;
  for (ch = CH_MIN; ch <= CH_MAX; ch++) {
    if (!setChannel(ch)) continue;
    LOG_INF("Try channel %u", ch);
    for (uint8_t i = 0; i < PROBES_PER_CHANNEL; i++) {
      esp_now_send(BROADCAST_ADDR, (uint8_t*)&p, PKT_LEN);
      uint32_t start = millis();
      while (millis() - start < REPLY_WAIT_MS) {
        if (otherMAC[0]) {
          char macstr[18];
          macToString(otherMAC, macstr);
          LOG_INF("Reply on channel %u from %s", ch, macstr);
          return true;
        }
        delay(1);
      }
    }
  }
  return false;
}
#endif // USE_SECURE 

#endif // ESPNOW_SENDER

/************************ receiver (secure handshake) ************************/
#if defined(ESPNOW_RECEIVER) && USE_SECURE 

typedef struct {
  uint8_t  nonceS[NONCE_LEN];
  uint8_t  nonceR[NONCE_LEN];
  uint32_t t;
  bool     valid;
  bool     done;     // key installed; further valid CONFIRMs only re-send the ACK
} Pending;
static Pending pend[PENDING_MAX];
static uint8_t pendIdx = 0;
static uint32_t lastProbeMs = 0;

static void processHandshake() {
  HsItem it;
  while (xQueueReceive(hsQueue, &it, 0) == pdTRUE) {
    uint32_t now = millis();
    uint8_t myAp[MAC_LEN];
    esp_wifi_get_mac(WIFI_IF_AP, myAp);
    uint8_t pri;
    wifi_second_chan_t sec;
    esp_wifi_get_channel(&pri, &sec);

    // ---- PROBE -> authenticated REPLY (no state change, no peer added) ----
    if (it.data[0] == CH_PROBE && it.len == sizeof(ProbeMsg)) {
      if (now - lastProbeMs < MIN_PROBE_GAP_MS) continue;
      lastProbeMs = now;

      Pending &p = pend[pendIdx++ % PENDING_MAX];
      memcpy(p.nonceS, it.data + 1, NONCE_LEN);
      esp_fill_random(p.nonceR, NONCE_LEN);
      p.t = now; p.valid = true; p.done = false;

      ReplyMsg r;
      r.type = CH_REPLY;
      memcpy(r.nonceR, p.nonceR, NONCE_LEN);
      memcpy(r.apMac, myAp, MAC_LEN);
      r.channel = pri;
      if (secTag("RPL", p.nonceS, p.nonceR, myAp, pri, r.tag))
        esp_now_send(BROADCAST_ADDR, (uint8_t*)&r, sizeof(r));
    }
    // ---- CONFIRM -> verify, install encrypted peer, ACK ----
    else if (it.data[0] == CH_CONFIRM && it.len == sizeof(ConfirmMsg)) {
      ConfirmMsg m;
      memcpy(&m, it.data, sizeof(m));
      if (m.channel != pri) continue;

      for (int i = 0; i < PENDING_MAX; i++) {
        Pending &p = pend[i];
        if (!p.valid || now - p.t > PENDING_TTL_MS) continue;
        if (!secEq(p.nonceR, m.nonceR, NONCE_LEN)) continue;

        uint8_t expect[TAG_LEN];
        if (!secTag("CNF", p.nonceS, p.nonceR, m.apMac, m.channel, expect)) break;
        if (!secEq(expect, m.tag, TAG_LEN)) break;           // forged -> ignore

        if (!p.done) {
          uint8_t lmk[KEY_LEN];
          bool ok = secDeriveLmk(p.nonceS, p.nonceR, m.apMac, myAp, m.channel, lmk);
          if (ok && otherMAC[0]) esp_now_del_peer(otherMAC); // replace previous session
          ok = ok && addPeer(m.apMac, lmk);
          memset(lmk, 0, sizeof(lmk));
          if (!ok) break;
          memcpy(otherMAC, m.apMac, MAC_LEN);
          secureUp = true;
          p.done = true;
          char macstr[18];
          macToString(m.apMac, macstr);
          LOG_INF("Authenticated sender %s on channel %u", macstr, pri);
        }
        // ACK (also re-sent for duplicate CONFIRMs, in case the first ACK was lost)
        AckMsg a;
        a.type = CH_ACK;
        if (secTag("ACK", p.nonceS, p.nonceR, m.apMac, m.channel, a.tag))
          esp_now_send(BROADCAST_ADDR, (uint8_t*)&a, sizeof(a));
        break;
      }
    }
  }
}

#endif // ESPNOW_RECEIVER && USE_SECURE 

/************************ common processing ************************/

static void processReceiver() {
  AppDataPacket p;
  while (xQueueReceive(rxQueue, &p, 0) == pdTRUE) {
    if (!linkUp) {
      linkUp = true;
      espNowLinkRestored();
    }
    switch(p.command) {
      case HB_PING:
        p.command = HB_PONG;
        esp_now_send(otherMAC, (uint8_t*)&p, PKT_LEN);
      break;
      case HB_PONG:
      break;
#if !USE_SECURE 
      case CH_PROBE:
        // receiver obtains sender MAC and replies
        addPeer(otherMAC);
        p.command = CH_REPLY;
        esp_now_send(otherMAC, (uint8_t*)&p, PKT_LEN);
      break;
      case CH_REPLY:
        // sender obtains Receiver MAC
        esp_now_del_peer(BROADCAST_ADDR);
        addPeer(otherMAC);
      break;
#endif
      // secure mode: handshake frames are handled via hsQueue, never reach this switch
      default:
        appControllerCommand(p);
      break;
    }
  }

  // Signed difference, so a timestamp set by the callback just now cannot underflow
  int32_t age = (int32_t)(millis() - lastReceive);
  if (linkUp && age > HEARTBEAT_TIMEOUT) {
    linkUp = false;
    espNowLinkLost();
  }
}

static void espNowTask (void* parameter) {
  // runs at periodic interval
  TickType_t lastWake = xTaskGetTickCount();
  while (true) {
      processReceiver();
#if defined(ESPNOW_RECEIVER) && USE_SECURE 
      processHandshake();
#endif
#ifdef ESPNOW_SENDER
      processHeartbeat();
#if USE_SECURE 
      processRekey();      // may block for a few seconds while the link is down
#endif
#endif
    const TickType_t period = pdMS_TO_TICKS(20); // 50Hz
    vTaskDelayUntil(&lastWake, period);
  }
  vTaskDelete(NULL);
}

bool espNowPrep() {
  if (esp_now_init() != ESP_OK) {
    LOG_WRN("ESP-NOW init failed");
    return false;
  }

#if USE_SECURE 
  // PMK must be set after init and before any encrypted peer is added
  uint8_t pmk[KEY_LEN];
  bool pmkOk = secDerivePmk(pmk) && esp_now_set_pmk(pmk) == ESP_OK;
  memset(pmk, 0, sizeof(pmk));
  if (!pmkOk) {
    LOG_WRN("Failed to set ESP-NOW PMK");
    return false;
  }
  LOG_INF("ESP-NOW security: ON");
#else
  LOG_INF("ESP-NOW security: OFF");
#endif

  esp_now_register_send_cb(OnDataSent);
  if (!rxQueue) rxQueue = xQueueCreate(RX_QUEUE_LEN, sizeof(AppDataPacket));
#if USE_SECURE 
  if (!hsQueue) hsQueue = xQueueCreate(HS_QUEUE_LEN, sizeof(HsItem));
  if (!rxQueue || !hsQueue) {
#else
  if (!rxQueue) {
#endif
    LOG_WRN("Failed to create ESP-NOW queues");
    return false;
  }
  if (esp_now_register_recv_cb(onReceive) != ESP_OK) {
    LOG_WRN("Failed to register receiver callback");
    return false;
  }
  return true;
}

bool espNowConnect() {
#ifdef ESPNOW_SENDER
  // find a compatible receiver
 #if !USE_SECURE 
  addPeer(BROADCAST_ADDR);   // secure mode adds/removes it inside scanForReceiver()
 #endif
  while (!scanForReceiver(CH_MIN)) delay(100); // wait for receiver
#else
  // receiver
 #if USE_SECURE 
  addPeer(BROADCAST_ADDR);   // receiver answers probes / confirms by broadcast
 #endif
  LOG_INF("ESP-NOW Receiver MAC: %s listening on channel: %ld", WiFi.AP.macAddress().c_str(), WiFi.channel());
  // receiever link status changed by espNowLinkRestored() and espNowLinkLost()
#endif 
  // common
  if (!espNowHandle) xTaskCreate(&espNowTask, "espNowTask", ESPNOW_STACK_SIZE, NULL, ESPNOW_PRI, &espNowHandle);
  if (!espNowHandle) {
    LOG_WRN("Failed to create espNowTask");
    return false;
  }
  return true;
}

#endif // ESPNOW_SENDER || ESPNOW_RECEIVER
