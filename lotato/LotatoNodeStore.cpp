#include "LotatoNodeStore.h"

#ifdef ESP32

#include <LotatoConfig.h>
#include <LotatoIngestTtl.h>
#include <lolog/LoLog.h>
#include <cstdint>
#include <cstring>
#include <time.h>
#include <vector>

namespace {

constexpr uint64_t kUuidSaltMeshNode = 0x4c6f744e6f646573ull; // 'LotNods'

}  // namespace

LotatoNodeStore::PubKey32 LotatoNodeStore::toKey(const uint8_t pub_key[32]) {
  PubKey32 k{};
  memcpy(k.b, pub_key, 32);
  return k;
}

void LotatoNodeStore::pubKeyToUuidInput(const uint8_t pub_key[32], char hex_out[65]) {
  static const char* hx = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    hex_out[i * 2]     = hx[(pub_key[i] >> 4) & 0x0f];
    hex_out[i * 2 + 1] = hx[pub_key[i] & 0x0f];
  }
  hex_out[64] = '\0';
}

lodb_uuid_t LotatoNodeStore::rowUuid(const uint8_t pub_key[32]) {
  char hex[65];
  pubKeyToUuidInput(pub_key, hex);
  return lodb_new_uuid(hex, kUuidSaltMeshNode);
}

bool LotatoNodeStore::recordToPb(const LotatoNodeRecord& rec, LotatoMeshNode* out) {
  if (!out) return false;
  *out = LotatoMeshNode_init_zero;
  out->pub_key.size = 32;
  memcpy(out->pub_key.bytes, rec.pub_key, 32);
  size_t nl = strnlen(rec.name, sizeof(rec.name));
  if (nl > 32) nl = 32;
  out->name.size = nl;
  memcpy(out->name.bytes, rec.name, nl);
  out->type        = rec.type;
  out->last_advert = rec.last_advert;
  out->gps_lat     = rec.gps_lat;
  out->gps_lon     = rec.gps_lon;
  out->magic       = rec.magic;
  return true;
}

bool LotatoNodeStore::pbToRecord(const LotatoMeshNode& in, LotatoNodeRecord& rec) {
  memset(&rec, 0, sizeof(rec));
  if (in.pub_key.size != 32) return false;
  memcpy(rec.pub_key, in.pub_key.bytes, 32);
  size_t nl = in.name.size;
  if (nl > sizeof(rec.name) - 1) nl = sizeof(rec.name) - 1;
  memcpy(rec.name, in.name.bytes, nl);
  rec.name[nl] = '\0';
  rec.type        = (uint8_t)in.type;
  rec.last_advert = in.last_advert;
  rec.gps_lat     = in.gps_lat;
  rec.gps_lon     = in.gps_lon;
  rec.magic       = in.magic;
  return true;
}

void LotatoNodeStore::begin() {
  if (!_idx_mtx) _idx_mtx = xSemaphoreCreateMutex();
  if (!_db) _db.reset(new LoDb("lotato", "/__ext__"));

  if (!_registered) {
    _db->registerTable("mesh_nodes", &LotatoMeshNode_msg, sizeof(LotatoMeshNode));
    _registered = true;
  }
  loadIndex();
}

void LotatoNodeStore::loadIndex() {
  if (!_registered || !_db) return;

  std::vector<void*> rows = _db->select("mesh_nodes", nullptr, nullptr, 0);
  if (_idx_mtx) xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  _by_key.clear();

  int loaded = 0;
  for (void* p : rows) {
    auto* row = static_cast<LotatoMeshNode*>(p);
    LotatoNodeRecord rec{};
    if (!pbToRecord(*row, rec) || rec.magic != LOTATO_NODE_MAGIC) continue;
    if (_by_key.size() >= (size_t)MAX) {
      ::lolog::LoLog::debug("lotato", "node store: LoDB row count exceeds MAX; stopping load");
      break;
    }
    PubKey32 pk = toKey(rec.pub_key);
    Entry e{};
    e.uuid          = rowUuid(rec.pub_key);
    e.last_advert   = rec.last_advert;
    e.last_posted_ms = 0;
    _by_key.emplace(pk, e);
    loaded++;
  }
  if (_idx_mtx) xSemaphoreGive(_idx_mtx);
  LoDb::freeRecords(rows);
  ::lolog::LoLog::debug("lotato", "node store: loaded %d nodes from LoDB", loaded);
}

