#pragma once

#ifdef ESP32

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lodb/LoDB.h>

#include "lotato_mesh_node.pb.h"

/** Maximum number of nodes tracked for ingest. Override via build flag. */
#ifndef LOTATO_NODE_STORE_MAX
#define LOTATO_NODE_STORE_MAX 4000
#endif

/**
 * In-memory / wire record per node. Fixed size: 84 bytes.
 * magic == LOTATO_NODE_MAGIC when the slot is occupied.
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
 * An in-memory index tracks {LoDB uuid, pub_key prefix, last_advert, last_posted_ms} for fast
 * dedup, LRU eviction, and ingest scheduling. Scheduling throttles off `millis()`, not wall
 * clock, so it works even before NTP. The `ingest_ttl` LoDB table on `/__ram__` is kept for
 * introspection only and is wiped on reboot.
 */
class LotatoNodeStore {
public:
  static constexpr int    MAX         = LOTATO_NODE_STORE_MAX;
  static constexpr size_t RECORD_SIZE = sizeof(LotatoNodeRecord);

  /**
   * Register LoDB tables and rebuild the in-memory index from disk.
   * @param fs optional: if set, removes orphaned `/lotato-nodes.bin` / `.tmp` from pre-LoDB builds.
   */
  void begin(fs::FS* fs);

  /**
   * Insert or update a node. Returns the slot index, or -1 on error.
   * Evicts the least-recently-heard node when the store is full.
   */
  int put(const uint8_t* pub_key, const char* name, uint8_t type,
          uint32_t last_advert, int32_t lat, int32_t lon);

  /** True if this occupied slot should be included in the next ingest batch (uptime-throttled). */
  bool dueForIngest(int slot, uint32_t now_ms) const;

  /** Record successful POST at @p now_ms (`millis()`); also updates RAM-TTL for introspection. */
  void markPosted(int slot, uint32_t now_ms);

  /** Read the persisted record for slot. Returns false on I/O error. */
  bool readRecord(int slot, LotatoNodeRecord& out) const;

  /** Total number of occupied slots. */
  int count() const { return _count; }

  /** Clear last-post time for ALL occupied slots, making them due again. */
  void flushAllTtl();

  /** Remove stale slots not mesh-heard for `lotato.ingest.gc_stale_secs` (no-op if wall clock invalid). */
  void gcSweepStale();

  /** Remove one occupied slot from LoDB + index + ingest TTL. */
  bool removeSlot(int slot);

  /** Count occupied slots that pass the mesh-heard visibility window. */
  int countVisibleNodes() const;

  /** Count occupied slots that are visible and due for ingest. */
  int countDueNodes() const;

  /** Log !id list for occupied slots when debug is on (truncated if very many nodes). */
  void logFlushTargetsDebug() const;

  /** Verbose per-slot dump: id, name, type, last_advert, last_posted_ms, gps. Debug log only. */
  void dumpAllNodesDebug() const;

private:
  bool visibleMeshHeard(int slot) const;

  struct Entry {
    lodb_uuid_t uuid{};
    uint8_t     key[4];         // first 4 bytes of pub_key for fast match
    uint32_t    last_advert;    // cached last_advert for LRU eviction
    uint32_t    last_posted_ms; // millis() of last successful post; 0 = never posted (or flushed)
  };

  int     _count = 0;
  Entry   _index[MAX]{};
  mutable SemaphoreHandle_t _idx_mtx = nullptr;

  LoDb     _db{"lotato", "/__ext__"};
  bool     _registered = false;

  static lodb_uuid_t rowUuid(const uint8_t pub_key[32]);
  static void        pubKeyToUuidInput(const uint8_t pub_key[32], char hex_out[65]);
  static bool        recordToPb(const LotatoNodeRecord& rec, LotatoMeshNode* out);
  static bool        pbToRecord(const LotatoMeshNode& in, LotatoNodeRecord& out);

  int  findSlot(const uint8_t* pub_key) const;
  int  findEmptySlot() const;
  int  evictLRU();
  bool persistRecord(const LotatoNodeRecord& rec);
  void loadIndex();
};

#endif // ESP32
