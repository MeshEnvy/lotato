#include "MeshtasticDelegate.h"

#if defined(ESP32) && defined(LOTATO_PLATFORM_MESHTASTIC)

#include <Arduino.h>
#include <cstring>

#include <LotatoCli.h>
#include <LotatoCore.h>
#include <LotatoIngestPlatform.h>
#include <locommand/Router.h>
#include <lolog/LoLog.h>
#include <lomessage/Buffer.h>
#include <lostar/NodeId.h>

// Now we need the real protobuf types. Included here (delegate .cpp) — not in the public header
// — so consumers of Lotato.h don't pull in the meshtastic generated tree.
#include <mesh/generated/meshtastic/mesh.pb.h>

// Fork-provided helpers (defined in meshtastic/src/main.cpp). Kept extern "C" so the lotato lib
// doesn't need access to Router/MeshService/NodeDB headers (Thread.h, etc. aren't reachable here).
extern "C" uint32_t lotato_meshtastic_self_nodenum(void);
extern "C" void     lotato_meshtastic_send_text_reply(uint32_t to, const char* text, unsigned len);

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
/** Skip leading whitespace and UTF-8 BOM so `/?` still matches after phone/app framing. */
static void skip_leading_framing(const uint8_t*& data, size_t& len) {
  while (len > 0 && (data[0] == ' ' || data[0] == '\t' || data[0] == '\r' || data[0] == '\n')) {
    data++;
    len--;
  }
  if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
    data += 3;
    len -= 3;
  }
  while (len > 0 && (data[0] == ' ' || data[0] == '\t' || data[0] == '\r' || data[0] == '\n')) {
    data++;
    len--;
  }
}

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

/**
 * ESP32: starting WiFi (Lofi) before NimBLE::init can deadlock/hang the radio stack.
 * Meshtastic calls this from `setBluetoothEnable(true)` after any NimBLE `setup()` (see
 * main-esp32.cpp), and also unconditionally on boards without BLE. Thin shim into the shared
 * core so network bringup policy lives in one place.
 */
extern "C" void lotato_meshtastic_start_lofi_after_ble(void) {
  lotato::core::startNetwork();
}

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

  core::bringup(internal_fs);
  // Network bringup is deferred: meshtastic must wait until after NimBLE::init to avoid a
  // BT/WiFi coexistence hang. `lotato_meshtastic_start_lofi_after_ble` calls `core::startNetwork`.

  // Meshtastic-specific additions to the shared CLI: `louser` engine + admin/user guards.
  louser::register_engine(g_user_engine);
  LotatoCli::instance().registerExtraEngine(&g_user_engine);
  attach_meshtastic_guards(LotatoCli::instance());

  _begun = true;
}

/* ── Delegate runtime methods ─────────────────────────────────────── */

void Delegate::onNodeInfo(const meshtastic_MeshPacket& mp, const meshtastic_User& user) {
  if (!g_self || !g_self->begun()) return;
  LotatoNodeRecord rec;
  fill_record_from_nodeinfo(rec, mp, user);
  LotatoCli::instance().ingestor().onAdvert(rec, LotatoCli::instance().ingestHistory(),
                                             g_self->selfNodeNum());
}

