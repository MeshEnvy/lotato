#include "LotatoCli.h"

#ifdef ESP32

#include <Arduino.h>
#include <WiFi.h>
#include <cstring>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_task_wdt.h>
#endif

#include <LotatoConfig.h>
#include <LotatoIngestPlatform.h>
#include <lofi/Lofi.h>
#include <lolog/LoLog.h>
#include <losettings/ConfigHub.h>
#include <lostar/NodeId.h>

namespace lotato {

namespace {

/** Min interval between LoFi `serviceWifiScan()` calls from the host main loop. */
constexpr uint32_t kLofiWifiScanServiceMs = 100;

void runWifiScanCli(lomessage::Buffer& out) {
  auto& lf = lofi::Lofi::instance();
  lf.serviceWifiScan();
  auto& cli = LotatoCli::instance();
  if (lf.scanSnapshotCount() > 0 && !cli.cliBusy()) {
    lf.formatScanBody(out);
    return;
  }
  if (cli.cliBusy()) {
    out.append("WiFi scan in progress...");
    return;
  }
  lf.requestWifiScan();
  out.append("Scanning for WiFi devices...");
}

/* ── lotato root handlers ─────────────────────────────────────────── */

void h_status(locommand::Context& ctx) {
  auto& cli = LotatoCli::instance();
  LotatoConfig& cfg = LotatoConfig::instance();
  wl_status_t wl = WiFi.status();
  const char* wl_str = (wl == WL_CONNECTED) ? "connected" : "not connected";
  int code = cli.ingestor().lastHttpCode();
  char code_str[12];
  if (code == 0) strcpy(code_str, "none");
  else snprintf(code_str, sizeof(code_str), "%d", code);
  const char* token_str = cfg.apiToken()[0] ? "set" : "(none)";
  const char* url_str = cfg.ingestOrigin()[0] ? cfg.ingestOrigin() : "(none)";
  const char* dbg_str = ::lolog::LoLog::isVerbose() ? "on" : "off";
  const int hist_count = cli.ingestHistory().count();
  const int hist_cap   = cli.ingestHistory().capacity();
  if (wl == WL_CONNECTED) {
    ctx.out.appendf("WiFi: %s\nSSID: %s\nIP: %s\nHistory: %d/%d\nPaused: %s\nLast API Response: %s\nURL: %s\nToken: %s\nDebug: %s",
                    wl_str, WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                    hist_count, hist_cap,
                    cli.ingestor().isPaused() ? "yes" : "no", code_str, url_str, token_str, dbg_str);
  } else {
    ctx.out.appendf("WiFi: %s\nSaved: %s\nHistory: %d/%d\nPaused: %s\nURL: %s\nToken: %s\nDebug: %s",
                    wl_str, cfg.ssid()[0] ? cfg.ssid() : "(none)",
                    hist_count, hist_cap,
                    cli.ingestor().isPaused() ? "yes" : "no", url_str, token_str, dbg_str);
  }
}

void h_pause(locommand::Context& ctx) {
  LotatoCli::instance().ingestor().setPaused(true);
  ctx.out.append("OK - ingest paused");
}

void h_resume(locommand::Context& ctx) {
  LotatoCli::instance().ingestor().setPaused(false);
  ctx.out.append("OK - ingest resumed");
}

bool join_argv_value(locommand::Context& ctx, char* valbuf, size_t val_cap) {
  valbuf[0] = '\0';
  size_t pos = 0;
  for (int i = 0; i < ctx.argc && pos + 1 < val_cap; i++) {
    if (i > 0 && pos + 1 < val_cap) valbuf[pos++] = ' ';
    const char* p = ctx.argv[i];
    size_t l = strlen(p);
    if (pos + l >= val_cap) return false;
    memcpy(valbuf + pos, p, l);
    pos += l;
    valbuf[pos] = '\0';
  }
  return true;
}

void h_endpoint(locommand::Context& ctx) {
  if (ctx.argc < 1) { ctx.printHelp(); return; }
  static char valbuf[320];
  if (!join_argv_value(ctx, valbuf, sizeof(valbuf))) {
    ctx.out.append("Err - value too long\n");
    return;
  }
  char err[128];
  if (!losettings::ConfigHub::instance().setFromString("lotato.ingest.url", valbuf, err, sizeof(err))) {
    ctx.out.appendf("Err - %s\n", err);
    return;
  }
  ctx.out.append("OK\n");
}

void h_auth(locommand::Context& ctx) {
  if (ctx.argc < 1) { ctx.printHelp(); return; }
  static char valbuf[320];
  if (!join_argv_value(ctx, valbuf, sizeof(valbuf))) {
    ctx.out.append("Err - value too long\n");
    return;
  }
  char err[128];
  if (!losettings::ConfigHub::instance().setFromString("lotato.ingest.token", valbuf, err, sizeof(err))) {
    ctx.out.appendf("Err - %s\n", err);
    return;
  }
  ctx.out.append("OK\n");
}

void h_ingest(locommand::Context& ctx) {
  size_t limit = 0;
  if (ctx.argc >= 1) {
    int n = atoi(ctx.argv[0]);
    if (n > 0) limit = (size_t)n;
  }
  auto& hist = LotatoCli::instance().ingestHistory();
  std::vector<LotatoIngestHistory::Row> rows;
  hist.snapshot(limit, rows);
  ctx.out.appendf("Ingest history: %d/%d\n", hist.count(), hist.capacity());
  const uint32_t now_ms = millis();
  for (const auto& r : rows) {
    lotato::ingest_platform::format_history_row(r.rec, r.last_posted_ms, now_ms, ctx.out);
  }
}

/* ── wifi root handlers ───────────────────────────────────────────── */

void h_wifi_status(locommand::Context& ctx) {
  LotatoConfig& cfg = LotatoConfig::instance();
  wl_status_t wl = WiFi.status();
  if (wl == WL_CONNECTED) {
    ctx.out.appendf("WiFi: connected\nSSID: %s\nIP: %s", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
  } else {
    ctx.out.appendf("WiFi: not connected\nSaved: %s\nUse: wifi scan",
                    cfg.ssid()[0] ? cfg.ssid() : "(none)");
  }
}

void h_wifi_scan(locommand::Context& ctx) { runWifiScanCli(ctx.out); }

void h_wifi_connect(locommand::Context& ctx) {
  LotatoConfig& cfg = LotatoConfig::instance();
  if (ctx.argc < 1) { ctx.printHelp(); return; }
  const char* tok1 = ctx.argv[0];
  const char* tok2 = (ctx.argc >= 2) ? ctx.argv[1] : "";

  char ssid_to_use[33] = {};
  bool is_index = true;
  for (const char* q = tok1; *q; q++) {
    if (*q < '0' || *q > '9') {
      is_index = false;
      break;
    }
  }
  auto& lf = lofi::Lofi::instance();
  if (is_index && tok1[0] != '\0') {
    int idx = atoi(tok1) - 1;
    int32_t rssi;
    if (idx < 0 || !lf.scanSnapshotEntry(idx, ssid_to_use, &rssi)) {
      ctx.out.appendf("Err - index out of range (1..%d)\nRun: wifi scan first", lf.scanSnapshotCount());
      return;
    }
  } else {
    strncpy(ssid_to_use, tok1, sizeof(ssid_to_use) - 1);
  }

  char pwd_to_use[65] = {};
  if (tok2[0] != '\0') {
    strncpy(pwd_to_use, tok2, sizeof(pwd_to_use) - 1);
    pwd_to_use[sizeof(pwd_to_use) - 1] = '\0';
  } else {
    cfg.getKnownWifiPassword(ssid_to_use, pwd_to_use, sizeof(pwd_to_use));
  }

  cfg.setWifi(ssid_to_use, pwd_to_use);
  LotatoCli::instance().ingestor().restartAfterConfigChange();
  lf.beginConnect(ssid_to_use, pwd_to_use);
  ::lolog::LoLog::debug("lotato", "lotato CLI: wifi connecting ssid=%s modem_sleep=off", ssid_to_use);
  ctx.out.appendf("Connecting to %s...", ssid_to_use);
}

void h_wifi_forget(locommand::Context& ctx) {
  LotatoConfig& cfg = LotatoConfig::instance();
  if (ctx.argc < 1) { ctx.printHelp(); return; }
  if (!cfg.forgetKnownWifi(ctx.argv[0])) {
    ctx.out.append("Err - SSID not in known list\n");
    return;
  }
  ctx.out.append("OK\n");
}

const locommand::ArgSpec k_wifi_connect_args[] = {
    {"n_or_ssid", "string", nullptr, true, "Scan index (1-based) or SSID"},
    {"password", "secret", nullptr, false, "PSK if not already saved"},
};

const locommand::ArgSpec k_wifi_forget_args[] = {
    {"ssid", "string", nullptr, true, "Network SSID to remove from known list"},
};

const locommand::ArgSpec k_ingest_args[] = {
    {"n", "uint", nullptr, false, "Max entries to show (default: all)"},
};

const locommand::ArgSpec k_lotato_endpoint_args[] = {
    {"url", "string", nullptr, true, "Ingest origin URL (same as config set lotato.ingest.url)"},
};

const locommand::ArgSpec k_lotato_auth_args[] = {
    {"token", "secret", nullptr, true, "API bearer token (same as config set lotato.ingest.token)"},
};

}  // namespace

LotatoCli& LotatoCli::instance() {
  static LotatoCli s;
  return s;
}

LotatoCli::LotatoCli() = default;

void LotatoCli::registerLotatoEngine() {
  _eng_lotato.add("status", &h_status, nullptr, nullptr, "show lotato/ingest status");
  _eng_lotato.add("pause", &h_pause, nullptr, nullptr, "pause ingest (shortcut for config)");
  _eng_lotato.add("resume", &h_resume, nullptr, nullptr, "resume ingest (shortcut for config)");
  _eng_lotato.addWithArgs("ingest", &h_ingest, k_ingest_args, 1, nullptr,
                           "recent ingest POSTs (newest first)");
  _eng_lotato.addWithArgs("endpoint", &h_endpoint, k_lotato_endpoint_args, 1, nullptr,
                          "set ingest URL (alias: lotato.ingest.url)");
  _eng_lotato.addWithArgs("auth", &h_auth, k_lotato_auth_args, 1, nullptr,
                          "set API token (alias: lotato.ingest.token)");
  _eng_lotato.setRootBrief("ingest status / history / endpoint / auth");
}

void LotatoCli::registerWifiEngine() {
  _eng_wifi.add("status", &h_wifi_status, nullptr, nullptr, "STA / saved SSID snapshot");
  _eng_wifi.add("scan", &h_wifi_scan, nullptr, nullptr, "scan for APs (async reply)");
  _eng_wifi.addWithArgs("connect", &h_wifi_connect, k_wifi_connect_args, 2, nullptr,
                        "connect by index or SSID");
  _eng_wifi.addWithArgs("forget", &h_wifi_forget, k_wifi_forget_args, 1, nullptr,
                        "remove SSID from known list");
  _eng_wifi.setRootBrief("WiFi STA scan/connect");
}

void LotatoCli::defaultRegister() {
  if (_registered_defaults) return;
  _registered_defaults = true;

  losettings::ConfigHub::instance().bindConfigCli(_eng_config);
  _eng_config.setRootBrief("LoSettings keys (ls/get/set/unset)");

  registerLotatoEngine();
  registerWifiEngine();

  _router.clear();
  _router.add(&_eng_lotato);
  _router.add(&_eng_wifi);
  _router.add(&_eng_config);
}

void LotatoCli::registerExtraEngine(locommand::Engine* eng) {
  if (!eng) return;
  _router.add(eng);
}

bool LotatoCli::dispatchLine(const lostar::NodeRef& caller, uint32_t /*sender_ts*/,
                             const char* command, lomessage::Buffer& out) {
  (void)&caller;
  void* app_ctx = const_cast<lostar::NodeRef*>(&caller);
  return _router.dispatch(command, out, app_ctx);
}

void tickLofiWifiScanIfDue() {
  static uint32_t s_last_ms = 0;
  const uint32_t now = millis();
  if ((uint32_t)(now - s_last_ms) < kLofiWifiScanServiceMs) return;
  s_last_ms = now;
#if defined(ARDUINO_ARCH_ESP32)
  esp_task_wdt_reset();
#endif
  lofi::Lofi::instance().serviceWifiScan();
#if defined(ARDUINO_ARCH_ESP32)
  esp_task_wdt_reset();
#endif
}

}  // namespace lotato

#endif  // ESP32