void LotatoNodeStore::evictLRU() {
  if (!_db) return;
  PubKey32 victim{};
  lodb_uuid_t u = 0;
  bool found = false;

  if (_idx_mtx) xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  uint32_t oldest_advert = UINT32_MAX;
  for (const auto& kv : _by_key) {
    if (kv.second.last_advert != 0 && kv.second.last_advert < oldest_advert) {
      oldest_advert = kv.second.last_advert;
      victim = kv.first;
      u      = kv.second.uuid;
      found  = true;
    }
  }
  if (!found && !_by_key.empty()) {
    const auto& kv = *_by_key.begin();
    victim = kv.first;
    u      = kv.second.uuid;
    found  = true;
  }
  if (found) _by_key.erase(victim);
  if (_idx_mtx) xSemaphoreGive(_idx_mtx);

  if (!found) return;
  lotato_ingest_ttl_store().clear(victim.b);
  (void)_db->deleteRecord("mesh_nodes", u);
}

bool LotatoNodeStore::persistRecord(const LotatoNodeRecord& rec) {
  if (!_db || !_registered) return false;
  LotatoMeshNode row = LotatoMeshNode_init_zero;
  if (!recordToPb(rec, &row)) return false;
  lodb_uuid_t u = rowUuid(rec.pub_key);
  if (_db->update("mesh_nodes", u, &row) == LODB_OK) return true;
  return _db->insert("mesh_nodes", u, &row) == LODB_OK;
}

bool LotatoNodeStore::put(const uint8_t* pub_key, const char* name, uint8_t type, uint32_t last_advert, int32_t lat,
                          int32_t lon) {
  if (!pub_key) return false;
  PubKey32 pk = toKey(pub_key);

  if (_idx_mtx) xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  bool exists = (_by_key.find(pk) != _by_key.end());
  while (!exists && _by_key.size() >= (size_t)MAX) {
    if (_idx_mtx) xSemaphoreGive(_idx_mtx);
    evictLRU();
    if (_idx_mtx) xSemaphoreTake(_idx_mtx, portMAX_DELAY);
    exists = (_by_key.find(pk) != _by_key.end());
  }
  if (_idx_mtx) xSemaphoreGive(_idx_mtx);

  LotatoNodeRecord rec{};
  memcpy(rec.pub_key, pub_key, 32);
  strncpy(rec.name, name ? name : "", sizeof(rec.name) - 1);
  rec.name[sizeof(rec.name) - 1] = '\0';
  rec.type        = type;
  memset(rec._pad, 0, sizeof(rec._pad));
  rec.last_advert = last_advert;
  rec.gps_lat     = lat;
  rec.gps_lon     = lon;
  rec.magic       = LOTATO_NODE_MAGIC;

  LotatoNodeRecord existing{};
  if (readRecord(pub_key, existing) && memcmp(&existing, &rec, RECORD_SIZE) == 0) return true;

  if (!persistRecord(rec)) {
    ::lolog::LoLog::debug("lotato", "node store: LoDB persist failed");
    return false;
  }

  if (_idx_mtx) xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  Entry e{};
  e.uuid        = rowUuid(pub_key);
  e.last_advert = last_advert;
  auto it       = _by_key.find(pk);
  if (it != _by_key.end()) {
    e.last_posted_ms = it->second.last_posted_ms;
    it->second       = e;
  } else {
    e.last_posted_ms = 0;
    _by_key.emplace(pk, e);
  }
  if (_idx_mtx) xSemaphoreGive(_idx_mtx);

  ::lolog::LoLog::debug("lotato", "node store put: nodes=%u last_advert=%lu type=%u heap=%u", (unsigned)count(),
                        (unsigned long)last_advert, (unsigned)type, (unsigned)ESP.getFreeHeap());
  return true;
}

void LotatoNodeStore::forEachPubKey(LotatoPubWalkFn fn, void* user) const {
  if (!fn || !_idx_mtx) return;
  std::vector<PubKey32> copy;
  xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  copy.reserve(_by_key.size());
  for (const auto& kv : _by_key) copy.push_back(kv.first);
  xSemaphoreGive(_idx_mtx);
  for (const auto& k : copy) fn(k.b, user);
}

