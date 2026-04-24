Import("env", "projenv")

# Lotato is a plain lo-star consumer; it does not touch host-native structs, so it does not
# need to match any fork's nanopb field-size flags, and it does not reach into the fork's src/.
# The only build-time behavior it still provides is the default advert-name override.

for e in (env, projenv):
    # Lotato-branded default advert name; fork respects `#ifndef ADVERT_NAME` so projects override.
    e.Append(CPPDEFINES=[("ADVERT_NAME", env.StringifyMacro("Lotato repeater"))])
