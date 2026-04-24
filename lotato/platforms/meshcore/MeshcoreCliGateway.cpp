#if defined(ESP32) && defined(LOTATO_PLATFORM_MESHCORE)

#include "MeshcoreCliGateway.h"

#include <Arduino.h>
#include <cstdio>
#include <cstring>

#include <Mesh.h>
#include <Identity.h>
#include <Packet.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/ClientACL.h>      // OUT_PATH_UNKNOWN
#include <helpers/TxtDataHelpers.h>  // TXT_TYPE_CLI_DATA
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <LotatoCli.h>
#include <LotatoCore.h>
#include <lofi/Lofi.h>
#include <lolog/LoLog.h>
#include <lomessage/Buffer.h>
#include <lomessage/Split.h>
#include <lostar/NodeId.h>

namespace lotato {
namespace meshcore {

namespace {

/** `lomessage::Buffer` capacity for one CLI dispatch (must comfortably hold wifi scan dump). */
constexpr size_t kDispatchBufferBytes = 1280;

/**
 * Inter-chunk delay between reply FIFO emissions. Must be ≥ the LoRa airtime for one ~160-byte
 * TXT_MSG at the configured radio params so we don't outrun the radio TX queue.
 */
constexpr unsigned long kInterChunkDelayMs = 200;

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

/** Build a lostar::NodeRef for the caller from its meshcore-native admin identity. */
void fill_caller_ref(lostar::NodeRef& out, const uint8_t pub_key[32]) {
  out.id = ((uint32_t)pub_key[0] << 24) | ((uint32_t)pub_key[1] << 16) |
           ((uint32_t)pub_key[2] << 8)  | (uint32_t)pub_key[3];
  out.ctx_len = 32 <= LOSTAR_NODEREF_CTX_CAP ? 32 : LOSTAR_NODEREF_CTX_CAP;
  memcpy(out.ctx, pub_key, out.ctx_len);
}

lostar::NodeId self_id_from_pub_key(const uint8_t pub_key[32]) {
  return ((uint32_t)pub_key[0] << 24) | ((uint32_t)pub_key[1] << 16) |
         ((uint32_t)pub_key[2] << 8)  | (uint32_t)pub_key[3];
}

}  // namespace

/* ── CliGateway ─────────────────────────────────────────────────────── */

CliGateway& CliGateway::instance() {
  static CliGateway s_inst;
  return s_inst;
}

CliGateway::CliGateway() = default;

bool CliGateway::cliBusy() const { return LotatoCli::instance().cliBusy(); }
void CliGateway::setCliBusy(bool v) { LotatoCli::instance().setCliBusy(v); }

void CliGateway::begin(lofs::FSys* internal_fs, const uint8_t self_pub_key[32], mesh::Mesh* mesh) {
  g_self = this;
  _mesh = mesh;
  if (self_pub_key) memcpy(_self_pub_key, self_pub_key, sizeof(_self_pub_key));

  core::bringup(internal_fs);

  // MeshCore-specific async-reply wiring: route WiFi scan/connect completions back through the
  // admin TXT reply FIFO (see `onWifiScanComplete` / `onWifiConnectComplete`).
  lofi::Lofi::instance().setScanCompleteCallback(&CliGateway::onWifiScanComplete, nullptr);
  lofi::Lofi::instance().setConnectCompleteCallback(&CliGateway::onWifiConnectComplete, nullptr);

  core::startNetwork();

  UBaseType_t words = uxTaskGetStackHighWaterMark(nullptr);
  ::lolog::LoLog::info("lotato.stack", "begin: free_bytes=%u", (unsigned)(words * sizeof(StackType_t)));
}

void CliGateway::tickServices() {
  core::service(self_id_from_pub_key(_self_pub_key));
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
  LotatoCli::instance().ingestor().onAdvert(rec, LotatoCli::instance().ingestHistory(),
                                             self_id_from_pub_key(_self_pub_key));
}

bool CliGateway::hasPendingTxInternal() const {
  if (core::hasPendingWork()) return true;
  return !_reply_queue.empty();
}

bool CliGateway::matchesAnyRoot(const char* command) const {
  auto& router = LotatoCli::instance().router();
  return router.matchesAnyRoot(command) || router.matchesGlobalHelp(command);
}

void CliGateway::handleAdminTxtCliInternal(uint32_t sender_ts, const uint8_t pub_key[32],
                                           const uint8_t secret[32], const uint8_t* out_path,
                                           uint8_t out_path_len, uint8_t path_hash_size,
                                           char* command, bool is_retry) {
  if (is_retry) return;

  if (::lolog::LoLog::isVerbose()) {
    size_t cmd_len = strlen(command);
    if (cmd_len) {
      unsigned show = cmd_len > 200u ? 200u : (unsigned)cmd_len;
      ::lolog::LoLog::debug("lotato", "mesh txt cmd len=%u preview: \"%.*s\"%s", (unsigned)cmd_len,
                            (int)show, command, cmd_len > 200u ? "..." : "");
    }
  }

  _reply_scratch[0] = '\0';

  if (LotatoCli::instance().cliBusy()) {
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

  dispatchCli(command, sender_ts, pub_key, _reply_scratch);

  if (!LotatoCli::instance().cliBusy()) _route.valid = false;

  if (_reply_scratch[0] != '\0') {
    enqueueTxtCliReply(pub_key, secret, out_path, out_path_len, path_hash_size, sender_ts,
                       _reply_scratch);
  }
}

void CliGateway::dispatchCli(const char* command, uint32_t sender_ts, const uint8_t pub_key[32],
                             char* reply) {
  if (!reply) return;
  reply[0] = '\0';

  UBaseType_t words = uxTaskGetStackHighWaterMark(nullptr);
  ::lolog::LoLog::info("lotato.stack", "cli pre: free_bytes=%u", (unsigned)(words * sizeof(StackType_t)));

  lostar::NodeRef caller{};
  fill_caller_ref(caller, pub_key);

  lomessage::Buffer buf(kDispatchBufferBytes);
  if (LotatoCli::instance().dispatchLine(caller, sender_ts, command, buf)) {
    ::lolog::LoLog::debug("lotato", "lotato dispatch: out_len=%u truncated=%d",
                          (unsigned)buf.length(), buf.truncated() ? 1 : 0);
    deliverLongReply(sender_ts, pub_key, buf.c_str(), reply);
  }

  words = uxTaskGetStackHighWaterMark(nullptr);
  ::lolog::LoLog::info("lotato.stack", "cli post: free_bytes=%u", (unsigned)(words * sizeof(StackType_t)));
}

void CliGateway::deliverLongReply(uint32_t sender_ts, const uint8_t pub_key[32], const char* text,
                                  char* reply) {
  reply[0] = '\0';
  if (!text || !text[0]) return;
  size_t n = strlen(text);
  if (n <= kMaxTxtChunk && n + 1 <= kCliReplyCap) {
    memcpy(reply, text, n + 1);
    return;
  }
  if (_route.valid) {
    enqueueTxtCliReply(_route.pub_key, _route.secret, _route.out_path, _route.out_path_len,
                       _route.path_hash_size, sender_ts, text);
  }
  (void)pub_key;
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
                                            size_t total_chunks, bool /*is_final*/,
                                            void* user_ctx) {
  auto* ctx = static_cast<CliReplyRoute*>(user_ctx);
  if (!ctx || !_mesh) return lomessage::SendResult::Abandon;
  if (len == 0 || len > kMaxTxtChunk) {
    ::lolog::LoLog::debug("lotato", "cli reply tx: BAD emit_len=%u (max=%u) — abandon job",
                          (unsigned)len, (unsigned)kMaxTxtChunk);
    return lomessage::SendResult::Abandon;
  }

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

  ::lolog::LoLog::debug("lotato", "cli reply tx: chunk %u/%u bytes=%u path_len=%u",
                        (unsigned)chunk_idx, (unsigned)total_chunks, (unsigned)len,
                        (unsigned)ctx->out_path_len);
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

/* ── Delegate runtime methods — thin translators. ── */

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

#endif  // ESP32 && LOTATO_PLATFORM_MESHCORE
