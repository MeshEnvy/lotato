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

void LotatoNodeStore::begin(fs::FS* fs) {
  if (!_idx_mtx) _idx_mtx = xSemaphoreCreateMutex();
  _count = 0;
  memset(_index, 0, sizeof(_index));

  if (!_registered) {
    _db.registerTable("mesh_nodes", &LotatoMeshNode_msg, sizeof(LotatoMeshNode));
    _registered = true;
  }
  loadIndex();
}

void LotatoNodeStore::loadIndex() {
  if (!_registered) return;

  std::vector<void*> rows = _db.select("mesh_nodes", nullptr, nullptr, 0);
  int loaded = 0;
  for (void* p : rows) {
    auto* row = static_cast<LotatoMeshNode*>(p);
    LotatoNodeRecord rec{};
    if (!pbToRecord(*row, rec) || rec.magic != LOTATO_NODE_MAGIC) continue;

    if (_count >= MAX) {
      ::lolog::LoLog::debug("lotato", "node store: LoDB row count exceeds MAX; stopping load");
      break;
    }
    int slot = findEmptySlot();
    if (slot < 0) break;

    lodb_uuid_t u = rowUuid(rec.pub_key);
    if (_idx_mtx) xSemaphoreTake(_idx_mtx, portMAX_DELAY);
    memcpy(_index[slot].key, rec.pub_key, 4);
    _index[slot].uuid          = u;
    _index[slot].last_advert    = rec.last_advert;
    _index[slot].last_posted_ms = 0;
    _count++;
    if (_idx_mtx) xSemaphoreGive(_idx_mtx);
    loaded++;
  }
  LoDb::freeRecords(rows);
  ::lolog::LoLog::debug("lotato", "node store: loaded %d nodes from LoDB", loaded);
}

int LotatoNodeStore::findSlot(const uint8_t* pub_key) const {
  for (int i = 0; i < MAX; i++) {
    if (_index[i].last_advert != 0 && memcmp(_index[i].key, pub_key, 4) == 0) {
      return i;
    }
  }
  return -1;
}

int LotatoNodeStore::findEmptySlot() const {
  for (int i = 0; i < MAX; i++) {
    if (_index[i].last_advert == 0) return i;
  }
  return -1;
}

int LotatoNodeStore::evictLRU() {
  int oldest = -1;
  uint32_t oldest_advert = UINT32_MAX;
  if (_idx_mtx) xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  for (int i = 0; i < MAX; i++) {
    if (_index[i].last_advert == 0) continue;
    if (oldest < 0 || _index[i].last_advert < oldest_advert) {
      oldest_advert = _index[i].last_advert;
      oldest        = i;
    }
  }
  if (oldest < 0) {
    if (_idx_mtx) xSemaphoreGive(_idx_mtx);
    return 0;
  }
  lodb_uuid_t u = _index[oldest].uuid;
  if (_idx_mtx) xSemaphoreGive(_idx_mtx);

  LotatoMeshNode row = LotatoMeshNode_init_zero;
  if (_db.get("mesh_nodes", u, &row) == LODB_OK && row.magic == LOTATO_NODE_MAGIC && row.pub_key.size == 32) {
    lotato_ingest_ttl_store().clear(row.pub_key.bytes);
  }
  (void)_db.deleteRecord("mesh_nodes", u);

  if (_idx_mtx) xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  _index[oldest].last_advert    = 0;
  _index[oldest].last_posted_ms = 0;
  _index[oldest].uuid          = 0;
  memset(_index[oldest].key, 0, 4);
  _count--;
  if (_idx_mtx) xSemaphoreGive(_idx_mtx);

  return oldest;
}

bool LotatoNodeStore::persistRecord(const LotatoNodeRecord& rec) {
  if (!_registered) return false;
  LotatoMeshNode row = LotatoMeshNode_init_zero;
  if (!recordToPb(rec, &row)) return false;
  lodb_uuid_t u = rowUuid(rec.pub_key);
  if (_db.update("mesh_nodes", u, &row) == LODB_OK) return true;
  return _db.insert("mesh_nodes", u, &row) == LODB_OK;
}

