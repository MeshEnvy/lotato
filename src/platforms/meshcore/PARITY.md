<!-- Copyright © 2025-26 l5yth & contributors -->
<!-- Licensed under the Apache License, Version 2.0 (see LICENSE) -->

# Meshcore Platform Parity vs `data/mesh_ingestor` (Meshtastic baseline)

This document compares `src/platforms/meshcore/` (plus the C++ ingest path it drives in `src/LotatoIngestor.*`) against the Python Meshtastic ingestor in `potato-mesh/data/mesh_ingestor/`, which is currently the most feature-complete ingestion implementation.

## Scope and legend

- Baseline: Meshtastic path in `data/mesh_ingestor` (`daemon.py`, `handlers/*`, `interfaces.py`, `queue.py`, `ingestors.py`).
- Meshcore target: `src/platforms/meshcore/*` and its runtime dependency `src/LotatoIngestor.*`.
- Status:
  - `COMPLETE`: comparable capability exists in meshcore platform path.
  - `PARTIAL`: capability exists but narrower, less structured, or missing metadata.
  - `MISSING`: no equivalent capability exists today.

## Capability parity matrix

Feasibility scale used below:
- `HIGH`: can be added with localized changes, low architectural risk.
- `MEDIUM`: requires new data model/pipeline pieces across multiple files.
- `LOW`: significant runtime or architecture work needed.

| Capability (Meshtastic baseline) | Meshcore platform status | Feasibility | Notes | Short implementation proposal |
|---|---|---|---|---|
| Ingest heartbeat (`POST /api/ingestors`) with protocol/version/liveness | COMPLETE | HIGH | Implemented in `LotatoIngestor.cpp` (`maybe_post_ingestor_heartbeat`) with `protocol="meshcore"` and version support. | Keep as-is; optionally add radio metadata fields when available. |
| Node upsert pipeline (`POST /api/nodes`) | PARTIAL | HIGH | Meshcore posts node records from adverts with `num`, `lastHeard`, `user.longName`, `user.shortName`, `user.publicKey`, optional `user.role`, optional `position`. Good core parity, but far less normalized/typed than Python handler stack. | Add a small node payload builder struct and normalize optional fields before JSON assembly. |
| Role mapping (`ADV_TYPE_* -> user.role`) | COMPLETE | HIGH | Implemented in `IngestPlatform.h` via `role_for_advert_type` (COMPANION/REPEATER/ROOM_SERVER/SENSOR). | Keep mapping table source-of-truth in platform adapter and extend only if MeshCore adds new advert types. |
| Dedup + batching before POST | PARTIAL | HIGH | C++ batches and dedups by `pub_key`, with retry/backoff and queue worker. Python has richer endpoint-priority queueing and finer payload classes. | Reuse current queue worker; add endpoint-specific priority lanes instead of one shared node batch lane. |
| Multi-endpoint event fanout (`/api/messages`, `/api/positions`, `/api/telemetry`, `/api/neighbors`, `/api/traces`) | MISSING | MEDIUM | Meshcore C++ path currently posts only `/api/nodes` and `/api/ingestors`. | Introduce endpoint-specific enqueue APIs in `LotatoIngestor` and dispatch by event type. |
| Message ingestion (text/encrypted/reaction/routing) | MISSING | MEDIUM | No equivalent to `handlers/generic.py::store_packet_dict` message path. | Add mesh packet hooks for text/direct packets, then build `/api/messages` payloads with deterministic meshcore IDs. |
| Dedicated position event ingestion (`POST /api/positions`) | MISSING | HIGH | Position is embedded inside node upsert only; no separate position stream/id model. | Emit a position event when advert lat/lon changes and assign a stable `(node_id,time)`-based ID. |
| Telemetry ingestion (`POST /api/telemetry`) | MISSING | MEDIUM | No telemetry extraction/forwarding path. | Add telemetry packet parser + minimal metric field map first (battery/voltage/uptime), then extend. |
| Neighbor topology ingestion (`POST /api/neighbors`) | MISSING | MEDIUM | No neighbor snapshot ingestion. | Add neighbor packet decode path and periodic neighbor snapshot POST queueing. |
| Traceroute/path ingestion (`POST /api/traces`) | MISSING | HIGH | No trace/path payload emission today. | Start with path length + hop list from routing context (`out_path`, `path_hash_size`) and post trace-compatible payloads for animation. |
| Channel metadata capture/filtering | MISSING | MEDIUM | Meshtastic Python captures names, allows hidden/allowed filters; meshcore C++ path has no channel metadata policy path. | Cache channel index/name map from meshcore APIs, then gate posting by allow/hidden settings. |
| Radio metadata enrichment (`lora_freq`, `modem_preset`) | MISSING | HIGH | Python enriches all payloads via shared helpers; meshcore C++ heartbeat/nodes do not attach radio metadata today. | Add metadata capture once at startup/self-info and inject into every payload builder. |
| Connection strategy (auto discovery, BLE/TCP/serial parsing, reconnect/inactivity/energy-save policy) | PARTIAL | LOW | Rich policy exists in Python daemon; meshcore C++ assumes host firmware runtime and focuses on ingest worker + WiFi readiness checks. | Keep host-owned connectivity model; only add ingest-side health backoff metrics and better reconnect observability. |
| Unknown packet capture / debug forensic files | MISSING | HIGH | No equivalent to Python ignored packet logging (`ignored*.txt`) in meshcore platform path. | Add debug-only ring-buffer/file sink for ignored packets with reason tags and truncation guard. |
| Structured contract typing (`events.py`/`CONTRACTS.md`) | MISSING | MEDIUM | C++ path builds JSON directly from strings; no typed/event-contract layer. | Create lightweight C++ endpoint payload structs + serializers aligned to `CONTRACTS.md`. |

