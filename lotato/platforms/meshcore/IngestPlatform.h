#pragma once

#include <MeshCore.h>
#include <helpers/AdvertDataHelpers.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <lomessage/Buffer.h>
#include <lostar/NodeId.h>

/**
 * MeshCore-specific ingest constants, in-memory node record, and JSON payload builder. The
 * shared ingestor treats `LotatoNodeRecord` as opaque (memcpy'd by value) and calls the
 * `lotato::ingest_platform::*` hooks below to translate it into a web payload.
 */

/** In-memory wire record per node (meshcore shape). Fixed size: 84 bytes. */
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

namespace lotato {
namespace ingest_platform {

inline constexpr size_t kPublicKeyBytes = PUB_KEY_SIZE;

inline const char* protocol_name() { return "meshcore"; }

/** Default ``version`` when ``LOTATO_INGESTOR_VERSION`` is not set at compile time. */
inline const char* default_ingestor_version() { return "meshcore-esp32"; }

/** Potato-mesh ``user.role`` string, or nullptr to omit the field. */
inline const char* role_for_advert_type(uint8_t adv_type) {
  switch (adv_type) {
    case ADV_TYPE_CHAT:     return "COMPANION";
    case ADV_TYPE_REPEATER: return "REPEATER";
    case ADV_TYPE_ROOM:     return "ROOM_SERVER";
    case ADV_TYPE_SENSOR:   return "SENSOR";
    default:                return nullptr;
  }
}

/* ── internal helpers (hex + JSON escaping) ────────────────────────── */

inline void _bin_to_hex_lower(const uint8_t* b, size_t n, char* out) {
  static const char* hexd = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = hexd[b[i] >> 4];
    out[i * 2 + 1] = hexd[b[i] & 0x0f];
  }
  out[n * 2] = '\0';
}

inline lostar::NodeId _id_from_pub_key(const uint8_t pub_key[kPublicKeyBytes]) {
  return ((uint32_t)pub_key[0] << 24) | ((uint32_t)pub_key[1] << 16) |
         ((uint32_t)pub_key[2] << 8)  | (uint32_t)pub_key[3];
}

inline bool _append_json_escaped(char* dest, size_t dest_size, const char* name) {
  if (dest_size < 3) return false;
  char* p = dest;
  char* end = dest + dest_size - 1;
  *p++ = '"';
  while (name && *name && p < end - 1) {
    unsigned char c = (unsigned char)*name++;
    if (c == '"' || c == '\\') {
      if (p >= end - 2) break;
      *p++ = '\\';
      *p++ = (char)c;
    } else if (c >= 32 && c < 127) {
      *p++ = (char)c;
    } else {
      *p++ = '?';
    }
  }
  if (p >= end) return false;
  *p++ = '"';
  *p = '\0';
  return true;
}

/**
 * Build one MeshCore node JSON fragment matching the current web contract. The shape is exactly
 * what the shared ingestor emitted prior to the hoist — byte-for-byte.
 *
 * @param rec       advert record (meshcore-shaped: pub_key, name, type, gps).
 * @param self_id   canonical `!xxxxxxxx` low bits for the ingestor's own identity.
 * @param body/cap  output buffer.
 * @param out_len   on success, set to bytes written (excluding NUL).
 * @param node_id_out optional — receives the resolved per-record NodeId (first 4 bytes of pub_key).
 */
inline bool build_node_payload(const LotatoNodeRecord& rec, lostar::NodeId self_id,
                               char* body, size_t body_cap, uint16_t* out_len,
                               lostar::NodeId* node_id_out) {
  char node_id_str[12];
  char ingestor_id[12];
  char pub_hex[kPublicKeyBytes * 2 + 1];
  char name_json[40];
  char short_hex[5];

  lostar::NodeId id = _id_from_pub_key(rec.pub_key);
  if (node_id_out) *node_id_out = id;
  lostar::format_canonical(id, node_id_str, sizeof(node_id_str));
  lostar::format_canonical(self_id, ingestor_id, sizeof(ingestor_id));

  _bin_to_hex_lower(rec.pub_key, kPublicKeyBytes, pub_hex);
  if (!_append_json_escaped(name_json, sizeof(name_json), rec.name)) {
    strncpy(name_json, "\"?\"", sizeof(name_json));
    name_json[sizeof(name_json) - 1] = '\0';
  }
  _bin_to_hex_lower(rec.pub_key, 2, short_hex);

  uint32_t num = ((uint32_t)rec.pub_key[0] << 24) | ((uint32_t)rec.pub_key[1] << 16) |
                 ((uint32_t)rec.pub_key[2] << 8) | (uint32_t)rec.pub_key[3];
  uint32_t last_heard = rec.last_advert;
  if (last_heard == 0) last_heard = (uint32_t)(millis() / 1000);

  const char* role = role_for_advert_type(rec.type);
  const char* proto = protocol_name();
  int n;

  if (rec.gps_lat != 0 || rec.gps_lon != 0) {
    double lat = (double)rec.gps_lat / 1000000.0;
    double lon = (double)rec.gps_lon / 1000000.0;
    if (role) {
      n = snprintf(body, body_cap,
                   "{\"%s\":{\"num\":%lu,\"lastHeard\":%lu,\"protocol\":\"%s\","
                   "\"user\":{\"longName\":%s,\"shortName\":\"%s\",\"publicKey\":\"%s\",\"role\":\"%s\"},"
                   "\"position\":{\"latitude\":%.6f,\"longitude\":%.6f,\"time\":%lu}},"
                   "\"ingestor\":\"%s\"}",
                   node_id_str, (unsigned long)num, (unsigned long)last_heard, proto, name_json,
                   short_hex, pub_hex, role, lat, lon, (unsigned long)last_heard, ingestor_id);
    } else {
      n = snprintf(body, body_cap,
                   "{\"%s\":{\"num\":%lu,\"lastHeard\":%lu,\"protocol\":\"%s\","
                   "\"user\":{\"longName\":%s,\"shortName\":\"%s\",\"publicKey\":\"%s\"},"
                   "\"position\":{\"latitude\":%.6f,\"longitude\":%.6f,\"time\":%lu}},"
                   "\"ingestor\":\"%s\"}",
                   node_id_str, (unsigned long)num, (unsigned long)last_heard, proto, name_json,
                   short_hex, pub_hex, lat, lon, (unsigned long)last_heard, ingestor_id);
    }
  } else {
    if (role) {
      n = snprintf(body, body_cap,
                   "{\"%s\":{\"num\":%lu,\"lastHeard\":%lu,\"protocol\":\"%s\","
                   "\"user\":{\"longName\":%s,\"shortName\":\"%s\",\"publicKey\":\"%s\",\"role\":\"%s\"}},"
                   "\"ingestor\":\"%s\"}",
                   node_id_str, (unsigned long)num, (unsigned long)last_heard, proto, name_json,
                   short_hex, pub_hex, role, ingestor_id);
    } else {
      n = snprintf(body, body_cap,
                   "{\"%s\":{\"num\":%lu,\"lastHeard\":%lu,\"protocol\":\"%s\","
                   "\"user\":{\"longName\":%s,\"shortName\":\"%s\",\"publicKey\":\"%s\"}},"
                   "\"ingestor\":\"%s\"}",
                   node_id_str, (unsigned long)num, (unsigned long)last_heard, proto, name_json,
                   short_hex, pub_hex, ingestor_id);
    }
  }

  if (n <= 0 || (size_t)n >= body_cap) return false;
  if (out_len) *out_len = (uint16_t)n;
  return true;
}

/** Canonical NodeId of this record (first 4 bytes of pub_key). */
inline lostar::NodeId node_id_from_record(const LotatoNodeRecord& rec) {
  return _id_from_pub_key(rec.pub_key);
}

/**
 * Append one formatted line to @p out for `lotato ingest` CLI output, reusing the legacy shape
 * so meshcore-side operators see the same view post-refactor.
 */
inline void format_history_row(const LotatoNodeRecord& rec, uint32_t posted_ms, uint32_t now_ms,
                               lomessage::Buffer& out) {
  char nid[10];
  nid[0] = '!';
  static const char* const hexd = "0123456789abcdef";
  for (int i = 0; i < 4; i++) {
    nid[1 + i * 2]     = hexd[(rec.pub_key[i] >> 4) & 0x0f];
    nid[1 + i * 2 + 1] = hexd[rec.pub_key[i] & 0x0f];
  }
  nid[9] = '\0';
  uint32_t age_s = (now_ms - posted_ms) / 1000u;
  uint32_t m = age_s / 60u;
  uint32_t s = age_s % 60u;
  if (rec.gps_lat != 0 || rec.gps_lon != 0) {
    out.appendf("%s \"%.32s\" type=%u posted=%lum%lus ago gps=%.4f,%.4f\n", nid, rec.name,
                (unsigned)rec.type, (unsigned long)m, (unsigned long)s,
                (double)rec.gps_lat / 1000000.0, (double)rec.gps_lon / 1000000.0);
  } else {
    out.appendf("%s \"%.32s\" type=%u posted=%lum%lus ago\n", nid, rec.name,
                (unsigned)rec.type, (unsigned long)m, (unsigned long)s);
  }
}

}  // namespace ingest_platform
}  // namespace lotato
