Import("env")

# MeshCore firmware headers (MeshCore.h, etc.) live under the consuming project's src/.
env.Append(CPPPATH=["$PROJECT_DIR/src"])
# Platform-specific ingest delegates (see lotato/platforms/meshcore/).
env.Append(CPPDEFINES=[("LOTATO_PLATFORM_MESHCORE", "1")])
