#include "LotatoCore.h"

#ifdef ESP32

#include <Arduino.h>
#include <WiFi.h>
#include <esp_log.h>

#include <LotatoCli.h>
#include <LotatoConfig.h>
#include <LotatoIngestHistory.h>
#include <LotatoIngestor.h>
#include <lofi/Lofi.h>
#include <lostar/LoStar.h>

namespace lotato {
namespace core {

namespace {

bool g_brought_up = false;
bool g_network_started = false;

}  // namespace

void bringup(lofs::FSys* internal_fs) {
  if (g_brought_up) return;
  g_brought_up = true;

  // Silence ESP-IDF VFS open-failure spam; shared host-platform noise, not mesh-specific.
  esp_log_level_set("vfs_api", ESP_LOG_NONE);

  LoStar::boot({{"/__int__", internal_fs}});

  LotatoConfig::instance().load();
  LotatoCli::instance().ingestHistory().begin();
  LotatoCli::instance().defaultRegister();
}

void startNetwork() {
  if (g_network_started) return;
  g_network_started = true;

  lofi::Lofi::instance().begin();

  // STA policy: public DNS override + disable Arduino's autoreconnect when we have multiple
  // known SSIDs (Lofi picks instead). Both are Arduino/ESP32 WiFi concerns — not protocol APIs.
  lotato_register_sta_dns_override();
  if (lofi::Lofi::instance().knownWifiCount() >= 2) {
    WiFi.setAutoReconnect(false);
  }
}

bool networkStarted() { return g_network_started; }

void service(lostar::NodeId self_id) {
  if (!g_network_started) return;
  tickLofiWifiScanIfDue();
  LotatoCli::instance().ingestor().serviceTick(self_id);
}

bool hasPendingWork() {
  if (LotatoConfig::instance().ssid()[0] != '\0') return true;
  return LotatoCli::instance().ingestor().pendingQueueDepth() > 0;
}

}  // namespace core
}  // namespace lotato

/** Shared Lofi busy hook — toggles the CLI busy flag for async replies across every platform. */
extern "C" void lofi_async_busy(bool busy) {
  lotato::LotatoCli::instance().setCliBusy(busy);
}

#endif  // ESP32
