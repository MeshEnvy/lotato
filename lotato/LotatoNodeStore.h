#pragma once

#ifdef ESP32

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lodb/LoDB.h>
#include <memory>
#include <unordered_map>

#include "lotato_mesh_node.pb.h"

/** Maximum number of nodes tracked for ingest. Override via build flag. */
#ifndef LOTATO_NODE_STORE_MAX
#define LOTATO_NODE_STORE_MAX 4000
#endif

/**
 * In-memory / wire record per node. Fixed size: 84 bytes.
 * magic == LOTATO_NODE_MAGIC when the record is valid.
 */
struct LotatoNodeRecord {
  uint8_t pub_key[32]; // full 32-byte public key
  char    name[32];    // advertised name (null-terminated)
  uint8_t type;        // firmware advert type (see lotato/platforms/*/ ingest delegate)
  uint8_t _pad[3];
  uint32_t last_advert; // Unix timestamp from advert packet
  int32_t  gps_lat;     // latitude  × 1e6 (0 if none)
  int32_t  gps_lon;     // longitude × 1e6 (0 if none)
  uint32_t magic;       // 0x4C4F5441 ('LOTA') if valid
};
static_assert(sizeof(LotatoNodeRecord) == 84, "LotatoNodeRecord layout changed");

#define LOTATO_NODE_MAGIC 0x4C4F5441u

/**
 * Persistent node store for Lotato ingest, backed by LoDB on `/__ext__` (LittleFS on ESP32).
 *
 * RAM state is keyed by the node's 32-byte public key (MeshCore identity). Each entry caches
 * LoDB uuid, last_advert, and last_posted_ms for ingest throttling. The `ingest_ttl` LoDB table
 * on `/__ram__` is kept for introspection only and is wiped on reboot.
 */
class LotatoNodeStore {
public:
  static constexpr int    MAX         = LOTATO_NODE_STORE_MAX;
  static constexpr size_t RECORD_SIZE = sizeof(LotatoNodeRecord);

  /** Register LoDB tables and rebuild the in-memory index from disk. */
  void begin();

  /**
   * Insert or update a node keyed by @p pub_key. Evicts LRU when at capacity.
   * @return false on persistence failure.
   */
  bool put(const uint8_t* pub_key, const char* name, uint8_t type, uint32_t last_advert, int32_t lat, int32_t lon);

  /** True if this node should be included in the next ingest batch (uptime-throttled). */
  bool dueForIngest(const uint8_t pub_key[32], uint32_t now_ms) const;

  /** Record successful POST at @p now_ms (`millis()`); also updates RAM-TTL for introspection. */
  void markPosted(const uint8_t pub_key[32], uint32_t now_ms);

  /** Read the persisted record for @p pub_key. Returns false if unknown or I/O error. */
  bool readRecord(const uint8_t pub_key[32], LotatoNodeRecord& out) const;

  /** Total tracked nodes. */
  int count() const;

  /** Clear last-post time for all nodes (re-ingest). */
  void flushAllTtl();

  /** Remove nodes not mesh-heard for `lotato.ingest.gc_stale_secs` (no-op if wall clock invalid). */
  void gcSweepStale();

  /** Remove one node from LoDB + RAM index + ingest TTL. */
  bool remove(const uint8_t pub_key[32]);

  /** Count nodes that pass the mesh-heard visibility window. */
  int countVisibleNodes() const;

  /** Count nodes that are visible and due for ingest. */
  int countDueNodes() const;

  /** Log !id list when debug is on (truncated if very many nodes). */
  void logFlushTargetsDebug() const;

  /** Verbose per-node dump (debug only). */
  void dumpAllNodesDebug() const;

  /** Iterate all tracked public keys (for ingest batching). Callback must not re-enter the store on the same task in a way that deadlocks. */
  using LotatoPubWalkFn = void (*)(const uint8_t pub_key[32], void* user);
  void forEachPubKey(LotatoPubWalkFn fn, void* user) const;

private:
  struct PubKey32 {
    uint8_t b[32];
    bool operator==(const PubKey32& o) const { return memcmp(b, o.b, 32) == 0; }
  };
  struct PubKey32Hash {
    size_t operator()(const PubKey32& k) const noexcept {
      size_t h = 2166136261u;
      for (int i = 0; i < 32; i++) {
        h ^= (unsigned char)k.b[i];
        h *= 16777619u;
      }
      return h;
    }
  };

  struct Entry {
    lodb_uuid_t uuid{};
    uint32_t    last_advert{};
    uint32_t    last_posted_ms{}; // millis() of last successful post; 0 = never (or flushed)
  };

  mutable std::unordered_map<PubKey32, Entry, PubKey32Hash> _by_key;
  mutable SemaphoreHandle_t                                 _idx_mtx = nullptr;

  /** Created in begin() so LoDb ctor logging never runs during global MyMesh construction. */
  mutable std::unique_ptr<LoDb> _db;
  bool                          _registered = false;

  static PubKey32    toKey(const uint8_t pub_key[32]);
  static lodb_uuid_t rowUuid(const uint8_t pub_key[32]);
  static void        pubKeyToUuidInput(const uint8_t pub_key[32], char hex_out[65]);
  static bool        recordToPb(const LotatoNodeRecord& rec, LotatoMeshNode* out);
  static bool        pbToRecord(const LotatoMeshNode& in, LotatoNodeRecord& out);

  bool visibleMeshHeard(const uint8_t pub_key[32]) const;
  void evictLRU();
  bool persistRecord(const LotatoNodeRecord& rec);
  void loadIndex();
};

#endif // ESP32
