#include "LotatoIngestPayload.h"

#ifdef ESP32

#include <Arduino.h>
#include <cstdio>
#include <cstring>

namespace lotato {

namespace {

constexpr size_t kPublicKeyBytes = 32;

void bin_to_hex_lower(const uint8_t* b, size_t n, char* out) {
  static const char* hexd = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[i * 2]     = hexd[b[i] >> 4];
    out[i * 2 + 1] = hexd[b[i] & 0x0f];
  }
  out[n * 2] = '\0';
}

bool append_json_escaped(char* dest, size_t dest_size, const char* name) {
  if (dest_size < 3) return false;
  char* p   = dest;
  char* end = dest + dest_size - 1;
  *p++      = '"';
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
  *p   = '\0';
  return true;
}

const char* meshcore_role_for(uint8_t advert_type) {
  switch (advert_type) {
    case LOSTAR_ADVERT_TYPE_CHAT:     return "COMPANION";
    case LOSTAR_ADVERT_TYPE_REPEATER: return "REPEATER";
    case LOSTAR_ADVERT_TYPE_ROOM:     return "ROOM_SERVER";
    case LOSTAR_ADVERT_TYPE_SENSOR:   return "SENSOR";
    default:                          return nullptr;
  }
}

/** Meshtastic: lat/lon are 1e-7; meshcore: 1e-6. */
double coord_scale(uint8_t protocol) {
  return protocol == LOSTAR_PROTOCOL_MESHCORE ? 1000000.0 : 10000000.0;
}

bool build_meshtastic_payload(const lostar_NodeAdvert& rec, lostar::NodeId self_id, char* body,
                              size_t body_cap, uint16_t* out_len) {
  char node_id_str[12];
  char ingestor_id[12];
  char long_name_json[48];
  char short_name_json[16];
  char pub_hex[kPublicKeyBytes * 2 + 1];
  pub_hex[0] = '\0';

  lostar::format_canonical(rec.num, node_id_str, sizeof(node_id_str));
  lostar::format_canonical(self_id, ingestor_id, sizeof(ingestor_id));

  if (!append_json_escaped(long_name_json, sizeof(long_name_json), rec.long_name)) {
    strncpy(long_name_json, "\"\"", sizeof(long_name_json));
  }
  if (!append_json_escaped(short_name_json, sizeof(short_name_json), rec.short_name)) {
    strncpy(short_name_json, "\"\"", sizeof(short_name_json));
  }

  const bool has_pk = rec.public_key_len == kPublicKeyBytes;
  if (has_pk) bin_to_hex_lower(rec.public_key, kPublicKeyBytes, pub_hex);

  const bool has_pos    = (rec.latitude_i != 0 || rec.longitude_i != 0);
  const uint32_t last_h = rec.last_heard;

  char user_json[160];
  int  n;
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
    n          = snprintf(body, body_cap,
                 "{\"%s\":{\"num\":%lu,\"lastHeard\":%lu,\"protocol\":\"meshtastic\",\"user\":%s,"
                          "\"position\":{\"latitude\":%.7f,\"longitude\":%.7f,\"time\":%lu}},"
                          "\"ingestor\":\"%s\"}",
                 node_id_str, (unsigned long)rec.num, (unsigned long)last_h, user_json, lat, lon,
                 (unsigned long)last_h, ingestor_id);
  } else {
    n = snprintf(body, body_cap,
                 "{\"%s\":{\"num\":%lu,\"lastHeard\":%lu,\"protocol\":\"meshtastic\",\"user\":%s},"
                 "\"ingestor\":\"%s\"}",
                 node_id_str, (unsigned long)rec.num, (unsigned long)last_h, user_json,
                 ingestor_id);
  }

  if (n <= 0 || (size_t)n >= body_cap) return false;
  if (out_len) *out_len = (uint16_t)n;
  return true;
}

