#pragma once

#ifdef ESP32

#include <cstdint>

#include <lofs/FsBackend.h>
#include <lostar/NodeId.h>

/**
 * Shared, platform-agnostic Lotato lifecycle. Anything that is hardware- or target-specific but
 * does NOT use a MeshCore/Meshtastic API lives here — not in the platform delegates.
 *
 * Platform delegates translate their native (packet/identity/port) shapes into lotato-neutral
 * calls and rely on this core for:
 *   - VFS + config + CLI + ingest bringup.
 *   - Bringing up Lofi (WiFi STA) and applying STA policy (DNS override, autoreconnect).
 *   - Per-tick service (WiFi scan pump + ingest batch worker).
 *   - Pending-work reporting used by host sleep gating.
 */

namespace lotato {
namespace core {

/**
 * One-shot bringup: VFS mount, config load, ingest history, shared CLI tree. Does NOT start the
 * network — call `startNetwork()` once the host is ready (MeshCore calls it immediately;
 * Meshtastic defers until after NimBLE::init to avoid a BT/WiFi coexistence hang).
 */
void bringup(lofs::FSys* internal_fs);

/**
 * Start Lofi (WiFi STA) and apply STA policy. Idempotent.
 */
void startNetwork();

/** True once `startNetwork()` has run. */
bool networkStarted();

/**
 * Tick shared services (throttled WiFi scan pump + ingest batch worker). No-op until
 * `startNetwork()` has been called. Delegates add their own platform-specific queue services
 * (e.g. MeshCore's reply FIFO) around this call.
 */
void service(lostar::NodeId self_id);

/**
 * True if Lotato has pending background work that should prevent the host from sleeping
 * (WiFi configured => needs radio, or ingest batch queue non-empty). Delegates OR this with
 * their own TX-queue depth checks.
 */
bool hasPendingWork();

}  // namespace core
}  // namespace lotato

#endif  // ESP32