int LotatoNodeStore::put(const uint8_t* pub_key, const char* name, uint8_t type, uint32_t last_advert, int32_t lat,
                         int32_t lon) {
  if (!pub_key) return -1;

  int slot = -1;
  int slot_mode = 0;  // 0=update, 1=new empty, 2=evict
  if (_idx_mtx) xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  slot = findSlot(pub_key);
  if (slot < 0) {
    if (_count < MAX) {
      slot      = findEmptySlot();
      slot_mode = (slot >= 0) ? 1 : 0;
    }
    if (slot < 0) {
      if (_idx_mtx) xSemaphoreGive(_idx_mtx);
      slot      = evictLRU();
      slot_mode = 2;
      ::lolog::LoLog::debug("lotato", "node store: store full, evicted LRU slot %d", slot);
    } else {
      if (_idx_mtx) xSemaphoreGive(_idx_mtx);
    }
  } else {
    if (_idx_mtx) xSemaphoreGive(_idx_mtx);
  }

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

  ::lolog::LoLog::debug("lotato", "node store put: slot=%d mode=%s idx_count=%u last_advert=%lu type=%u heap=%u", slot,
                        slot_mode == 0 ? "update" : (slot_mode == 1 ? "new" : "evict"), (unsigned)_count,
                        (unsigned long)last_advert, (unsigned)type, (unsigned)ESP.getFreeHeap());

  LotatoNodeRecord existing{};
  if (readRecord(slot, existing) && memcmp(&existing, &rec, RECORD_SIZE) == 0) {
    return slot;
  }

  if (!persistRecord(rec)) {
    ::lolog::LoLog::debug("lotato", "node store: LoDB persist failed slot=%d", slot);
    return -1;
  }

  if (_idx_mtx) xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  bool is_new = (_index[slot].last_advert == 0);
  memcpy(_index[slot].key, pub_key, 4);
  _index[slot].uuid          = rowUuid(pub_key);
  _index[slot].last_advert    = last_advert;
  if (is_new) {
    _count++;
    _index[slot].last_posted_ms = 0;
  }
  if (_idx_mtx) xSemaphoreGive(_idx_mtx);

  return slot;
}

void LotatoNodeStore::logFlushTargetsDebug() const {
  if (!::lolog::LoLog::isVerbose()) return;
  char line[384];
  size_t pos = 0;
  int listed = 0;
  int overflow = 0;
  constexpr int kMaxList = 32;
  LotatoNodeRecord rec;
  line[0] = '\0';

  for (int i = 0; i < MAX; i++) {
    if (_index[i].last_advert == 0) continue;
    if (!readRecord(i, rec)) continue;
    char nid[10];
    nid[0] = '!';
    static const char* hx = "0123456789abcdef";
    for (int b = 0; b < 4; b++) {
      nid[1 + b * 2]     = hx[rec.pub_key[b] >> 4];
      nid[1 + b * 2 + 1] = hx[rec.pub_key[b] & 0x0f];
    }
    nid[9] = '\0';
    if (listed >= kMaxList) {
      overflow++;
      continue;
    }
    int w = snprintf(line + pos, sizeof(line) - pos, "%s%s", pos ? " " : "", nid);
    if (w < 0 || (size_t)w >= sizeof(line) - pos) break;
    pos += (size_t)w;
    listed++;
  }

  if (line[0]) {
    ::lolog::LoLog::debug("lotato", "lotato CLI: flush — node ids: %s", line);
    if (overflow > 0) {
      ::lolog::LoLog::debug("lotato", "lotato CLI: flush — (%d more ids not listed)", overflow);
    }
  }
}

