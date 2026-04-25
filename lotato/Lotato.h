#pragma once

#include <lofs/FsVolume.h>
#include <lostar/Types.h>

/**
 * Public Lotato entry point. Lotato is a plain lo-star consumer: a fork adapter installs
 * `lostar_host_ops` first, then calls `lotato::init()` to bring up the Potato Mesh ingest app
 * on top of lostar. No host-native types appear in this header.
 */

namespace lotato {

/**
 * One-shot bringup. Safe to call multiple times (idempotent). Performs:
 *   - LoStar boot (VFS, config hub; optionally binds @p internal_vol under `/__int__`)
 *   - Loads LotatoConfig, opens ingest history
 *   - Registers `about` (via `loabout`, Lotato banner) plus the `lotato` and `config` CLI engines
 *   - Subscribes the ingestor to `lostar_ingress_node_advert`
 *   - Registers a busy hinter covering ingest queue depth + configured WiFi
 *   - Registers a tick hook that services the ingestor each `lostar_tick()`
 *
 * @param host_protocol the protocol this firmware image speaks on-air. Used by the ingestor's
 *   heartbeat to stamp the correct `protocol` field.
 * @param internal_vol optional host-provided `lofs::FsVolume` for `/__int__`. Pass nullptr to
 *   let LoFS use its platform default.
 */
void init(lostar_protocol host_protocol, lofs::FsVolume* internal_vol = nullptr);

/** Protocol this lotato instance was initialized with. */
lostar_protocol host_protocol();

}  // namespace lotato
