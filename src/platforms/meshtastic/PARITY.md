<!-- Copyright © 2025-26 l5yth & contributors -->
<!-- Licensed under the Apache License, Version 2.0 (see LICENSE) -->

# Meshtastic Platform Parity vs `data/mesh_ingestor` (Python baseline)

This document tracks parity for `src/platforms/meshtastic/` against the current reference implementation in `potato-mesh/data/mesh_ingestor` (Meshtastic provider + handlers).

## Scope and legend

- Baseline: `data/mesh_ingestor` (`daemon.py`, `protocols/meshtastic.py`, `handlers/*`, `queue.py`, `channels.py`, `ingestors.py`).
- Target: `src/platforms/meshtastic/*` plus shared C++ ingest runtime integration.
- Status:
  - `COMPLETE`: parity capability exists and is wired.
  - `PARTIAL`: some implementation exists but key behavior/metadata is missing.
  - `MISSING`: no implementation exists yet.
- Feasibility:
  - `HIGH`: localized and straightforward to add.
  - `MEDIUM`: multi-file data-path work.
  - `LOW`: broader architectural/runtime change.

## Capability parity matrix

| Capability (Python Meshtastic baseline) | Meshtastic platform status | Feasibility | Notes | Short implementation proposal |
|---|---|---|---|---|
| Platform adapter files under `src/platforms/meshtastic` | MISSING | HIGH | Directory currently has no adapter files. | Add `IngestPlatform.h` + gateway `.h/.cpp` and wire through `Lotato.h` surface like meshcore does. |
| Ingest heartbeat (`POST /api/ingestors`) with protocol/version/liveness | MISSING | HIGH | Python sends periodic heartbeat in `ingestors.py`. | Reuse shared `LotatoIngestor` heartbeat pattern and set protocol name to `meshtastic`. |
| Node upsert (`POST /api/nodes`) from nodeinfo/user snapshots | MISSING | HIGH | No meshtastic platform hook currently emits node payloads. | Add nodeinfo ingest hook and map to canonical node payload fields first (`num`, `lastHeard`, `user`, optional `position`). |
| Message ingestion (`POST /api/messages`) text/encrypted/reaction/routing | MISSING | MEDIUM | Python route is comprehensive in `handlers/generic.py`. | Implement in slices: text + encrypted first, then reaction (`reply_id`/`emoji`), then routing payload handling. |
| Position ingestion (`POST /api/positions`) | MISSING | HIGH | Python supports both float and `latitudeI`/`longitudeI` forms. | Add dedicated position event builder and emit separate position stream (not only embedded in nodes). |
| Telemetry ingestion (`POST /api/telemetry`) | MISSING | MEDIUM | Python handles multiple telemetry subtypes and fields. | Start with device metrics (battery/voltage/channel util/uptime), then extend to environment/power/air-quality keys. |
| Neighbor topology ingestion (`POST /api/neighbors`) | MISSING | MEDIUM | Python serializes neighbor snapshots in `handlers/neighborinfo.py`. | Parse neighbor info app packets and enqueue snapshot payloads with canonical node IDs. |
| Traceroute ingestion (`POST /api/traces`) | MISSING | MEDIUM | Python normalizes hop representations and identifiers. | Add traceroute parser path with minimal contract fields (`id/request_id`, `src`, `dest`, `hops`, `rx_time`). |
| Channel metadata capture + hidden/allowed filtering | MISSING | MEDIUM | Python caches channel names and applies policy before enqueue. | Add channel index/name cache and allow/hidden checks before message ingest POST. |
| Radio metadata enrichment (`lora_freq`, `modem_preset`) | MISSING | HIGH | Python adds radio metadata to all endpoint payloads. | Capture radio settings once and inject metadata in every payload builder. |
| Queue semantics (endpoint priority + retry behavior) | PARTIAL | MEDIUM | Shared C++ queueing exists but today is node-focused. | Generalize queue to endpoint classes with explicit priorities mirroring Python queue intent. |
| Host self-node refresh + suppression windows | MISSING | MEDIUM | Python throttles noisy self telemetry/nodeinfo updates. | Add host-self reporting timer and suppression windows for repeated self updates. |
| Connection lifecycle observability (retry/inactivity/energy policy parity) | MISSING | LOW | Python daemon owns connection policy; platform C++ typically defers to firmware runtime. | Keep transport ownership in firmware; add ingest health counters/logging rather than duplicating full daemon policy. |
| Ignored/unsupported packet debug capture | MISSING | HIGH | Python has debug ignored-packet recording. | Add debug-only ignored packet sink with reason codes and bounded storage. |
| Contract-typed payload layer | MISSING | MEDIUM | Python has typed event contracts (`events.py`, `CONTRACTS.md`). | Introduce small C++ payload structs + serializers aligned to contract keys per endpoint. |

## Suggested milestone order

1. Bootstrap adapter + heartbeat + node upsert.
2. Add `/api/messages` and `/api/positions` for core UX parity.
3. Add telemetry, neighbors, and traces.
4. Add channel/radio metadata policy, then tighten debug and typed payload structures.

## Tracking guidance

- Update each row from `MISSING` -> `PARTIAL` -> `COMPLETE` as merges land.
- Prefer end-to-end endpoint slices over broad scaffolding-only changes.
