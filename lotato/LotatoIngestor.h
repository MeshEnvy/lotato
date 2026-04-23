#pragma once

#ifdef ESP32

#include <Arduino.h>

#include <lostar/NodeId.h>

class LotatoIngestHistory;
struct LotatoNodeRecord;

/** Apply lotato STA policy (public DNS override if LOTATO_STA_FORCE_PUBLIC_DNS=1). */
void lotato_register_sta_dns_override();
/** Proxy to `lofi::Lofi::staFailoverSuppress` (scan vs connect gate). */
void lotato_sta_failover_suppress(bool suppress);
/** Drop pending ingest batch after LoSettings-backed ingest config changes. */
void lotato_ingest_restart_after_config();

class LotatoIngestor {
public:
  /**
   * Push entry point: called from the advert hook. Throttles via @p hist, merges into the
   * pending batch (deduped by per-platform record key), and notifies the worker. The caller
   * supplies its own canonical ingestor NodeId so the worker can stamp the `ingestor` field
   * in both heartbeat and per-node payloads.
   */
  void onAdvert(const LotatoNodeRecord& rec, LotatoIngestHistory& hist, lostar::NodeId self_id);
  /** Periodic tick that keeps the worker alive and caches @p self_id for the heartbeat. */
  void serviceTick(lostar::NodeId self_id);
  /** Nodes in the current pending batch (0 if none). */
  uint8_t pendingQueueDepth() const;
  /** Drop pending batch and reset retry timer after URL/token/WiFi settings change. */
  void restartAfterConfigChange();
  void setPaused(bool paused);
  bool isPaused() const;
  /** Last HTTP response code (0 = no attempt yet). */
  int lastHttpCode() const;
};

#endif  // ESP32
