#include "LotatoIngestHistory.h"

#ifdef ESP32

#include <LotatoConfig.h>
#include <LotatoIngestPlatform.h>
#include <algorithm>

void LotatoIngestHistory::begin() {
  if (!_mtx) _mtx = xSemaphoreCreateMutex();
}

void LotatoIngestHistory::recordPosted(const LotatoNodeRecord& rec, uint32_t now_ms) {
  if (!_mtx) return;
  const size_t cap = (size_t)capacity();
  xSemaphoreTake(_mtx, portMAX_DELAY);
  lostar::NodeId key = lotato::ingest_platform::node_id_from_record(rec);
  auto it = _rows.find(key);
  if (it != _rows.end()) {
    it->second.rec            = rec;
    it->second.last_posted_ms = now_ms;
  } else {
    _rows.emplace(key, Row{rec, now_ms});
  }
  while (_rows.size() > cap) {
    auto victim = _rows.begin();
    uint32_t oldest = victim->second.last_posted_ms;
    for (auto jt = _rows.begin(); jt != _rows.end(); ++jt) {
      if ((int32_t)(jt->second.last_posted_ms - oldest) < 0) {
        victim = jt;
        oldest = jt->second.last_posted_ms;
      }
    }
    _rows.erase(victim);
  }
  xSemaphoreGive(_mtx);
}

void LotatoIngestHistory::snapshot(size_t limit, std::vector<Row>& out) const {
  out.clear();
  if (!_mtx) return;
  xSemaphoreTake(_mtx, portMAX_DELAY);
  out.reserve(_rows.size());
  for (const auto& kv : _rows) out.push_back(kv.second);
  xSemaphoreGive(_mtx);
  std::sort(out.begin(), out.end(), [](const Row& a, const Row& b) {
    return (int32_t)(a.last_posted_ms - b.last_posted_ms) > 0;
  });
  if (limit > 0 && out.size() > limit) out.resize(limit);
}

int LotatoIngestHistory::count() const {
  if (!_mtx) return 0;
  xSemaphoreTake(_mtx, portMAX_DELAY);
  int n = (int)_rows.size();
  xSemaphoreGive(_mtx);
  return n;
}

int LotatoIngestHistory::capacity() const {
  uint32_t c = LotatoConfig::instance().ingestHistoryMax();
  if (c < 1) c = 1;
  if (c > 100) c = 100;
  return (int)c;
}

#endif // ESP32