bool LotatoNodeStore::visibleMeshHeard(int slot) const {
  if (slot < 0 || slot >= MAX || !_idx_mtx) return false;
  xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  uint32_t la = _index[slot].last_advert;
  xSemaphoreGive(_idx_mtx);
  if (la == 0) return false;
  time_t now = time(nullptr);
  if (now < (time_t)1700000000) return true;
  uint32_t vis = LotatoConfig::instance().ingestVisibilitySecs();
  return (uint64_t)now - (uint64_t)la <= (uint64_t)vis;
}

bool LotatoNodeStore::dueForIngest(int slot, uint32_t now_ms) const {
  if (slot < 0 || slot >= MAX || !_idx_mtx) return false;
  xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  uint32_t la    = _index[slot].last_advert;
  uint32_t lp_ms = _index[slot].last_posted_ms;
  xSemaphoreGive(_idx_mtx);
  if (la == 0) return false;
  if (lp_ms == 0) return true;
  uint32_t ref_ms = LotatoConfig::instance().ingestRefreshSecs() * 1000u;
  return (uint32_t)(now_ms - lp_ms) >= ref_ms;
}

void LotatoNodeStore::markPosted(int slot, uint32_t now_ms) {
  if (slot < 0 || slot >= MAX) return;
  LotatoNodeRecord rec{};
  if (!readRecord(slot, rec)) return;
  time_t now_wall = time(nullptr);
  if (now_wall >= (time_t)1700000000) {
    lotato_ingest_ttl_store().setLastPostedUnix(rec.pub_key, (uint32_t)now_wall);
  }
  if (!_idx_mtx) return;
  xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  if (_index[slot].last_advert != 0) {
    _index[slot].last_posted_ms = now_ms == 0 ? 1u : now_ms;
  }
  xSemaphoreGive(_idx_mtx);
}

void LotatoNodeStore::flushAllTtl() {
  for (int s = 0; s < MAX; s++) {
    if (_idx_mtx) xSemaphoreTake(_idx_mtx, portMAX_DELAY);
    bool occupied = _index[s].last_advert != 0;
    if (_idx_mtx) xSemaphoreGive(_idx_mtx);
    if (!occupied) continue;
    LotatoNodeRecord r{};
    if (!readRecord(s, r)) continue;
    lotato_ingest_ttl_store().clear(r.pub_key);
    if (_idx_mtx) xSemaphoreTake(_idx_mtx, portMAX_DELAY);
    if (_index[s].last_advert != 0) _index[s].last_posted_ms = 0;
    if (_idx_mtx) xSemaphoreGive(_idx_mtx);
  }
}

void LotatoNodeStore::gcSweepStale() {
  time_t now = time(nullptr);
  if (now < (time_t)1700000000) return;
  uint32_t gc = LotatoConfig::instance().ingestGcStaleSecs();
  int victims[64];
  int nv = 0;
  for (int s = 0; s < MAX && nv < 64; s++) {
    if (_idx_mtx) xSemaphoreTake(_idx_mtx, portMAX_DELAY);
    uint32_t la = _index[s].last_advert;
    if (_idx_mtx) xSemaphoreGive(_idx_mtx);
    if (la == 0) continue;
    if ((uint64_t)now > (uint64_t)la + (uint64_t)gc) victims[nv++] = s;
  }
  for (int i = 0; i < nv; i++) {
    (void)removeSlot(victims[i]);
  }
}

bool LotatoNodeStore::removeSlot(int slot) {
  if (slot < 0 || slot >= MAX) return false;

  LotatoNodeRecord old{};
  if (!readRecord(slot, old) || old.magic != LOTATO_NODE_MAGIC) return false;

  lodb_uuid_t u = 0;
  if (_idx_mtx) xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  if (_index[slot].last_advert == 0) {
    if (_idx_mtx) xSemaphoreGive(_idx_mtx);
    return true;
  }
  u = _index[slot].uuid;
  memset(_index[slot].key, 0, 4);
  _index[slot].last_advert     = 0;
  _index[slot].last_posted_ms  = 0;
  _index[slot].uuid           = 0;
  _count--;
  if (_idx_mtx) xSemaphoreGive(_idx_mtx);

  lotato_ingest_ttl_store().clear(old.pub_key);
  (void)_db.deleteRecord("mesh_nodes", u);
  return true;
}

