#pragma once

#ifdef ESP32

#include <Arduino.h>
#include <cstdint>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <unordered_map>
#include <vector>

/**
 * In-memory wire record per node. Fixed size: 84 bytes.
 * magic == LOTATO_NODE_MAGIC when the record is valid.
 */
struct LotatoNodeRecord {
  uint8_t  pub_key[32];
  char     name[32];
  uint8_t  type;
  uint8_t  _pad[3];
  uint32_t last_advert;
  int32_t  gps_lat;
  int32_t  gps_lon;
  uint32_t magic;
};
static_assert(sizeof(LotatoNodeRecord) == 84, "LotatoNodeRecord layout changed");

#define LOTATO_NODE_MAGIC 0x4C4F5441u

/**
 * Ephemeral ingest history keyed by the node's 32-byte public key.
 *
 * Entries are inserted on each successful `/api/nodes` POST. When the map exceeds the
 * `ingest.history_max` capacity, the entry with the oldest `last_posted_ms` is evicted.
 * No persistence: reboots wipe everything, which is intentional.
 */
class LotatoIngestHistory {
public:
  struct Row {
    LotatoNodeRecord rec;
    uint32_t         last_posted_ms;
  };

  void begin();

  /** Insert/update, then evict the oldest-posted entry if the map is over capacity. */
  void recordPosted(const LotatoNodeRecord& rec, uint32_t now_ms);

  /** Copy entries into @p out, sorted desc by `last_posted_ms`, truncated to @p limit (0 = all). */
  void snapshot(size_t limit, std::vector<Row>& out) const;

  int count() const;
  int capacity() const;

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

  static PubKey32 toKey(const uint8_t pub_key[32]) {
    PubKey32 k{};
    memcpy(k.b, pub_key, 32);
    return k;
  }

  mutable std::unordered_map<PubKey32, Row, PubKey32Hash> _rows;
  mutable SemaphoreHandle_t                               _mtx = nullptr;
};

#endif // ESP32