bool build_meshcore_payload(const lostar_NodeAdvert& rec, lostar::NodeId self_id, char* body,
                            size_t body_cap, uint16_t* out_len) {
  char node_id_str[12];
  char ingestor_id[12];
  char pub_hex[kPublicKeyBytes * 2 + 1];
  char name_json[40];
  char short_hex[5];

  lostar::format_canonical(rec.num, node_id_str, sizeof(node_id_str));
  lostar::format_canonical(self_id, ingestor_id, sizeof(ingestor_id));

  bin_to_hex_lower(rec.public_key, kPublicKeyBytes, pub_hex);
  if (!append_json_escaped(name_json, sizeof(name_json), rec.long_name)) {
    strncpy(name_json, "\"?\"", sizeof(name_json));
    name_json[sizeof(name_json) - 1] = '\0';
  }
  bin_to_hex_lower(rec.public_key, 2, short_hex);

  uint32_t last_h = rec.last_heard;
  if (last_h == 0) last_h = (uint32_t)(millis() / 1000);

  const char* role = meshcore_role_for(rec.advert_type);
  int         n;

  if (rec.latitude_i != 0 || rec.longitude_i != 0) {
    double lat = (double)rec.latitude_i / 1000000.0;
    double lon = (double)rec.longitude_i / 1000000.0;
    if (role) {
      n = snprintf(body, body_cap,
                   "{\"%s\":{\"num\":%lu,\"lastHeard\":%lu,\"protocol\":\"meshcore\","
                   "\"user\":{\"longName\":%s,\"shortName\":\"%s\",\"publicKey\":\"%s\",\"role\":\"%s\"},"
                   "\"position\":{\"latitude\":%.6f,\"longitude\":%.6f,\"time\":%lu}},"
                   "\"ingestor\":\"%s\"}",
                   node_id_str, (unsigned long)rec.num, (unsigned long)last_h, name_json,
                   short_hex, pub_hex, role, lat, lon, (unsigned long)last_h, ingestor_id);
    } else {
      n = snprintf(body, body_cap,
                   "{\"%s\":{\"num\":%lu,\"lastHeard\":%lu,\"protocol\":\"meshcore\","
                   "\"user\":{\"longName\":%s,\"shortName\":\"%s\",\"publicKey\":\"%s\"},"
                   "\"position\":{\"latitude\":%.6f,\"longitude\":%.6f,\"time\":%lu}},"
                   "\"ingestor\":\"%s\"}",
                   node_id_str, (unsigned long)rec.num, (unsigned long)last_h, name_json,
                   short_hex, pub_hex, lat, lon, (unsigned long)last_h, ingestor_id);
    }
  } else {
    if (role) {
      n = snprintf(body, body_cap,
                   "{\"%s\":{\"num\":%lu,\"lastHeard\":%lu,\"protocol\":\"meshcore\","
                   "\"user\":{\"longName\":%s,\"shortName\":\"%s\",\"publicKey\":\"%s\",\"role\":\"%s\"}},"
                   "\"ingestor\":\"%s\"}",
                   node_id_str, (unsigned long)rec.num, (unsigned long)last_h, name_json,
                   short_hex, pub_hex, role, ingestor_id);
    } else {
      n = snprintf(body, body_cap,
                   "{\"%s\":{\"num\":%lu,\"lastHeard\":%lu,\"protocol\":\"meshcore\","
                   "\"user\":{\"longName\":%s,\"shortName\":\"%s\",\"publicKey\":\"%s\"}},"
                   "\"ingestor\":\"%s\"}",
                   node_id_str, (unsigned long)rec.num, (unsigned long)last_h, name_json,
                   short_hex, pub_hex, ingestor_id);
    }
  }

  if (n <= 0 || (size_t)n >= body_cap) return false;
  if (out_len) *out_len = (uint16_t)n;
  return true;
}

}  // namespace

const char* protocol_name(const lostar_NodeAdvert& rec) {
  switch (rec.protocol) {
    case LOSTAR_PROTOCOL_MESHTASTIC: return "meshtastic";
    case LOSTAR_PROTOCOL_MESHCORE:   return "meshcore";
    default:                         return "unknown";
  }
}

const char* default_ingestor_version(const lostar_NodeAdvert& rec) {
  switch (rec.protocol) {
    case LOSTAR_PROTOCOL_MESHTASTIC: return "meshtastic-esp32";
    case LOSTAR_PROTOCOL_MESHCORE:   return "meshcore-esp32";
    default:                         return "lostar-esp32";
  }
}

bool build_node_payload(const lostar_NodeAdvert& rec, lostar::NodeId self_id, char* body,
                        size_t body_cap, uint16_t* out_len, lostar::NodeId* node_id_out) {
  if (node_id_out) *node_id_out = rec.num;
  switch (rec.protocol) {
    case LOSTAR_PROTOCOL_MESHCORE:
      return build_meshcore_payload(rec, self_id, body, body_cap, out_len);
    case LOSTAR_PROTOCOL_MESHTASTIC:
    default:
      return build_meshtastic_payload(rec, self_id, body, body_cap, out_len);
  }
}

void format_history_row(const lostar_NodeAdvert& rec, uint32_t posted_ms, uint32_t now_ms,
                        lomessage::Buffer& out) {
  char     nid[12];
  lostar::format_canonical(rec.num, nid, sizeof(nid));
  uint32_t age_s = (now_ms - posted_ms) / 1000u;
  uint32_t m     = age_s / 60u;
  uint32_t s     = age_s % 60u;
  const double scale   = coord_scale(rec.protocol);
  const bool   has_pos = (rec.latitude_i != 0 || rec.longitude_i != 0);

  if (rec.protocol == LOSTAR_PROTOCOL_MESHCORE) {
    if (has_pos) {
      out.appendf("%s \"%.32s\" type=%u posted=%lum%lus ago gps=%.4f,%.4f\n", nid, rec.long_name,
                  (unsigned)rec.advert_type, (unsigned long)m, (unsigned long)s,
                  (double)rec.latitude_i / scale, (double)rec.longitude_i / scale);
    } else {
      out.appendf("%s \"%.32s\" type=%u posted=%lum%lus ago\n", nid, rec.long_name,
                  (unsigned)rec.advert_type, (unsigned long)m, (unsigned long)s);
    }
  } else {
    if (has_pos) {
      out.appendf("%s \"%.32s\" (%.5s) hw=%u posted=%lum%lus ago gps=%.4f,%.4f\n", nid,
                  rec.long_name, rec.short_name, (unsigned)rec.hw_model, (unsigned long)m,
                  (unsigned long)s, (double)rec.latitude_i / scale,
                  (double)rec.longitude_i / scale);
    } else {
      out.appendf("%s \"%.32s\" (%.5s) hw=%u posted=%lum%lus ago\n", nid, rec.long_name,
                  rec.short_name, (unsigned)rec.hw_model, (unsigned long)m, (unsigned long)s);
    }
  }
}

}  // namespace lotato

extern "C" void lotato_format_history_row(const lostar_NodeAdvert& rec, uint32_t posted_ms,
                                          uint32_t now_ms, lomessage::Buffer& out) {
  lotato::format_history_row(rec, posted_ms, now_ms, out);
}

#endif  // ESP32
