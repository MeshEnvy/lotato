#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <lomessage/Buffer.h>
#include <lostar/NodeId.h>

/**
 * Meshtastic-specific ingest constants, in-memory node record, and JSON payload builder.
 *
 * Mirrors the Python baseline shape in `potato-mesh/data/mesh_ingestor/handlers/nodeinfo.py`
 * (`/api/nodes` payload: `num`, `lastHeard`, `user.{longName,shortName,hwModel,publicKey?}`,
 * optional `position`).
 */

/** Meshtastic-shaped advert record. Fixed size, copyable by value. */
struct LotatoNodeRecord {
  uint32_t num;                ///< node_num — canonical NodeId (`!%08x`).
  uint32_t last_heard;         ///< unix seconds, 0 when unknown.
  int32_t  latitude_i;         ///< 1e-7 degrees; 0/0 means no position.
  int32_t  longitude_i;
  char     long_name[40];
  char     short_name[8];      ///< 5 chars + NUL + 2 pad.
  uint8_t  public_key[32];
  uint8_t  public_key_len;     ///< 0 when not present, else 32.
  uint16_t hw_model;
  uint8_t  _pad[1];
  uint32_t magic;
};

#define LOTATO_NODE_MAGIC 0x4C4F5441u

namespace lotato {
namespace ingest_platform {

/** Pub-key byte count. Meshtastic carries optional 32B PKI keys. */
inline constexpr size_t kPublicKeyBytes = 32;

inline const char* protocol_name() { return "meshtastic"; }

inline const char* default_ingestor_version() { return "meshtastic-esp32"; }

/* ── internal helpers ──────────────────────────────────────────────── */

inline void _bin_to_hex_lower(const uint8_t* b, size_t n, char* out) {
  static const char* hexd = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = hexd[b[i] >> 4];
    out[i * 2 + 1] = hexd[b[i] & 0x0f];
  }
  out[n * 2] = '\0';
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

/* ── hooks consumed by shared LotatoIngestor / LotatoIngestHistory ──── */

inline lostar::NodeId node_id_from_record(const LotatoNodeRecord& rec) { return rec.num; }

/**
 * Build one Meshtastic node JSON fragment compatible with the Potato Mesh web contract. Emits
 * `publicKey` only when a real 32B key is present; omits `position` when lat/lon are both 0.
 */
inline bool build_node_payload(const LotatoNodeRecord& rec, lostar::NodeId self_id,
                               char* body, size_t body_cap, uint16_t* out_len,
                               lostar::NodeId* node_id_out) {
  char node_id_str[12];
  char ingestor_id[12];
  char long_name_json[48];
  char short_name_json[16];
  char pub_hex[kPublicKeyBytes * 2 + 1];
  pub_hex[0] = '\0';

  if (node_id_out) *node_id_out = rec.num;
  lostar::format_canonical(rec.num, node_id_str, sizeof(node_id_str));
  lostar::format_canonical(self_id, ingestor_id, sizeof(ingestor_id));

  if (!_append_json_escaped(long_name_json, sizeof(long_name_json), rec.long_name)) {
    strncpy(long_name_json, "\"\"", sizeof(long_name_json));
  }
  if (!_append_json_escaped(short_name_json, sizeof(short_name_json), rec.short_name)) {
    strncpy(short_name_json, "\"\"", sizeof(short_name_json));
  }

  const bool has_pk = rec.public_key_len == kPublicKeyBytes;
  if (has_pk) _bin_to_hex_lower(rec.public_key, kPublicKeyBytes, pub_hex);

  const bool has_pos = (rec.latitude_i != 0 || rec.longitude_i != 0);
  uint32_t last_heard = rec.last_heard;

  const char* proto = protocol_name();
  int n;

  // Assemble: {"<id>":{num,lastHeard,protocol,user:{...},[position:{...}]},"ingestor":"<self>"}
  // The `user` object keeps short/long/hwModel always; `publicKey` appended only when present.
  char user_json[160];
  if (has_pk) {
    n = snprintf(user_json, sizeof(user_json),
                 "{\"longName\":%s,\"shortName\":%s,\"hwModel\":%u,\"publicKey\":\"%s\"}",
                 long_name_json, short_name_json, (unsigned)rec.hw_model, pub_hex);
  } else {
    n = snprintf(user_json, sizeof(user_json),
                 "{\"longName\":%s,\"shortName\":%s,\"hwModel\":%u}",
                 long_name_json, short_name_json, (unsigned)rec.hw_model);
  }
  if (n <= 0 || (size_t)n >= sizeof(user_json)) return false;

  if (has_pos) {
    double lat = (double)rec.latitude_i / 10000000.0;
    double lon = (double)rec.longitude_i / 10000000.0;
    n = snprintf(body, body_cap,
                 "{\"%s\":{\"num\":%lu,\"lastHeard\":%lu,\"protocol\":\"%s\",\"user\":%s,"
                 "\"position\":{\"latitude\":%.7f,\"longitude\":%.7f,\"time\":%lu}},"
                 "\"ingestor\":\"%s\"}",
                 node_id_str, (unsigned long)rec.num, (unsigned long)last_heard, proto, user_json,
                 lat, lon, (unsigned long)last_heard, ingestor_id);
  } else {
    n = snprintf(body, body_cap,
                 "{\"%s\":{\"num\":%lu,\"lastHeard\":%lu,\"protocol\":\"%s\",\"user\":%s},"
                 "\"ingestor\":\"%s\"}",
                 node_id_str, (unsigned long)rec.num, (unsigned long)last_heard, proto, user_json,
                 ingestor_id);
  }

  if (n <= 0 || (size_t)n >= body_cap) return false;
  if (out_len) *out_len = (uint16_t)n;
  return true;
}

/** One formatted line for `lotato ingest` CLI output (meshtastic shape). */
inline void format_history_row(const LotatoNodeRecord& rec, uint32_t posted_ms, uint32_t now_ms,
                               lomessage::Buffer& out) {
  char nid[12];
  lostar::format_canonical(rec.num, nid, sizeof(nid));
  uint32_t age_s = (now_ms - posted_ms) / 1000u;
  uint32_t m = age_s / 60u;
  uint32_t s = age_s % 60u;
  if (rec.latitude_i != 0 || rec.longitude_i != 0) {
    out.appendf("%s \"%.32s\" (%.5s) hw=%u posted=%lum%lus ago gps=%.4f,%.4f\n", nid,
                rec.long_name, rec.short_name, (unsigned)rec.hw_model, (unsigned long)m,
                (unsigned long)s, (double)rec.latitude_i / 10000000.0,
                (double)rec.longitude_i / 10000000.0);
  } else {
    out.appendf("%s \"%.32s\" (%.5s) hw=%u posted=%lum%lus ago\n", nid, rec.long_name,
                rec.short_name, (unsigned)rec.hw_model, (unsigned long)m, (unsigned long)s);
  }
}

}  // namespace ingest_platform
}  // namespace lotato
