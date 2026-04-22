#include "MeshcoreCliGateway.h"

#ifdef ESP32

#include <Arduino.h>
#include <WiFi.h>
#include <cstdio>
#include <cstring>

#include <Mesh.h>
#include <Identity.h>
#include <Packet.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/ClientACL.h>     // OUT_PATH_UNKNOWN
#include <helpers/TxtDataHelpers.h>  // TXT_TYPE_CLI_DATA
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>

#include <LotatoConfig.h>
#include <LotatoIngestor.h>
#include <lofi/Lofi.h>
#include <lofs/LoFS.h>
#include <lolog/LoLog.h>
#include <lomessage/Buffer.h>
#include <lomessage/Split.h>
#include <losettings/ConfigHub.h>

namespace lotato {
namespace meshcore {

namespace {

/** `lomessage::Buffer` capacity for one CLI dispatch (must comfortably hold wifi scan dump). */
constexpr size_t kDispatchBufferBytes = 1280;

/**
 * Inter-chunk delay between reply FIFO emissions. Must be ≥ the LoRa airtime for one ~160-byte
 * TXT_MSG at the configured radio params so we don't outrun the radio TX queue. Upstream used
 * 600 ms for a single reply; for chunked delivery we can cruise closer to airtime since the client
 * is the only RX target and the radio serializes TX internally anyway.
 */
constexpr unsigned long kInterChunkDelayMs = 200;

struct CliCtx {
  uint32_t sender_ts;
};

/** Opaque routing ctx copied into each `lomessage::Queue` job; consumed by the gateway's Sink. */
struct CliReplyRoute {
  uint32_t sender_ts;
  uint8_t  pub_key[32];
  uint8_t  secret[32];
  uint8_t  out_path[MAX_PATH_SIZE];
  uint8_t  out_path_len;
  uint8_t  path_hash_size;
};

CliGateway* g_self = nullptr;

void runWifiScanCli(lomessage::Buffer& out) {
  lofi::Lofi& lf = lofi::Lofi::instance();
  lf.serviceWifiScan();
  if (lf.scanSnapshotCount() > 0 && !g_self->cliBusy()) {
    lf.formatScanBody(out);
    return;
  }
  if (g_self->cliBusy()) {
    out.append("WiFi scan in progress...");
    return;
  }
  lf.requestWifiScan();
  out.append("Scanning for WiFi devices...");
}

/* ── lotato root handlers ─────────────────────────────────────────── */

void h_status(locommand::Context& ctx) {
  LotatoConfig& cfg = LotatoConfig::instance();
  wl_status_t wl = WiFi.status();
  const char* wl_str = (wl == WL_CONNECTED) ? "connected" : "not connected";
  int code = g_self->ingestor().lastHttpCode();
  char code_str[12];
  if (code == 0) strcpy(code_str, "none");
  else snprintf(code_str, sizeof(code_str), "%d", code);
  const char* token_str = cfg.apiToken()[0] ? "set" : "(none)";
  const char* url_str = cfg.ingestOrigin()[0] ? cfg.ingestOrigin() : "(none)";
  const char* dbg_str = ::lolog::LoLog::isVerbose() ? "on" : "off";
  const int hist_count = g_self->ingestHistory().count();
  const int hist_cap   = g_self->ingestHistory().capacity();
  if (wl == WL_CONNECTED) {
    ctx.out.appendf("WiFi: %s\nSSID: %s\nIP: %s\nHistory: %d/%d\nPaused: %s\nLast API Response: %s\nURL: %s\nToken: %s\nDebug: %s",
                    wl_str, WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                    hist_count, hist_cap,
                    g_self->ingestor().isPaused() ? "yes" : "no", code_str, url_str, token_str, dbg_str);
  } else {
    ctx.out.appendf("WiFi: %s\nSaved: %s\nHistory: %d/%d\nPaused: %s\nURL: %s\nToken: %s\nDebug: %s",
                    wl_str, cfg.ssid()[0] ? cfg.ssid() : "(none)",
                    hist_count, hist_cap,
                    g_self->ingestor().isPaused() ? "yes" : "no", url_str, token_str, dbg_str);
  }
}

void h_pause(locommand::Context& ctx) {
  g_self->ingestor().setPaused(true);
  ctx.out.append("OK - ingest paused");
}

void h_resume(locommand::Context& ctx) {
  g_self->ingestor().setPaused(false);
  ctx.out.append("OK - ingest resumed");
}

void h_ingest(locommand::Context& ctx) {
  size_t limit = 0;
  if (ctx.argc >= 1) {
    int n = atoi(ctx.argv[0]);
    if (n > 0) limit = (size_t)n;
  }
  std::vector<LotatoIngestHistory::Row> rows;
  g_self->ingestHistory().snapshot(limit, rows);
  ctx.out.appendf("Ingest history: %d/%d\n", g_self->ingestHistory().count(),
                  g_self->ingestHistory().capacity());
  const uint32_t now_ms = millis();
  static const char* const hexd = "0123456789abcdef";
  for (const auto& r : rows) {
    char nid[10];
    nid[0] = '!';
    for (int i = 0; i < 4; i++) {
      nid[1 + i * 2]     = hexd[(r.rec.pub_key[i] >> 4) & 0x0f];
      nid[1 + i * 2 + 1] = hexd[r.rec.pub_key[i] & 0x0f];
    }
    nid[9] = '\0';
    uint32_t age_s = (now_ms - r.last_posted_ms) / 1000u;
    uint32_t m = age_s / 60u;
    uint32_t s = age_s % 60u;
    if (r.rec.gps_lat != 0 || r.rec.gps_lon != 0) {
      ctx.out.appendf("%s \"%.32s\" type=%u posted=%lum%lus ago gps=%.4f,%.4f\n", nid, r.rec.name,
                      (unsigned)r.rec.type, (unsigned long)m, (unsigned long)s,
                      (double)r.rec.gps_lat / 1000000.0, (double)r.rec.gps_lon / 1000000.0);
    } else {
      ctx.out.appendf("%s \"%.32s\" type=%u posted=%lum%lus ago\n", nid, r.rec.name,
                      (unsigned)r.rec.type, (unsigned long)m, (unsigned long)s);
    }
  }
}

/* ── wifi root handlers ───────────────────────────────────────────── */

void h_wifi_status(locommand::Context& ctx) {
  LotatoConfig& cfg = LotatoConfig::instance();
  wl_status_t wl = WiFi.status();
  if (wl == WL_CONNECTED) {
    ctx.out.appendf("WiFi: connected\nSSID: %s\nIP: %s", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
  } else {
    ctx.out.appendf("WiFi: not connected\nSaved: %s\nUse: wifi scan",
                    cfg.ssid()[0] ? cfg.ssid() : "(none)");
  }
}

void h_wifi_scan(locommand::Context& ctx) { runWifiScanCli(ctx.out); }

void h_wifi_connect(locommand::Context& ctx) {
  auto* lc = static_cast<CliCtx*>(ctx.app_ctx);
  LotatoConfig& cfg = LotatoConfig::instance();
  if (ctx.argc < 1) {
    ctx.printHelp();
    return;
  }
  const char* tok1 = ctx.argv[0];
  const char* tok2 = (ctx.argc >= 2) ? ctx.argv[1] : "";

  char ssid_to_use[33] = {};
  bool is_index = true;
  for (const char* q = tok1; *q; q++) {
    if (*q < '0' || *q > '9') {
      is_index = false;
      break;
    }
  }
  lofi::Lofi& lf = lofi::Lofi::instance();
  if (is_index && tok1[0] != '\0') {
    int idx = atoi(tok1) - 1;
    int32_t rssi;
    if (idx < 0 || !lf.scanSnapshotEntry(idx, ssid_to_use, &rssi)) {
      ctx.out.appendf("Err - index out of range (1..%d)\nRun: wifi scan first", lf.scanSnapshotCount());
      return;
    }
  } else {
    strncpy(ssid_to_use, tok1, sizeof(ssid_to_use) - 1);
  }

  char pwd_to_use[65] = {};
  if (tok2[0] != '\0') {
    strncpy(pwd_to_use, tok2, sizeof(pwd_to_use) - 1);
    pwd_to_use[sizeof(pwd_to_use) - 1] = '\0';
  } else {
    cfg.getKnownWifiPassword(ssid_to_use, pwd_to_use, sizeof(pwd_to_use));
  }

  cfg.setWifi(ssid_to_use, pwd_to_use);
  g_self->ingestor().restartAfterConfigChange();
  lf.beginConnect(ssid_to_use, pwd_to_use);
  ::lolog::LoLog::debug("lotato", "lotato CLI: wifi connecting ssid=%s modem_sleep=off", ssid_to_use);
  ctx.out.appendf("Connecting to %s...", ssid_to_use);
}

void h_wifi_forget(locommand::Context& ctx) {
  LotatoConfig& cfg = LotatoConfig::instance();
  if (ctx.argc < 1) {
    ctx.printHelp();
    return;
  }
  if (!cfg.forgetKnownWifi(ctx.argv[0])) {
    ctx.out.append("Err - SSID not in known list\n");
    return;
  }
  ctx.out.append("OK\n");
}

const locommand::ArgSpec k_wifi_connect_args[] = {
    {"n_or_ssid", "string", nullptr, true, "Scan index (1-based) or SSID"},
    {"password", "secret", nullptr, false, "PSK if not already saved"},
};

const locommand::ArgSpec k_wifi_forget_args[] = {
    {"ssid", "string", nullptr, true, "Network SSID to remove from known list"},
};

const locommand::ArgSpec k_ingest_args[] = {
    {"n", "uint", nullptr, false, "Max entries to show (default: all)"},
};

}  // namespace

/* ── CliGateway ─────────────────────────────────────────────────────── */

CliGateway& CliGateway::instance() {
  static CliGateway s_inst;
  return s_inst;
}

CliGateway::CliGateway() = default;

void CliGateway::registerLotatoEngine() {
  _eng_lotato.add("status", &h_status, nullptr, nullptr, "show lotato/ingest status");
  _eng_lotato.add("pause", &h_pause, nullptr, nullptr, "pause ingest (shortcut for config)");
  _eng_lotato.add("resume", &h_resume, nullptr, nullptr, "resume ingest (shortcut for config)");
  _eng_lotato.addWithArgs("ingest", &h_ingest, k_ingest_args, 1, nullptr,
                           "recent ingest POSTs (newest first)");
  _eng_lotato.setRootBrief("ingest status / history");
}

void CliGateway::registerWifiEngine() {
  _eng_wifi.add("status", &h_wifi_status, nullptr, nullptr, "STA / saved SSID snapshot");
  _eng_wifi.add("scan", &h_wifi_scan, nullptr, nullptr, "scan for APs (async reply)");
  _eng_wifi.addWithArgs("connect", &h_wifi_connect, k_wifi_connect_args, 2, nullptr,
                        "connect by index or SSID");
  _eng_wifi.addWithArgs("forget", &h_wifi_forget, k_wifi_forget_args, 1, nullptr,
                        "remove SSID from known list");
  _eng_wifi.setRootBrief("WiFi STA scan/connect");
}

void CliGateway::begin(lofs::FSys* internal_fs, const uint8_t self_pub_key[32], mesh::Mesh* mesh) {
  g_self = this;
  _mesh = mesh;
  if (self_pub_key) memcpy(_self_pub_key, self_pub_key, sizeof(_self_pub_key));

  // Stage 1 — platform bringup. VFS logs [E] for every fopen of a missing file; LoDB/LoSettings
  // treat "missing" as normal (get returns NOT_FOUND), so silence the noise — real errors still log.
  esp_log_level_set("vfs_api", ESP_LOG_NONE);
  LoFS::bindInternalFs(internal_fs);
  LoFS::mountDefaults();
  ::lolog::LoLog::registerConfigSchema();
  ::lolog::LoLog::loadFromSettings();

  // Stage 2 — Lotato stack.
  LotatoConfig::instance().load();
  lofi::Lofi::instance().begin();
  _history.begin();

  lofi::Lofi::instance().setScanCompleteCallback(&CliGateway::onWifiScanComplete, nullptr);
  lofi::Lofi::instance().setConnectCompleteCallback(&CliGateway::onWifiConnectComplete, nullptr);

  losettings::ConfigHub::instance().bindConfigCli(_eng_config);
  _eng_config.setRootBrief("LoSettings keys (ls/get/set/unset)");

  registerLotatoEngine();
  registerWifiEngine();

  _router.clear();
  _router.add(&_eng_lotato);
  _router.add(&_eng_wifi);
  _router.add(&_eng_config);

  lotato_register_sta_dns_override();
  if (lofi::Lofi::instance().knownWifiCount() >= 2) {
    // Multiple known SSIDs: own reconnect timing to avoid fighting our failover.
    WiFi.setAutoReconnect(false);
  }

  UBaseType_t words = uxTaskGetStackHighWaterMark(nullptr);
  ::lolog::LoLog::info("lotato.stack", "begin: free_bytes=%u", (unsigned)(words * sizeof(StackType_t)));
}

void CliGateway::tickServices() {
  lofi::Lofi::instance().serviceWifiScan();
  _ingestor.serviceTick(_self_pub_key);
  _reply_queue.service(millis(), *this);
}

void CliGateway::onAdvertRecvInternal(const uint8_t pub_key[32], const uint8_t* app_data,
                                      size_t app_data_len, uint8_t path_len, uint32_t timestamp) {
  AdvertDataParser parser(app_data, app_data_len);
  if (!parser.isValid()) return;
  int32_t lat = parser.hasLatLon() ? parser.getIntLat() : 0;
  int32_t lon = parser.hasLatLon() ? parser.getIntLon() : 0;
  const char* name = parser.hasName() ? parser.getName() : "";
  uint8_t atype = parser.getType();
  char id_hex[9];
  static const char* const hexd = "0123456789abcdef";
  for (int i = 0; i < 4; i++) {
    id_hex[i * 2]     = hexd[pub_key[i] >> 4];
    id_hex[i * 2 + 1] = hexd[pub_key[i] & 0xf];
  }
  id_hex[8] = '\0';
  ::lolog::LoLog::debug("lotato", "advert: !%s name=\"%.32s\" type=%u hops=%u ts=%lu gps=%s", id_hex,
                        name, (unsigned)atype, (unsigned)path_len, (unsigned long)timestamp,
                        parser.hasLatLon() ? "yes" : "no");
  LotatoNodeRecord rec{};
  memcpy(rec.pub_key, pub_key, 32);
  strncpy(rec.name, name ? name : "", sizeof(rec.name) - 1);
  rec.name[sizeof(rec.name) - 1] = '\0';
  rec.type        = atype;
  rec.last_advert = timestamp;
  rec.gps_lat     = lat;
  rec.gps_lon     = lon;
  rec.magic       = LOTATO_NODE_MAGIC;
  _ingestor.onAdvert(rec, _history, _self_pub_key);
}

bool CliGateway::hasPendingTxInternal() const {
  // Keep the radio awake whenever WiFi is configured so our scan/connect state machine isn't cut
  // off by the host's sleep loop — host polls this before `board.sleep()`.
  if (LotatoConfig::instance().ssid()[0] != '\0') return true;
  if (!_reply_queue.empty()) return true;
  return _ingestor.pendingQueueDepth() > 0;
}

bool CliGateway::matchesAnyRoot(const char* command) const {
  return _router.matchesAnyRoot(command) || _router.matchesGlobalHelp(command);
}

void CliGateway::handleAdminTxtCliInternal(uint32_t sender_ts, const uint8_t pub_key[32],
                                           const uint8_t secret[32], const uint8_t* out_path,
                                           uint8_t out_path_len, uint8_t path_hash_size,
                                           char* command, bool is_retry) {
  if (is_retry) return;  // original packet will be resent via mesh; we own no state for it.

  if (::lolog::LoLog::isVerbose()) {
    size_t cmd_len = strlen(command);
    if (cmd_len) {
      unsigned show = cmd_len > 200u ? 200u : (unsigned)cmd_len;
      ::lolog::LoLog::debug("lotato", "mesh txt cmd len=%u preview: \"%.*s\"%s", (unsigned)cmd_len,
                            (int)show, command, cmd_len > 200u ? "..." : "");
    }
  }

  _reply_scratch[0] = '\0';

  if (_async_busy) {
    strncpy(_reply_scratch, "Err - busy (operation in progress)", kCliReplyCap - 1);
    _reply_scratch[kCliReplyCap - 1] = '\0';
    ::lolog::LoLog::debug("lotato", "cli reply: reject (busy) cmd=%.60s", command);
    enqueueTxtCliReply(pub_key, secret, out_path, out_path_len, path_hash_size, sender_ts,
                       _reply_scratch);
    return;
  }

  _route.valid          = true;
  _route.out_path_len   = out_path_len;
  _route.path_hash_size = path_hash_size;
  memcpy(_route.pub_key, pub_key, 32);
  memcpy(_route.secret, secret, 32);
  memcpy(_route.out_path, out_path, sizeof(_route.out_path));

  dispatchCli(command, sender_ts, _reply_scratch);

  if (!_async_busy) _route.valid = false;

  if (_reply_scratch[0] != '\0') {
    enqueueTxtCliReply(pub_key, secret, out_path, out_path_len, path_hash_size, sender_ts,
                       _reply_scratch);
  }
}

void CliGateway::dispatchCli(const char* command, uint32_t sender_ts, char* reply) {
  if (!reply) return;
  reply[0] = '\0';

  UBaseType_t words = uxTaskGetStackHighWaterMark(nullptr);
  ::lolog::LoLog::info("lotato.stack", "cli pre: free_bytes=%u", (unsigned)(words * sizeof(StackType_t)));

  CliCtx lctx{sender_ts};
  lomessage::Buffer buf(kDispatchBufferBytes);
  if (_router.dispatch(command, buf, &lctx)) {
    ::lolog::LoLog::debug("lotato", "lotato dispatch: out_len=%u truncated=%d", (unsigned)buf.length(),
                          buf.truncated() ? 1 : 0);
    deliverLongReply(sender_ts, buf.c_str(), reply);
  }

  words = uxTaskGetStackHighWaterMark(nullptr);
  ::lolog::LoLog::info("lotato.stack", "cli post: free_bytes=%u", (unsigned)(words * sizeof(StackType_t)));
}

void CliGateway::deliverLongReply(uint32_t sender_ts, const char* text, char* reply) {
  reply[0] = '\0';
  if (!text || !text[0]) return;
  size_t n = strlen(text);
  if (n + 1 <= kCliReplyCap) {
    // Fits in one TXT_MSG — let the caller's mesh send-path deliver it.
    memcpy(reply, text, n + 1);
    return;
  }
  // Long reply: chunk via the FIFO. `_route.valid` is always set by the caller
  // (`handleAdminTxtCliInternal`) before we get here; if that ever changes, we'd silently drop.
  if (_route.valid) {
    enqueueTxtCliReply(_route.pub_key, _route.secret, _route.out_path, _route.out_path_len,
                       _route.path_hash_size, sender_ts, text);
  }
}

bool CliGateway::enqueueTxtCliReply(const uint8_t pub_key[32], const uint8_t secret[32],
                                    const uint8_t* out_path, uint8_t out_path_len,
                                    uint8_t path_hash_size, uint32_t sender_ts, const char* text) {
  CliReplyRoute ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.sender_ts = sender_ts;
  memcpy(ctx.pub_key, pub_key, 32);
  memcpy(ctx.secret, secret, 32);
  memcpy(ctx.out_path, out_path, sizeof(ctx.out_path));
  ctx.out_path_len   = out_path_len;
  ctx.path_hash_size = path_hash_size;

  lomessage::Options opts;
  opts.max_chunk            = kMaxTxtChunk;
  opts.inter_chunk_delay_ms = kInterChunkDelayMs;
  opts.split_flags          = lomessage::CHUNK_ABSORB_LINE_BOUNDARY;

  bool ok = _reply_queue.send(text, &ctx, sizeof(ctx), opts, millis());
  if (ok) {
    ::lolog::LoLog::debug("lotato", "cli reply q: enqueue len=%u", (unsigned)strlen(text));
  } else {
    ::lolog::LoLog::debug("lotato", "cli reply q: enqueue FAILED (empty or OOM)");
  }
  return ok;
}

lomessage::SendResult CliGateway::sendChunk(const uint8_t* data, size_t len, size_t chunk_idx,
                                            size_t total_chunks, bool /*is_final*/, void* user_ctx) {
  auto* ctx = static_cast<CliReplyRoute*>(user_ctx);
  if (!ctx || !_mesh) return lomessage::SendResult::Abandon;
  if (len == 0 || len > kMaxTxtChunk) {
    ::lolog::LoLog::debug("lotato", "cli reply tx: BAD emit_len=%u (max=%u) — abandon job",
                          (unsigned)len, (unsigned)kMaxTxtChunk);
    return lomessage::SendResult::Abandon;
  }

  // Build reply TXT_MSG: 4B ts + 1B flags + chunk. Timestamp must not collide with the requestor's
  // (the client uses the pair as a packet-hash unique).
  uint8_t temp[5 + kMaxTxtChunk];
  uint32_t ts = _mesh->getRTCClock()->getCurrentTimeUnique();
  if (ts == ctx->sender_ts) ts++;
  memcpy(temp, &ts, 4);
  temp[4] = (TXT_TYPE_CLI_DATA << 2);
  memcpy(&temp[5], data, len);

  mesh::Identity recipient(ctx->pub_key);
  mesh::Packet* pkt = _mesh->createDatagram(PAYLOAD_TYPE_TXT_MSG, recipient, ctx->secret, temp,
                                            5 + len);
  if (!pkt) return lomessage::SendResult::Retry;
  if (ctx->out_path_len == OUT_PATH_UNKNOWN) {
    _mesh->sendFlood(pkt, 0, ctx->path_hash_size);
  } else {
    _mesh->sendDirect(pkt, ctx->out_path, ctx->out_path_len, 0);
  }

  ::lolog::LoLog::debug("lotato", "cli reply tx: chunk %u/%u bytes=%u path_len=%u", (unsigned)chunk_idx,
                        (unsigned)total_chunks, (unsigned)len, (unsigned)ctx->out_path_len);
  return lomessage::SendResult::Sent;
}

void CliGateway::onWifiScanComplete(void*, const char* text) {
  if (!g_self || !g_self->_route.valid) return;
  g_self->enqueueTxtCliReply(g_self->_route.pub_key, g_self->_route.secret, g_self->_route.out_path,
                             g_self->_route.out_path_len, g_self->_route.path_hash_size, 0, text);
  g_self->_route.valid = false;
}

void CliGateway::onWifiConnectComplete(void*, bool ok, const char* detail) {
  if (!g_self || !g_self->_route.valid) return;
  char msg[96];
  if (ok) {
    snprintf(msg, sizeof(msg), "OK - WiFi connected (%s)", detail ? detail : "");
  } else {
    snprintf(msg, sizeof(msg), "Err - WiFi connect failed (%s)", detail ? detail : "?");
  }
  g_self->enqueueTxtCliReply(g_self->_route.pub_key, g_self->_route.secret, g_self->_route.out_path,
                             g_self->_route.out_path_len, g_self->_route.path_hash_size, 0, msg);
  g_self->_route.valid = false;
}

/* ── Delegate runtime methods — thin translators between meshcore-native shapes and lotato core. ── */

void Delegate::onAdvertRecv(const mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp,
                            const uint8_t* app_data, size_t app_data_len) {
  CliGateway::instance().onAdvertRecvInternal(id.pub_key, app_data, app_data_len,
                                              packet ? packet->path_len : 0, timestamp);
}

bool Delegate::handleAdminTxtCliIfMine(uint32_t sender_ts, const uint8_t pub_key[32],
                                       const uint8_t secret[32], const uint8_t* out_path,
                                       uint8_t out_path_len, uint8_t path_hash_size, char* command,
                                       bool is_retry) {
  auto& g = CliGateway::instance();
  if (!g.matchesAnyRoot(command)) return false;
  g.handleAdminTxtCliInternal(sender_ts, pub_key, secret, out_path, out_path_len, path_hash_size,
                              command, is_retry);
  return true;
}

void Delegate::service() { CliGateway::instance().tickServices(); }
bool Delegate::isBusy() const { return CliGateway::instance().hasPendingTxInternal(); }

}  // namespace meshcore
}  // namespace lotato

/** Hook called by lofi when an async op marks the session busy/idle. */
extern "C" void lofi_async_busy(bool busy) {
  lotato::meshcore::CliGateway::instance().setCliBusy(busy);
}

#endif  // ESP32
