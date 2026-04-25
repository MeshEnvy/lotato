#include "Lotato.h"

#ifdef ESP32

#include <Arduino.h>
#include <WiFi.h>
#include <esp_log.h>

#include <LotatoCli.h>
#include <LotatoConfig.h>
#include <LotatoIngestHistory.h>
#include <LotatoIngestor.h>
#include <loabout/LoAbout.h>
#include <lofi/Lofi.h>
#include <lolog/LoLog.h>
#include <lostar/AdvertObserver.h>
#include <lostar/Busy.h>
#include <lostar/Host.h>
#include <lostar/LoStar.h>
#include <lostar/NodeId.h>
#include <lostar/Router.h>

namespace lotato {

namespace {

#ifndef LOTATO_VERSION_STRING
#define LOTATO_VERSION_STRING "0.2.0-rc.1"
#endif

bool            g_inited = false;
lostar_protocol g_host_protocol = LOSTAR_PROTOCOL_UNKNOWN;

/** Advert observer adapter: hand the incoming POD to the Lotato ingestor. */
void on_advert(const lostar_NodeAdvert *adv, void * /*ctx*/) {
  if (!adv) return;
  LotatoCli::instance().ingestor().onAdvert(*adv, LotatoCli::instance().ingestHistory(),
                                             lostar_self_nodenum());
}

/** Busy predicate: keep the host awake while there's ingest work queued or WiFi is configured. */
bool busy_hint(void * /*ctx*/) {
  if (LotatoConfig::instance().ssid()[0] != '\0') return true;
  if (LotatoCli::instance().ingestor().pendingQueueDepth() > 0) return true;
  return false;
}

/** Tick hook: keep the ingestor worker alive and caching self-id. */
void tick(void * /*ctx*/) {
  LotatoCli::instance().ingestor().serviceTick(lostar_self_nodenum());
}

void append_lotato_about_banner(lomessage::Buffer& out, void * /*user*/) {
  out.append("Lotato v" LOTATO_VERSION_STRING "\n");
  out.append("~~the Potato Mesh ingestor~~\n");
}

}  // namespace

lostar_protocol host_protocol() { return g_host_protocol; }

void init(lostar_protocol host_proto, lofs::FSys* internal_fs) {
  if (g_inited) return;
  g_inited        = true;
  g_host_protocol = host_proto;

  // Silence ESP-IDF VFS open-failure spam; shared host-platform noise, not mesh-specific.
  esp_log_level_set("vfs_api", ESP_LOG_NONE);

  if (internal_fs) {
    LoStar::boot({{"/__int__", internal_fs}});
  } else {
    LoStar::boot({});
  }

  LotatoConfig::instance().load();
  LotatoCli::instance().ingestHistory().begin();
  LotatoCli::instance().defaultRegister();

  lotato_register_sta_dns_override();

  lostar_register_advert_observer(&on_advert, nullptr);
  lostar_register_busy_hint(&busy_hint, nullptr);
  lostar_register_tick_hook(&tick, nullptr);

  loabout::set_banner_fn(&append_lotato_about_banner, nullptr);
  loabout::init();
  lostar::router().setHelpBanner(&loabout::append_banner);
}

}  // namespace lotato

#endif  // ESP32
