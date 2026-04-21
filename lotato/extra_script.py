Import("env", "projenv")

# Nanopb: Lotato .pb.* and sketch TUs (lo-star flags only apply to lo-star sources).
env.Append(CPPDEFINES=[("PB_FIELD_32BIT", "1")])

# MeshCore firmware headers (MeshCore.h, etc.) live under the consuming project's src/.
env.Append(CPPPATH=["$PROJECT_DIR/src"])

# Platform-specific ingest delegates (see lotato/platforms/meshcore/) — must reach both the lotato
# library sources AND the project/example sources that #include <Lotato.h>.
for e in (env, projenv):
  e.Append(CPPDEFINES=[("LOTATO_PLATFORM_MESHCORE", "1")])
  # Lotato-branded default advert name; fork respects `#ifndef ADVERT_NAME` so projects override.
  e.Append(CPPDEFINES=[("ADVERT_NAME", env.StringifyMacro("Lotato repeater"))])
