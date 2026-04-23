Import("env", "projenv")

# Nanopb: Lotato .pb.* and sketch TUs (lo-star flags only apply to lo-star sources).
env.Append(CPPDEFINES=[("PB_FIELD_32BIT", "1")])

# MeshCore / Meshtastic firmware headers (MeshCore.h, mesh/generated/...) live under the
# consuming project's src/. The lotato library pulls them via the shared include path below.
env.Append(CPPPATH=["$PROJECT_DIR/src"])


def _has_lotato_platform(scons_env):
    """True when the consuming project/env already declares a concrete LOTATO_PLATFORM_*."""
    for define in scons_env.get("CPPDEFINES", []):
        name = define[0] if isinstance(define, (list, tuple)) else define
        if isinstance(name, str) and name.startswith("LOTATO_PLATFORM_"):
            return True
    return False


# Platform-specific ingest delegates (see lotato/platforms/*): the consuming firmware opts in by
# setting e.g. `-D LOTATO_PLATFORM_MESHCORE=1` or `-D LOTATO_PLATFORM_MESHTASTIC=1` in its
# platformio.ini build_flags. When no choice was made, default to MeshCore so historical
# consumers that never set LOTATO_PLATFORM_* keep working unchanged.
explicit_choice = _has_lotato_platform(env) or _has_lotato_platform(projenv)

for e in (env, projenv):
    if not explicit_choice:
        e.Append(CPPDEFINES=[("LOTATO_PLATFORM_MESHCORE", "1")])
    # Lotato-branded default advert name; fork respects `#ifndef ADVERT_NAME` so projects override.
    e.Append(CPPDEFINES=[("ADVERT_NAME", env.StringifyMacro("Lotato repeater"))])
