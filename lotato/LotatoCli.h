#pragma once

#ifdef ESP32

#include <locommand/Engine.h>

#include <LotatoIngestor.h>
#include <LotatoIngestHistory.h>

/**
 * Lotato's CLI state: the `lotato` + `config` engines plus the ingestor + history the handlers
 * read/write. Engines are registered with `lostar::router()` from `lotato::init()`; the router
 * itself lives in lostar. The `wifi` engine migrated to `lofi` and the `user` engine to
 * `louser`, so this header only knows about lotato-owned state now.
 */

namespace lotato {

class LotatoCli {
public:
  static LotatoCli& instance();

  /** Register the lotato + config engines with `lostar::router()`. Idempotent. */
  void defaultRegister();

  /** Accessors for handlers + observers. */
  locommand::Engine&   lotatoEngine()  { return _eng_lotato; }
  locommand::Engine&   configEngine()  { return _eng_config; }
  LotatoIngestor&      ingestor()      { return _ingestor; }
  LotatoIngestHistory& ingestHistory() { return _history; }

private:
  LotatoCli();
  void registerLotatoEngine();

  locommand::Engine   _eng_lotato{"lotato"};
  locommand::Engine   _eng_config{"config"};
  LotatoIngestor      _ingestor;
  LotatoIngestHistory _history;
  bool                _registered_defaults = false;
};

}  // namespace lotato

#endif  // ESP32
