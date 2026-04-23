# lotato

Lotato is the firmware-only [Potato Mesh](https://github.com/l5yth/potato-mesh) ingestor for MeshCore*.

> * Meshtastic coming soon!

It runs directly on a WiFi-capable device and posts mesh data to a Potato Mesh instance without a sidecar.

## Quick start

**MeshCore**

1. Flash the latest Lotato firmware: <https://meshforge.org/MeshEnvy/MeshCore-lotato>
2. Run initial repeater setup at <https://config.meshcore.dev> and set an admin password.
3. From remote admin CLI, configure WiFi and ingest:

```text
wifi scan
wifi connect <n|ssid> [pwd]
lotato endpoint <https://your-potato-mesh-instance>
lotato auth <api-token>
lotato status
```

4. Validate settings (shortcut + config alias):

```text
lotato status
```

## Full Command Reference

Three CLI roots are dispatched first (`lotato`, `wifi`, `config`); bare `help` / `?` lists them. Legacy MeshCore CLI still handles other lines.

| Command | Description |
|---|---|
| `lotato` | Show `lotato` subcommand help (same as `lotato help`) |
| `lotato status` | WiFi, IP, node count, **Due** (visible + refresh-due), paused, last HTTP code, URL/token state, debug |
| `lotato pause` | Pause ingest (shortcut for `config set lotato.ingest.paused on`) |
| `lotato resume` | Resume ingest (shortcut for `config set lotato.ingest.paused off`) |
| `lotato ingest [n]` | Show recent ingest POST attempts (newest first) |
| `lotato endpoint <url>` | Set ingest URL (same as `config set lotato.ingest.url <url>`) |
| `lotato auth <token>` | Set API token (same as `config set lotato.ingest.token <token>`) |
| `wifi status` | Current WiFi / saved SSID snapshot |
| `wifi scan` | Scan for nearby APs (async — full list when scan completes) |
| `wifi connect <n\|ssid> [pwd]` | Connect by scan index (1-based) or raw SSID |
| `wifi forget <ssid>` | Remove an SSID from the known list |
| `config ls` | List registered keys with effective values (secrets redacted) |
| `config get <ns.key>` | Print one value (e.g. `lotato.ingest.url`) |
| `config set <ns.key> <value>` | Set a value (validators + ranges apply) |
| `config unset <ns.key>` | Remove saved value (revert to default) |
| `help` / `?` | Router lists each root (name + brief); use `help <root>` or `<root> help` for that root’s commands |
| `help lotato` | Same as `lotato help` |
| `lotato help` | Flat `lotato` command list |

## Changelog (Lotato)

Lotato releases use annotated git tags of the form `lotato-v<lotato>-repeater-v<meshcore>`, for example `lotato-v0.1.0-repeater-v1.14.1`, where the `repeater-v…` suffix is the upstream [MeshCore](https://github.com/ripplebiz/MeshCore) repeater release that revision was based on.

### Unreleased (`lotato` branch, not yet tagged)

- _No changes yet._

### [0.2.0-rc.1] — 2026-04-22 (`lotato-v0.2.0-rc.1-repeater-v1.14.1`)

- **Composable CLI:** `locommand::Router` with roots `lotato`, `wifi`, and `config`. Endpoint/token setup is via `lotato endpoint` and `lotato auth` (plus `config` aliases), and WiFi commands are on the `wifi` root. `locommand::ArgSpec` improves leaf help.
- **ConfigHub / `config` CLI:** typed `lotato.*` and `lofi.*` keys in LoSettings with `config ls|get|set|unset`.
- **Ingest:** visibility (`lotato.ingest.visibility_secs`) and GC (`ingest.gc_stale_secs`) controls; LoDB `ingest_ttl` persists last-post unix per node; `lotato status` shows **Due**.
- Rename MeshForge-facing naming and unify **Lotato** branding in CLI, configuration, and source (follow-up to the Potato Mesh ingestor naming used in earlier tags).
- **Debug logging:** no compile-time `LOTATO_DEBUG`; use `config set lolog.verbose on|off` (LoSettings, via the new `lolog` + `loserial` libraries that back all lo* logging across ESP32 and nRF52). `lotato status` shows `Debug: on|off` from `LoLog::isVerbose()`.
- **CLI:** bare `lotato` prints the same help as `lotato help` (use `lotato status` for the WiFi / ingest snapshot).
- **`locommand` + `lomessage`:** `Engine` / `Router` / `ArgSpec`; `Buffer` + `Queue` for chunked replies. **Breaking:** WiFi commands are `wifi …` (not `lotato wifi …`); scan is `wifi scan`.
- **Long Lotato replies:** oversized replies go through the mesh FIFO or drip on USB serial; `wifi scan` returns one full list.

### [0.1.2] — 2026-04-11 (`lotato-v0.1.2-repeater-v1.14.1`)

- Documentation refresh for Lotato usage and setup.

### [0.1.1] — 2026-04-11 (`lotato-v0.1.1-repeater-v1.14.1`)

- MeshForge / flasher-oriented project configuration updates.
- **Lotato** branding (project and user-facing naming).

### [0.1.0] — 2026-04-09 (`lotato-v0.1.0-repeater-v1.14.1`)

First tagged Lotato release, based on MeshCore **repeater v1.14.1**.

- Initial Potato Mesh / MeshEnvy ingestor firmware path (WiFi repeater ingest to a remote HTTP endpoint).
- Batch posting fixes for the ingest pipeline.
- Fix MeshCore platform reporting for this build.
- ESP32 CLI improvements: chunked serial replies to reduce blocking, larger reply buffer, WiFi failover with rotation across known networks, and related serial output handling.
- Asynchronous handling for certain CLI command responses.
- HTTPS / TLS certificate handling fixes for outbound ingest.
