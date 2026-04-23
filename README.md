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

