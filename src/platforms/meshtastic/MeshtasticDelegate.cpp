#include "MeshtasticDelegate.h"

#if defined(ESP32) && defined(LOTATO_PLATFORM_MESHTASTIC)

#include <Arduino.h>
#include <cstring>

#include <LotatoCli.h>
#include <LotatoConfig.h>
#include <LotatoIngestor.h>
#include <LotatoIngestHistory.h>
#include <LotatoIngestPlatform.h>
#include <lofi/Lofi.h>
#include <lolog/LoLog.h>
#include <lomessage/Buffer.h>
#include <lostar/LoStar.h>
#include <lostar/NodeId.h>

// Now we need the real protobuf types. Included here (delegate .cpp) — not in the public header
// — so consumers of Lotato.h don't pull in the meshtastic generated tree.
#include <mesh/generated/meshtastic/mesh.pb.h>

#include <louser/Auth.h>
#include <louser/Engine.h>
#include <louser/Guard.h>

namespace lotato {
namespace meshtastic {

namespace {

constexpr size_t kDispatchBufferBytes = 1280;

Gateway* g_self = nullptr;

locommand::Engine g_user_engine("user");

/** Pack node_num into a NodeRef. `ctx` is unused on meshtastic (id == node_num == web canonical). */
void fill_node_ref(lostar::NodeRef& out, uint32_t node_num) {
  out = {};
  out.id = node_num;
}

/** Convert (mp, user) into a LotatoNodeRecord that the shared ingestor will merge/post. */
void fill_record_from_nodeinfo(LotatoNodeRecord& rec, const meshtastic_MeshPacket& mp,
                               const meshtastic_User& user) {
  memset(&rec, 0, sizeof(rec));
  rec.num = mp.from;
  rec.last_heard = mp.rx_time;
  rec.hw_model = (uint16_t)user.hw_model;
  strncpy(rec.long_name, user.long_name, sizeof(rec.long_name) - 1);
  rec.long_name[sizeof(rec.long_name) - 1] = '\0';
  strncpy(rec.short_name, user.short_name, sizeof(rec.short_name) - 1);
  rec.short_name[sizeof(rec.short_name) - 1] = '\0';
  if (user.public_key.size == 32) {
    memcpy(rec.public_key, user.public_key.bytes, 32);
    rec.public_key_len = 32;
  }
  rec.magic = LOTATO_NODE_MAGIC;
}

/**
 * Apply Step 1 guard policy (mirrors Phase C table in the plan):
 *  - Always open: `hi`, `bye`, `whoami`, `user register`, `user login`, `lotato status`, `wifi status`.
 *  - require_user: `wifi scan`, `user logout`, `user whoami`.
 *  - require_admin: everything else that mutates state.
 */
void attach_meshtastic_guards(LotatoCli& cli) {
  auto& lotato_eng = cli.lotatoEngine();
  lotato_eng.setGuardFor("pause",    &louser::require_admin);
  lotato_eng.setGuardFor("resume",   &louser::require_admin);
  lotato_eng.setGuardFor("endpoint", &louser::require_admin);
  lotato_eng.setGuardFor("auth",     &louser::require_admin);
  lotato_eng.setGuardFor("ingest",   &louser::require_admin);

  auto& wifi_eng = cli.wifiEngine();
  wifi_eng.setGuardFor("scan",    &louser::require_user);
  wifi_eng.setGuardFor("connect", &louser::require_admin);
  wifi_eng.setGuardFor("forget",  &louser::require_admin);

  auto& config_eng = cli.configEngine();
  config_eng.setGuardFor("set",   &louser::require_admin);
  config_eng.setGuardFor("unset", &louser::require_admin);

  g_user_engine.setGuardFor("logout",         &louser::require_user);
  g_user_engine.setGuardFor("whoami",         &louser::require_user);
  g_user_engine.setGuardFor("bye",            &louser::require_user);
  g_user_engine.setGuardFor("sessions.clear", &louser::require_admin);
  g_user_engine.setGuardFor("delete",         &louser::require_admin);
  g_user_engine.setGuardFor("rename",         &louser::require_admin);
  g_user_engine.setGuardFor("passwd",         &louser::require_admin);
}

}  // namespace

/* ── Gateway ─────────────────────────────────────────────────────────── */

Gateway& Gateway::instance() {
  static Gateway s;
  return s;
}

void Gateway::begin(lofs::FSys* internal_fs, uint32_t self_node_num,
                    const uint8_t* self_pub_key_or_null) {
  g_self = this;
  _self_node_num = self_node_num;
  if (self_pub_key_or_null) {
    memcpy(_self_pub_key, self_pub_key_or_null, sizeof(_self_pub_key));
    _self_pub_key_set = true;
  }

  // Stage 1 — platform bringup.
  LoStar::boot({{"/__int__", internal_fs}});

  // Stage 2 — Lotato stack.
  LotatoConfig::instance().load();
  lofi::Lofi::instance().begin();
  LotatoCli::instance().ingestHistory().begin();

  // Shared CLI tree + louser `user` engine + guard policy.
  LotatoCli::instance().defaultRegister();
  louser::register_engine(g_user_engine);
  LotatoCli::instance().registerExtraEngine(&g_user_engine);
  attach_meshtastic_guards(LotatoCli::instance());

  lotato_register_sta_dns_override();

  _begun = true;
}

/* ── Delegate runtime methods ─────────────────────────────────────── */

void Delegate::onNodeInfo(const meshtastic_MeshPacket& mp, const meshtastic_User& user) {
  if (!g_self || !g_self->_begun) return;
  LotatoNodeRecord rec;
  fill_record_from_nodeinfo(rec, mp, user);
  LotatoCli::instance().ingestor().onAdvert(rec, LotatoCli::instance().ingestHistory(),
                                             g_self->_self_node_num);
}

bool Delegate::handleTextCommandIfMine(const meshtastic_MeshPacket& mp) {
  if (!g_self || !g_self->_begun) return false;
  if (mp.which_payload_variant != meshtastic_MeshPacket_decoded_tag) return false;
  if (mp.decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP) return false;

  const char* text = (const char*)mp.decoded.payload.bytes;
  size_t len = mp.decoded.payload.size;
  if (len == 0 || text[0] != '/') return false;

  // Build a NUL-terminated C-string + strip the leading '/'.
  char command[256];
  size_t copy = len - 1;
  if (copy > sizeof(command) - 1) copy = sizeof(command) - 1;
  memcpy(command, text + 1, copy);
  command[copy] = '\0';

  // Apply `hi` / `bye` / `whoami` alias rewriting so handlers live in the `user` engine.
  char rewritten[256];
  const char* dispatch_line = command;
  if (louser::rewrite_alias(command, rewritten, sizeof(rewritten))) dispatch_line = rewritten;

  lostar::NodeRef caller;
  fill_node_ref(caller, mp.from);

  lomessage::Buffer out(kDispatchBufferBytes);
  if (!LotatoCli::instance().dispatchLine(caller, mp.rx_time, dispatch_line, out)) {
    return false;
  }

  // Reply delivery: Step 1 leaves the reply TX path up to the fork (via a future hook on
  // mesh/Router.cpp) since chunked DM reply plumbing is out-of-scope for the heartbeat + node
  // upsert milestone. Log the reply for operator visibility in the meantime.
  ::lolog::LoLog::debug("lotato", "meshtastic cli: from=0x%08x reply len=%u", (unsigned)mp.from,
                        (unsigned)out.length());
  return true;
}

void Delegate::service() {
  if (!g_self || !g_self->_begun) return;
  lofi::Lofi::instance().serviceWifiScan();
  LotatoCli::instance().ingestor().serviceTick(g_self->_self_node_num);
}

bool Delegate::isBusy() const {
  if (!g_self || !g_self->_begun) return false;
  if (LotatoConfig::instance().ssid()[0] != '\0') return true;
  return LotatoCli::instance().ingestor().pendingQueueDepth() > 0;
}

}  // namespace meshtastic
}  // namespace lotato

/** Hook called by lofi when an async op marks the session busy/idle. Shared across platforms. */
extern "C" void lofi_async_busy(bool busy) {
  lotato::LotatoCli::instance().setCliBusy(busy);
}

#endif  // ESP32 && LOTATO_PLATFORM_MESHTASTIC
