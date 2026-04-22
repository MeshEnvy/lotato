#pragma once

#ifdef ESP32

#include <Arduino.h>

class LotatoIngestHistory;
struct LotatoNodeRecord;

/** Apply lotato STA policy (public DNS override if LOTATO_STA_FORCE_PUBLIC_DNS=1). */
void lotato_register_sta_dns_override();
/** No-op (STA event logging is now in lofi::Lofi); kept for call-site compat. */
void lotato_register_wifi_event_logging();
/** No-op (failover lives in lofi::Lofi); kept for call-site compat. */
void lotato_register_sta_known_wifi_failover();
/** Proxy to `lofi::Lofi::staFailoverSuppress` (scan vs connect gate). */
void lotato_sta_failover_suppress(bool suppress);
/** Drop pending ingest batch after LoSettings-backed ingest config changes. */
void lotato_ingest_restart_after_config();

class LotatoIngestor {
public:
  /**
   * Push entry point: called from the advert hook. Throttles via @p hist, merges into the
   * pending batch (deduping by pub_key), and notifies the worker.
   */
  void onAdvert(const LotatoNodeRecord& rec, LotatoIngestHistory& hist, const uint8_t self_pub_key[32]);
  /** Periodic tick that keeps the worker alive and caches @p self_pub_key for the heartbeat. */
  void serviceTick(const uint8_t self_pub_key[32]);
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