void LotatoNodeStore::dumpAllNodesDebug() const {
  if (!::lolog::LoLog::isVerbose()) return;
  time_t now_wall = time(nullptr);
  bool wall_ok = now_wall >= (time_t)1700000000;
  uint32_t now_ms = millis();
  uint32_t ref_ms = LotatoConfig::instance().ingestRefreshSecs() * 1000u;
  ::lolog::LoLog::debug("lotato",
                        "contacts dump: count=%d now_wall=%lu now_ms=%lu ref_ms=%lu", _count,
                        (unsigned long)(wall_ok ? (uint32_t)now_wall : 0u), (unsigned long)now_ms,
                        (unsigned long)ref_ms);
  int printed = 0;
  for (int s = 0; s < MAX; s++) {
    if (_idx_mtx) xSemaphoreTake(_idx_mtx, portMAX_DELAY);
    uint32_t la    = _index[s].last_advert;
    uint32_t lp_ms = _index[s].last_posted_ms;
    if (_idx_mtx) xSemaphoreGive(_idx_mtx);
    if (la == 0) continue;
    LotatoNodeRecord rec{};
    if (!readRecord(s, rec)) continue;
    char nid[10];
    nid[0] = '!';
    static const char* hx = "0123456789abcdef";
    for (int b = 0; b < 4; b++) {
      nid[1 + b * 2]     = hx[rec.pub_key[b] >> 4];
      nid[1 + b * 2 + 1] = hx[rec.pub_key[b] & 0x0f];
    }
    nid[9] = '\0';
    uint32_t advert_age = wall_ok && (uint64_t)now_wall > (uint64_t)la ? (uint32_t)((uint64_t)now_wall - (uint64_t)la) : 0u;
    uint32_t since_post_ms = (lp_ms == 0) ? 0u : (uint32_t)(now_ms - lp_ms);
    bool due = (lp_ms == 0) || (ref_ms > 0 && since_post_ms >= ref_ms);
    double lat = (double)rec.gps_lat / 1000000.0;
    double lon = (double)rec.gps_lon / 1000000.0;
    ::lolog::LoLog::debug(
        "lotato",
        "  slot=%d %s type=%u name=\"%.32s\" last_advert=%lu age=%lus lp_ms=%lu since_post=%lums due=%d gps=%.6f,%.6f",
        s, nid, (unsigned)rec.type, rec.name, (unsigned long)la, (unsigned long)advert_age, (unsigned long)lp_ms,
        (unsigned long)since_post_ms, due ? 1 : 0, lat, lon);
    printed++;
  }
  ::lolog::LoLog::debug("lotato", "contacts dump: printed=%d slots", printed);
}

int LotatoNodeStore::countVisibleNodes() const {
  int n = 0;
  for (int s = 0; s < MAX; s++) {
    if (visibleMeshHeard(s)) n++;
  }
  return n;
}

int LotatoNodeStore::countDueNodes() const {
  int n = 0;
  uint32_t now_ms = millis();
  for (int s = 0; s < MAX; s++) {
    if (dueForIngest(s, now_ms)) n++;
  }
  return n;
}

bool LotatoNodeStore::readRecord(int slot, LotatoNodeRecord& out) const {
  if (slot < 0 || slot >= MAX || !_registered || !_idx_mtx) return false;

  xSemaphoreTake(_idx_mtx, portMAX_DELAY);
  if (_index[slot].last_advert == 0) {
    xSemaphoreGive(_idx_mtx);
    return false;
  }
  lodb_uuid_t u = _index[slot].uuid;
  xSemaphoreGive(_idx_mtx);

  LotatoMeshNode row = LotatoMeshNode_init_zero;
  if (_db.get("mesh_nodes", u, &row) != LODB_OK) return false;
  if (!pbToRecord(row, out)) return false;
  return out.magic == LOTATO_NODE_MAGIC;
}

#endif // ESP32
