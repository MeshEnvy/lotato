#pragma once

#ifdef ESP32

#include <Arduino.h>
#include <cstdint>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <unordered_map>
#include <vector>

#include <lostar/NodeId.h>

#include <LotatoIngestor.h>  // LotatoNodeRecord typedef

/**
 * Ephemeral ingest history keyed by `lostar::NodeId` (the web-canonical low bits). Entries are
 * inserted on each successful `/api/nodes` POST. When the map exceeds `ingest.history_max`
 * capacity, the entry with the oldest `last_posted_ms` is evicted.
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
  mutable std::unordered_map<lostar::NodeId, Row> _rows;
  mutable SemaphoreHandle_t                       _mtx = nullptr;
};

#endif  // ESP32
