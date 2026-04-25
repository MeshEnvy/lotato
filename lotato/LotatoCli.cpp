#include "LotatoCli.h"

#ifdef ESP32

#include <Arduino.h>
#include <WiFi.h>
#include <cstring>

#include <LotatoConfig.h>
#include <lolog/LoLog.h>
#include <lostar/Router.h>
#include <losettings/ConfigHub.h>

#include "LotatoIngestPayload.h"

namespace lotato {

namespace {

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
    lotato_format_history_row(r.rec, r.last_posted_ms, now_ms, ctx.out);
  }
}

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
  _eng_lotato.add("pause", &h_pause, nullptr, nullptr, "pause ingest");
  _eng_lotato.add("resume", &h_resume, nullptr, nullptr, "resume ingest");
  _eng_lotato.addWithArgs("ingest", &h_ingest, k_ingest_args, 1, nullptr,
                           "recent ingest POSTs (newest first)");
  _eng_lotato.addWithArgs("endpoint", &h_endpoint, k_lotato_endpoint_args, 1, nullptr,
                          "set ingest URL");
  _eng_lotato.addWithArgs("auth", &h_auth, k_lotato_auth_args, 1, nullptr,
                          "set API token");
  _eng_lotato.setRootBrief("ingest status / history / endpoint / auth");
}

void LotatoCli::defaultRegister() {
  if (_registered_defaults) return;
  _registered_defaults = true;

  losettings::ConfigHub::instance().bindConfigCli(_eng_config);
  _eng_config.setRootBrief("LoSettings keys (ls/get/set/unset)");

  registerLotatoEngine();

  auto& rt = lostar::router();
  rt.add(&_eng_lotato);
  rt.add(&_eng_config);
}

}  // namespace lotato

#endif  // ESP32