bool LotatoNodeStore::visibleMeshHeard(const uint8_t pub_key[32]) const {
  time_t now = time(nullptr);
  if (now < (time_t)1700000000) return true;

  PubKey32 pk = toKey(pub_key);
  uint32_t last_advert = 0;
  if (_idx_mtx) xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  auto it = _by_key.find(pk);
  if (it != _by_key.end()) last_advert = it->second.last_advert;
  if (_idx_mtx) xSemaphoreGive(_idx_mtx);
  if (last_advert == 0) return false;

  const uint32_t now_u = (uint32_t)now;
  const uint32_t vis   = LotatoConfig::instance().ingestVisibilitySecs();
  if (now_u >= last_advert) return (now_u - last_advert) <= vis;
  return true;
}

bool LotatoNodeStore::dueForIngest(const uint8_t pub_key[32], uint32_t now_ms) const {
  if (!visibleMeshHeard(pub_key)) return false;

  PubKey32 pk = toKey(pub_key);
  uint32_t last_posted_ms = 0;
  if (_idx_mtx) xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  auto it = _by_key.find(pk);
  if (it == _by_key.end()) {
    if (_idx_mtx) xSemaphoreGive(_idx_mtx);
    return false;
  }
  last_posted_ms = it->second.last_posted_ms;
  if (_idx_mtx) xSemaphoreGive(_idx_mtx);

  const uint32_t refresh_ms = LotatoConfig::instance().ingestRefreshSecs() * 1000u;
  if (last_posted_ms == 0) return true;
  return (uint32_t)(now_ms - last_posted_ms) >= refresh_ms;
}

void LotatoNodeStore::markPosted(const uint8_t pub_key[32], uint32_t now_ms) {
  PubKey32 pk = toKey(pub_key);
  if (_idx_mtx) xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  auto it = _by_key.find(pk);
  if (it != _by_key.end()) it->second.last_posted_ms = now_ms;
  if (_idx_mtx) xSemaphoreGive(_idx_mtx);

  time_t t = time(nullptr);
  if (t > (time_t)1700000000) lotato_ingest_ttl_store().setLastPostedUnix(pub_key, (uint32_t)t);
}

bool LotatoNodeStore::readRecord(const uint8_t pub_key[32], LotatoNodeRecord& out) const {
  PubKey32 pk = toKey(pub_key);
  lodb_uuid_t u = 0;
  bool in_index = false;
  if (_idx_mtx) xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  auto it = _by_key.find(pk);
  if (it != _by_key.end()) {
    u        = it->second.uuid;
    in_index = true;
  }
  if (_idx_mtx) xSemaphoreGive(_idx_mtx);
  if (!in_index || !_db) return false;

  LotatoMeshNode row = LotatoMeshNode_init_zero;
  if (_db->get("mesh_nodes", u, &row) != LODB_OK) return false;
  return pbToRecord(row, out) && out.magic == LOTATO_NODE_MAGIC;
}

int LotatoNodeStore::count() const {
  if (!_idx_mtx) return 0;
  xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  int n = (int)_by_key.size();
  xSemaphoreGive(_idx_mtx);
  return n;
}

void LotatoNodeStore::flushAllTtl() {
  std::vector<PubKey32> keys;
  if (_idx_mtx) xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  keys.reserve(_by_key.size());
  for (auto& kv : _by_key) {
    kv.second.last_posted_ms = 0;
    keys.push_back(kv.first);
  }
  if (_idx_mtx) xSemaphoreGive(_idx_mtx);
  for (const auto& k : keys) lotato_ingest_ttl_store().clear(k.b);
}

void LotatoNodeStore::gcSweepStale() {
  time_t now = time(nullptr);
  if (now < (time_t)1700000000) return;

  const uint32_t stale = LotatoConfig::instance().ingestGcStaleSecs();
  const uint32_t now_u = (uint32_t)now;
  std::vector<PubKey32> victims;
  if (_idx_mtx) xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  for (const auto& kv : _by_key) {
    uint32_t la = kv.second.last_advert;
    if (la == 0) continue;
    if (now_u >= la && now_u - la >= stale) victims.push_back(kv.first);
  }
  if (_idx_mtx) xSemaphoreGive(_idx_mtx);
  for (const auto& k : victims) (void)remove(k.b);
}

