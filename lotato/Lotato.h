#pragma once

/**
 * Public Lotato entry point. Consumers include only this header and call into the neutral
 * `Lotato::*` API (`init`, `delegate()`, …).
 *
 * The concrete platform delegate — MeshCore / Meshtastic today, more later — is selected by
 * the `LOTATO_PLATFORM_*` define injected by `extra_script.py`. Each delegate header provides
 * the neutral `Lotato::Delegate` typedef plus inline `Lotato::init` / `Lotato::delegate`.
 */

#if defined(LOTATO_PLATFORM_MESHCORE)
#include "platforms/meshcore/MeshcoreCliGateway.h"
#elif defined(LOTATO_PLATFORM_MESHTASTIC)
#include "platforms/meshtastic/MeshtasticDelegate.h"
#else
#error "Lotato: define LOTATO_PLATFORM_MESHCORE=1 or LOTATO_PLATFORM_MESHTASTIC=1 for ingest."
#endif
