#pragma once

#ifdef ESP32

#include <cstddef>
#include <cstdint>

#include <locommand/Engine.h>
#include <locommand/Router.h>
#include <lomessage/Buffer.h>
#include <lostar/NodeId.h>

#include <LotatoIngestor.h>
#include <LotatoIngestHistory.h>

/**
 * Shared Lotato CLI tree.
 *
 * Hoists the `lotato` / `wifi` / `config` engines + handlers out of the per-platform delegate
 * so meshcore and meshtastic share one canonical definition. Each delegate is responsible for:
 *   - Converting its native caller identity into a `lostar::NodeRef` at ingress.
 *   - Handing the command line (after any platform-specific prefix stripping / alias rewriting)
 *     to `LotatoCli::dispatchLine(...)` along with that NodeRef.
 *   - Taking the produced reply buffer and transmitting it over its own mesh transport.
 *   - Optionally attaching guards to shared commands (meshtastic uses `louser::require_admin`).
 */

namespace lotato {

class LotatoCli {
public:
  static LotatoCli& instance();

  /**
   * Register the default Lotato CLI tree: lotato / wifi / config engines, their handlers, and
   * binds `ConfigHub::bindConfigCli`. Idempotent.
   */
  void defaultRegister();

  /** Add an extra engine (e.g. `user` from louser) into the shared router. */
  void registerExtraEngine(locommand::Engine* eng);

  /**
   * Dispatch one command line as @p caller. Sets `app_ctx` on the locommand context so guards
   * and handlers can recover the `lostar::NodeRef*`. `@p sender_ts` is forwarded for platforms
   * that use it for reply timestamp uniqueness; ignored otherwise.
   *
   * @return true if the router matched an engine.
   */
  bool dispatchLine(const lostar::NodeRef& caller, uint32_t sender_ts, const char* command,
                    lomessage::Buffer& out);

  /** Accessors for handlers + delegates. */
  locommand::Router&   router()        { return _router; }
  locommand::Engine&   lotatoEngine()  { return _eng_lotato; }
  locommand::Engine&   wifiEngine()    { return _eng_wifi; }
  locommand::Engine&   configEngine() { return _eng_config; }
  LotatoIngestor&      ingestor()      { return _ingestor; }
  LotatoIngestHistory& ingestHistory() { return _history; }

  /** Set/read the CLI-busy flag that wifi async callbacks toggle. */
  void setCliBusy(bool v) { _async_busy = v; }
  bool cliBusy() const    { return _async_busy; }

private:
  LotatoCli();
  void registerLotatoEngine();
  void registerWifiEngine();

  locommand::Engine _eng_lotato{"lotato"};
  locommand::Engine _eng_wifi{"wifi"};
  locommand::Engine _eng_config{"config"};
  locommand::Router _router;
  LotatoIngestor _ingestor;
  LotatoIngestHistory _history;
  bool _registered_defaults = false;
  bool _async_busy = false;
};

/**
 * Poll the LoFi WiFi scan state machine from the host main loop. Internally throttled so we do not
 * call into WiFi on every mesh iteration (which can starve loopTask and trip the ESP32 task WDT
 * when NimBLE is up).
 */
void tickLofiWifiScanIfDue();

}  // namespace lotato

#endif  // ESP32