bool LotatoNodeStore::remove(const uint8_t pub_key[32]) {
  PubKey32 pk = toKey(pub_key);
  lodb_uuid_t u = 0;
  bool had = false;
  if (_idx_mtx) xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  auto it = _by_key.find(pk);
  if (it != _by_key.end()) {
    u   = it->second.uuid;
    had = true;
    _by_key.erase(it);
  }
  if (_idx_mtx) xSemaphoreGive(_idx_mtx);
  if (!had || !_db) return false;
  lotato_ingest_ttl_store().clear(pub_key);
  return _db->deleteRecord("mesh_nodes", u) == LODB_OK;
}

int LotatoNodeStore::countVisibleNodes() const {
  std::vector<PubKey32> copy;
  if (!_idx_mtx) return 0;
  xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  copy.reserve(_by_key.size());
  for (const auto& kv : _by_key) copy.push_back(kv.first);
  xSemaphoreGive(_idx_mtx);
  int n = 0;
  for (const auto& k : copy) {
    if (visibleMeshHeard(k.b)) n++;
  }
  return n;
}

int LotatoNodeStore::countDueNodes() const {
  const uint32_t now_ms = millis();
  std::vector<PubKey32> copy;
  if (!_idx_mtx) return 0;
  xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  copy.reserve(_by_key.size());
  for (const auto& kv : _by_key) copy.push_back(kv.first);
  xSemaphoreGive(_idx_mtx);
  int n = 0;
  for (const auto& k : copy) {
    if (dueForIngest(k.b, now_ms)) n++;
  }
  return n;
}

void LotatoNodeStore::logFlushTargetsDebug() const {
  if (!::lolog::LoLog::isVerbose()) return;
  std::vector<PubKey32> copy;
  if (!_idx_mtx) return;
  xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  copy.reserve(_by_key.size());
  for (const auto& kv : _by_key) copy.push_back(kv.first);
  xSemaphoreGive(_idx_mtx);

  char line[384];
  line[0] = '\0';
  constexpr int kMaxList = 32;
  int listed = 0;
  static const char* hx = "0123456789abcdef";
  for (const auto& k : copy) {
    if (listed >= kMaxList) break;
    char nid[10];
    nid[0] = '!';
    for (int b = 0; b < 4; b++) {
      nid[1 + b * 2]     = hx[k.b[b] >> 4];
      nid[1 + b * 2 + 1] = hx[k.b[b] & 0x0f];
    }
    nid[9] = '\0';
    size_t p = strlen(line);
    if (p + 1 + 9 < sizeof(line)) {
      snprintf(line + p, sizeof(line) - p, "%s%s", p ? " " : "", nid);
      listed++;
    }
  }
  if (line[0]) {
    int extra = (int)copy.size() - listed;
    if (extra > 0) {
      ::lolog::LoLog::debug("lotato", "flush TTL targets (%d more): %s", extra, line);
    } else {
      ::lolog::LoLog::debug("lotato", "flush TTL targets: %s", line);
    }
  }
}

void LotatoNodeStore::dumpAllNodesDebug() const {
  if (!::lolog::LoLog::isVerbose()) return;
  std::vector<PubKey32> copy;
  if (!_idx_mtx) return;
  xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  copy.reserve(_by_key.size());
  for (const auto& kv : _by_key) copy.push_back(kv.first);
  xSemaphoreGive(_idx_mtx);

  for (const auto& k : copy) {
    LotatoNodeRecord rec{};
    if (!readRecord(k.b, rec)) continue;
    char pkhex[65];
    pubKeyToUuidInput(k.b, pkhex);
    ::lolog::LoLog::debug("lotato", "node %.16s… name=\"%.32s\" type=%u last_advert=%lu lat=%ld lon=%ld", pkhex,
                          rec.name, (unsigned)rec.type, (unsigned long)rec.last_advert, (long)rec.gps_lat,
                          (long)rec.gps_lon);
  }
}

#endif // ESP32
