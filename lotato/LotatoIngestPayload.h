#pragma once

#ifdef ESP32

#include <cstddef>
#include <cstdint>

#include <lostar/Types.h>
#include <lomessage/Buffer.h>
#include <lostar/NodeId.h>

/**
 * Lotato ingest payload helpers. Given a `lostar_NodeAdvert`, produce either the Potato Mesh
 * `/api/nodes` JSON body or the one-line CLI history row. Dispatches on `rec.protocol`.
 */

namespace lotato {

/** Protocol string ("meshtastic" / "meshcore" / "unknown") for a given advert. */
const char* protocol_name(const lostar_NodeAdvert& rec);

/** Default ingestor version string for the advert's protocol. */
const char* default_ingestor_version(const lostar_NodeAdvert& rec);

/** Canonical node id extraction (always `rec.num`). */
inline lostar::NodeId node_id_from_record(const lostar_NodeAdvert& rec) { return rec.num; }

/**
 * Build a single `/api/nodes` fragment matching the protocol-specific wire shape. On success
 * writes a NUL-terminated JSON object into @p body and sets `*out_len` and optionally
 * `*node_id_out`. Returns false on buffer overflow.
 */
bool build_node_payload(const lostar_NodeAdvert& rec, lostar::NodeId self_id, char* body,
                        size_t body_cap, uint16_t* out_len, lostar::NodeId* node_id_out);

/** One-line CLI history row, protocol-aware. */
void format_history_row(const lostar_NodeAdvert& rec, uint32_t posted_ms, uint32_t now_ms,
                        lomessage::Buffer& out);

}  // namespace lotato

/** C-linkage wrapper used by the CLI's `/lotato ingest` handler. */
extern "C" void lotato_format_history_row(const lostar_NodeAdvert& rec, uint32_t posted_ms,
                                          uint32_t now_ms, lomessage::Buffer& out);

#endif  // ESP32
