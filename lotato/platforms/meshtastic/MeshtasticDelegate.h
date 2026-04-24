#pragma once

#ifdef ESP32

#include <cstddef>
#include <cstdint>

#include <lofs/FsBackend.h>

// Meshtastic protobuf types (declared in the fork). Keep as forward decls to avoid pulling the
// full protobuf tree into every TU that includes `Lotato.h`.
struct _meshtastic_MeshPacket;
typedef struct _meshtastic_MeshPacket meshtastic_MeshPacket;
struct _meshtastic_User;
typedef struct _meshtastic_User meshtastic_User;

namespace lotato {
namespace meshtastic {

/**
 * Platform adapter singleton. The host (meshtastic fork) installs it once via `Lotato::init()`
 * and delegates per-event hooks into `Lotato::delegate()`.
 *
 * Step 1 scope (matches `src/platforms/meshtastic/PARITY.md`): adapter bootstrap, ingestor
 * heartbeat, node upsert. Slash-command ingress via `handleTextCommandIfMine` is wired behind
 * the same API so later phases can attach louser guards without touching the fork.
 */
class Delegate {
public:
  /** Forward a NodeInfo / User update into the ingest pipeline. */
  void onNodeInfo(const meshtastic_MeshPacket& mp, const meshtastic_User& user);

  /**
   * If @p mp is a slash-prefixed DM addressed to us, run it through the shared CLI gateway and
   * send the reply back to the sender; returns true when we consumed the packet (host skips).
   * Returns false on non-matching packets (host keeps its normal routing).
   */
  bool handleTextCommandIfMine(const meshtastic_MeshPacket& mp);

  /** Tick internal services (ingest worker, reply FIFO if enabled). */
  void service();

  /** True if lotato has work that should prevent the host from sleeping. */
  bool isBusy() const;
};

/**
 * Meshtastic gateway singleton. Manages Lotato bringup (VFS, config, history, CLI registration)
 * and the delegate facade.
 */
class Gateway {
public:
  static Gateway& instance();

  /**
   * One-shot Lotato bringup. @p internal_fs is the fs driver the fork chose (FSCom on ESP32).
   * @p self_node_num is the host's canonical node number (web canonical = `!%08x`). If
   * @p self_pub_key is non-null and 32 bytes long it is stashed in the delegate for future
   * outgoing heartbeat/user snapshots.
   */
  void begin(lofs::FSys* internal_fs, uint32_t self_node_num,
             const uint8_t* self_pub_key_or_null);

  Delegate& delegate() { return _delegate; }

  uint32_t selfNodeNum() const { return _self_node_num; }
  bool begun() const { return _begun; }

private:
  Gateway() = default;
  Delegate _delegate;
  uint32_t _self_node_num = 0;
  uint8_t  _self_pub_key[32] = {};
  bool     _self_pub_key_set = false;
  bool     _begun = false;
};

}  // namespace meshtastic
}  // namespace lotato

/**
 * Fork-facing neutral API. Same surface as meshcore — include `<Lotato.h>` in the fork and use
 * `Lotato::init(...)`, `Lotato::delegate()`.
 */
namespace Lotato {

using Delegate = lotato::meshtastic::Delegate;

inline void init(lofs::FSys* internal_fs, uint32_t self_node_num,
                 const uint8_t* self_pub_key_or_null = nullptr) {
  lotato::meshtastic::Gateway::instance().begin(internal_fs, self_node_num, self_pub_key_or_null);
}

inline Delegate& delegate() { return lotato::meshtastic::Gateway::instance().delegate(); }

}  // namespace Lotato

#endif  // ESP32