## What is already strong on meshcore platform

- Tight integration with MeshCore runtime hooks (advert receive and admin CLI interception).
- Good low-footprint batching strategy for ESP32 (`LOTATO_INGEST_BODY_CAP`, slot dedup, retry backoff).
- Ingest heartbeat sequencing and protocol tagging are aligned with web expectations.
- MeshCore role mapping is first-class and already reflected in node payloads.

## Platform-specific gaps and likely causes

- Embedded constraints: design is optimized for memory-safe node batching, not full multi-endpoint event streaming.
- Integration point is advert-centric: `onAdvertRecv` currently feeds node records only; richer event classes are not modeled in this path.
- Path/routing data is available in runtime hooks (`packet->path_len`, CLI `out_path`/`path_hash_size`) but not forwarded to ingest contracts.
- No reusable typed event layer in firmware yet, so adding new endpoints currently means manual JSON surface expansion.

## Enhancement opportunities for potato mesh core

1. Add trace/path event emission from existing meshcore routing context
   - Use `packet->path_len` (already captured) and available route context (`out_path`, `path_hash_size`) to emit path observability payloads.
   - Target `POST /api/traces` compatibility (or a meshcore-specific path event endpoint first, then normalize).
   - This is the most direct path to route animations.

2. Split node vs position streams
   - Keep node identity/profile in `/api/nodes`.
   - Emit separate `/api/positions` events when advert coordinates change, enabling position history, replay, and cleaner dedup behavior.

3. Add message ingest parity incrementally
   - Start with text/direct/channel message events into `/api/messages`.
   - Follow the deterministic ID approach already documented in `data/mesh_ingestor/CONTRACTS.md` for meshcore dedup.

4. Add radio/channel metadata enrichment
   - Include `lora_freq` and `modem_preset` on heartbeat and node/events where available.
   - Introduce optional channel-name map capture (or at least channel index propagation) to support filtering and UX parity.

5. Introduce a small typed payload builder layer in firmware
   - A minimal internal struct-to-JSON adapter (per endpoint) would reduce string-assembly risk and make parity features easier to add safely.

## Route animation note (your example)

Meshcore is well-positioned for path-driven animation because the platform hook already sees route-related context. The missing piece is persistence/export of that context into ingest events. Once path samples are emitted (ideally trace-compatible), the frontend can animate hop progression, flood vs direct behavior, and per-route quality overlays.
