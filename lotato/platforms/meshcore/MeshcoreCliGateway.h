#pragma once

#ifdef ESP32

#include <cstddef>
#include <cstdint>

#include <MeshCore.h>

namespace mesh {
class Mesh;
class Packet;
class Identity;
}  // namespace mesh

#include <locommand/Engine.h>
#include <locommand/Router.h>
#include <lofs/FsBackend.h>
#include <lomessage/Queue.h>
#include <LotatoIngestHistory.h>
#include <LotatoIngestor.h>

namespace lotato {
namespace meshcore {

/** Admin TXT_MSG single-packet reply cap — must match upstream `main.cpp reply[160]` and
 *  `mesh temp[5 + 160]`. Replies ≤ this fit in one TXT_MSG; longer replies chunk via the FIFO. */
static constexpr size_t kCliReplyCap = 160;
/** Max text bytes per TXT_MSG chunk; equal to `kCliReplyCap` since both are the mesh wire budget. */
static constexpr size_t kMaxTxtChunk = 160;

/**
 * Platform adapter singleton. The host hands over the mesh pointer + self identity at
 * `Lotato::init` time; every runtime hook lives here as a method, and the delegate owns all
 * mesh-TX primitives internally (no host-supplied callbacks).
 */
class Delegate {
public:
  /** Push the advert into the ingest queue; parses the meshcore wire format internally. */
  void onAdvertRecv(const mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp,
                    const uint8_t* app_data, size_t app_data_len);

  /**
   * Conditionally handle an admin TXT_MSG CLI command — only if @p command is a Lotato root
   * (`lotato …` / `wifi …` / `config …`). On hit: busy gate, route snapshot (with the client's
   *  pub_key + shared_secret so async / chunked replies can re-encrypt later), dispatch, reply
   *  enqueue; returns true (host skips the upstream reply path). On miss: no side effects;
   *  returns false so the host's upstream reply path runs unchanged, tracking upstream drift-free.
   *
   * @p is_retry causes an immediate true return without dispatching (mesh resends the original).
   */
  bool handleAdminTxtCliIfMine(uint32_t sender_ts, const uint8_t pub_key[32], const uint8_t secret[32],
                               const uint8_t* out_path, uint8_t out_path_len, uint8_t path_hash_size,
                               char* command, bool is_retry);

  /** Tick internal services (WiFi scan SM, ingest batch worker, reply chunk FIFO). */
  void service();

  /** True if lotato has work that should prevent the host from sleeping. */
  bool isBusy() const;
};

/**
 * Owns the lotato / wifi / config CLI stack, async WiFi reply bookkeeping, and the chunked TXT_MSG
 * reply FIFO for a MeshCore ESP32 repeater. The fork only touches this via the `Lotato::meshcore*`
 * wrappers below.
 */
class CliGateway : private lomessage::Sink {
public:
  static CliGateway& instance();

  /**
   * One-shot Lotato bringup: VFS quiet, LoFS binding + mount, LoLog schema + settings, ingest + node
   * store start, CLI engines + async WiFi callbacks. Stores `mesh` for later TX primitives.
   *
   * @p internal_fs is the fs driver the host firmware chose (SPIFFS, LittleFS, ...); lofs reuses
   * that mounted volume so we don't fight the host over the `spiffs` partition.
   * @p self_pub_key is the host's own 32-byte identity; stored so the ingest worker can skip it.
   * @p mesh is the host's `mesh::Mesh*` (owns TX primitives and RTC); must outlive the gateway.
   */
  void begin(lofs::FSys* internal_fs, const uint8_t self_pub_key[32], mesh::Mesh* mesh);

  /** Accessor for the singleton delegate (fork calls this via `Lotato::delegate()`). */
  Delegate& delegate() { return _delegate; }

