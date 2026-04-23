#pragma once

/**
 * Build-time selection of ingest wire constants (protocol name, key sizes, record shape, JSON
 * payload builder). Core Lotato sources include only this header — not MeshCore / Meshtastic
 * headers directly.
 */
#if defined(LOTATO_PLATFORM_MESHCORE)
#include "platforms/meshcore/IngestPlatform.h"
#elif defined(LOTATO_PLATFORM_MESHTASTIC)
#include "platforms/meshtastic/IngestPlatform.h"
#else
#error "Lotato: define LOTATO_PLATFORM_MESHCORE=1 or LOTATO_PLATFORM_MESHTASTIC=1 for ingest."
#endif
