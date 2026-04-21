#pragma once

/**
 * Build-time selection of ingest wire constants (protocol name, key sizes, advert-type → role).
 * Core Lotato sources include only this header — not MeshCore / Meshtastic headers.
 */
#if defined(LOTATO_PLATFORM_MESHCORE)
#include "platforms/meshcore/IngestPlatform.h"
#else
#error "Lotato: define LOTATO_PLATFORM_MESHCORE=1 (or another supported LOTATO_PLATFORM_*) for ingest."
#endif