bool Delegate::handleTextCommandIfMine(const meshtastic_MeshPacket& mp) {
  const uint32_t self = lotato_meshtastic_self_nodenum();
  const size_t raw_len = (size_t)mp.decoded.payload.size;
  char hex[32 + 1] = {0};
  {
    const uint8_t* b = mp.decoded.payload.bytes;
    size_t show = raw_len < 16 ? raw_len : 16;
    size_t w = 0;
    for (size_t i = 0; i < show && w + 3 < sizeof(hex); i++) {
      static const char kH[] = "0123456789abcdef";
      hex[w++] = kH[b[i] >> 4];
      hex[w++] = kH[b[i] & 0x0f];
      if (i + 1 < show) hex[w++] = ' ';
    }
    hex[w] = '\0';
  }
  ::lolog::LoLog::info(
      "lotato.cli",
      "rx: begun=%d variant=%d portnum=%d from=0x%08x to=0x%08x self=0x%08x len=%u hex=[%s]",
      g_self ? (int)g_self->begun() : 0, (int)mp.which_payload_variant,
      (int)mp.decoded.portnum, (unsigned)mp.from, (unsigned)mp.to, (unsigned)self,
      (unsigned)raw_len, hex);

  if (!g_self || !g_self->begun()) {
    ::lolog::LoLog::info("lotato.cli", "skip: gateway not begun");
    return false;
  }
  if (mp.which_payload_variant != meshtastic_MeshPacket_decoded_tag) {
    ::lolog::LoLog::info("lotato.cli", "skip: payload not decoded (variant=%d)",
                         (int)mp.which_payload_variant);
    return false;
  }
  if (mp.decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP) {
    ::lolog::LoLog::info("lotato.cli", "skip: portnum=%d != TEXT_MESSAGE_APP",
                         (int)mp.decoded.portnum);
    return false;
  }
  if (mp.to != self) {
    ::lolog::LoLog::info("lotato.cli", "skip: not DM to us (to=0x%08x self=0x%08x)",
                         (unsigned)mp.to, (unsigned)self);
    return false;
  }

  const uint8_t* data = mp.decoded.payload.bytes;
  size_t len = raw_len;
  skip_leading_framing(data, len);
  if (len == 0) {
    ::lolog::LoLog::info("lotato.cli", "skip: empty after trim (raw_len=%u)", (unsigned)raw_len);
    return false;
  }

  char command[256];
  auto& router_cli = LotatoCli::instance().router();

  if (data[0] == '/') {
    if (len < 2) {
      ::lolog::LoLog::info("lotato.cli", "skip: bare '/' (len=%u)", (unsigned)len);
      return false;
    }
    size_t copy = len - 1;
    if (copy > sizeof(command) - 1) copy = sizeof(command) - 1;
    memcpy(command, data + 1, copy);
    command[copy] = '\0';
    ::lolog::LoLog::info("lotato.cli", "slash cmd: \"%s\"", command);
  } else {
    size_t copy = len;
    if (copy > sizeof(command) - 1) copy = sizeof(command) - 1;
    memcpy(command, data, copy);
    command[copy] = '\0';
    if (!router_cli.matchesGlobalHelp(command)) {
      ::lolog::LoLog::info("lotato.cli",
                           "skip: non-slash \"%s\" is not global help — not consuming",
                           command);
      return false;
    }
    ::lolog::LoLog::info("lotato.cli", "bare help: \"%s\"", command);
  }

  char rewritten[256];
  const char* dispatch_line = command;
  if (louser::rewrite_alias(command, rewritten, sizeof(rewritten))) {
    dispatch_line = rewritten;
    ::lolog::LoLog::info("lotato.cli", "alias rewrite: \"%s\" -> \"%s\"", command, rewritten);
  }

  const bool is_root = router_cli.matchesAnyRoot(dispatch_line);
  const bool is_help = router_cli.matchesGlobalHelp(dispatch_line);
  ::lolog::LoLog::info("lotato.cli", "match: root=%d help=%d line=\"%s\"", (int)is_root,
                       (int)is_help, dispatch_line);
  if (!is_root && !is_help) {
    ::lolog::LoLog::info("lotato.cli", "skip: no engine root / not help");
    return false;
  }

  lostar::NodeRef caller;
  fill_node_ref(caller, mp.from);

  lomessage::Buffer out(kDispatchBufferBytes);
  if (!LotatoCli::instance().dispatchLine(caller, mp.rx_time, dispatch_line, out)) {
    ::lolog::LoLog::info("lotato.cli", "dispatchLine returned false for \"%s\"", dispatch_line);
    return false;
  }

  ::lolog::LoLog::info(
      "lotato.cli", "reply: from=0x%08x len=%u truncated=%d preview=\"%.60s\"",
      (unsigned)mp.from, (unsigned)out.length(), out.truncated() ? 1 : 0, out.c_str());
  lotato_meshtastic_send_text_reply(mp.from, out.c_str(), (unsigned)out.length());
  return true;
}

void Delegate::service() {
  if (!g_self || !g_self->begun()) return;
  core::service(g_self->selfNodeNum());
}

bool Delegate::isBusy() const {
  if (!g_self || !g_self->begun()) return false;
  return core::hasPendingWork();
}

}  // namespace meshtastic
}  // namespace lotato

#endif  // ESP32 && LOTATO_PLATFORM_MESHTASTIC
