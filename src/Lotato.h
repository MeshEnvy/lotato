#pragma once

/**
 * Public Lotato entry point. Consumers include only this header and call into the neutral
 * `Lotato::*` API (`boot`, `init`, `service`, `onAdvertRecv`, `handleAdminTxtCli`, ...).
 *
 * The concrete platform delegate — MeshCore / Meshtastic today, more later — is selected by the
 * `LOTATO_PLATFORM_*` define injected by the lotato library's `extra_script.py`. The delegate
 * header provides the `Lotato::Host` type the fork class inherits from, plus the inline wrappers
 * backing the neutral API.
 */

#if defined(LOTATO_PLATFORM_MESHCORE)
#include "platforms/meshcore/MeshcoreCliGateway.h"
#else
#error "Lotato: define LOTATO_PLATFORM_MESHCORE=1 (or another supported LOTATO_PLATFORM_*) for ingest."
#endif