  /* ── Internal: invoked by `Delegate` methods and async callbacks; not for direct fork use. ── */
  void tickServices();
  bool cliBusy() const { return _async_busy; }
  void setCliBusy(bool v) { _async_busy = v; }
  void onAdvertRecvInternal(const uint8_t pub_key[32], const uint8_t* app_data, size_t app_data_len,
                            uint8_t path_len, uint32_t timestamp);
  bool hasPendingTxInternal() const;
  bool matchesAnyRoot(const char* command) const;
  void handleAdminTxtCliInternal(uint32_t sender_ts, const uint8_t pub_key[32],
                                 const uint8_t secret[32], const uint8_t* out_path,
                                 uint8_t out_path_len, uint8_t path_hash_size, char* command,
                                 bool is_retry);
  void dispatchCli(const char* command, uint32_t sender_ts, char* reply);
  bool enqueueTxtCliReply(const uint8_t pub_key[32], const uint8_t secret[32],
                          const uint8_t* out_path, uint8_t out_path_len, uint8_t path_hash_size,
                          uint32_t sender_ts, const char* text);
  LotatoIngestor&      ingestor() { return _ingestor; }
  LotatoIngestHistory& ingestHistory() { return _history; }

  /** Route context for async replies (wifi scan/connect completion) + chunked CLI replies. */
  struct RouteCtx {
    bool    valid = false;
    uint8_t pub_key[32]{};        ///< recipient's mesh identity
    uint8_t secret[32]{};         ///< shared secret for encrypting the reply datagram
    uint8_t out_path[MAX_PATH_SIZE]{};
    uint8_t out_path_len = 0;
    uint8_t path_hash_size = 0;
  };
  RouteCtx& route() { return _route; }

private:
  CliGateway();

  void registerLotatoEngine();
  void registerWifiEngine();
  void deliverLongReply(uint32_t sender_ts, const char* text, char* reply);

  static void onWifiScanComplete(void*, const char* text);
  static void onWifiConnectComplete(void*, bool ok, const char* detail);

  lomessage::SendResult sendChunk(const uint8_t* data, size_t len, size_t chunk_idx,
                                  size_t total_chunks, bool is_final, void* user_ctx) override;

  Delegate            _delegate;
  mesh::Mesh*         _mesh = nullptr;
  uint8_t             _self_pub_key[32]{};
  LotatoIngestor      _ingestor;
  LotatoIngestHistory _history;
  locommand::Engine _eng_lotato{"lotato"};
  locommand::Engine _eng_wifi{"wifi"};
  locommand::Engine _eng_config{"config"};
  locommand::Router _router;
  lomessage::Queue  _reply_queue;
  RouteCtx          _route;
  /** Reply scratch for admin TXT_MSG CLI; kept off the `Mesh::onRecvPacket` stack frame. */
  char              _reply_scratch[kCliReplyCap];
  bool              _async_busy = false;
};

}  // namespace meshcore
}  // namespace lotato

/**
 * Fork-facing public API. Install the delegate once via `init`, reach back into it at runtime via
 * `delegate()`. Delegate methods translate meshcore-native shapes into lotato-neutral operations
 * internally — `Lotato::*` surface stays platform-agnostic.
 *
 * Consumers should `#include <Lotato.h>` rather than this platform-specific header.
 */
namespace Lotato {

using Delegate = lotato::meshcore::Delegate;

/** Max stack-allocated reply size for admin TXT_MSG CLI (matches `main.cpp reply[]`). */
inline constexpr size_t kCliReplyCap = lotato::meshcore::kCliReplyCap;
/** Max text bytes per TXT_MSG chunk (wire payload budget for `sendTxtCliChunk`). */
inline constexpr size_t kMaxTxtChunk = lotato::meshcore::kMaxTxtChunk;

inline void init(lofs::FSys* internal_fs, const uint8_t self_pub_key[32], mesh::Mesh* mesh) {
  lotato::meshcore::CliGateway::instance().begin(internal_fs, self_pub_key, mesh);
}

inline Delegate& delegate() { return lotato::meshcore::CliGateway::instance().delegate(); }

}  // namespace Lotato

#endif  // ESP32
