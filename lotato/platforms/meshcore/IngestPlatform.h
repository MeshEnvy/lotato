#pragma once

#include <MeshCore.h>
#include <helpers/AdvertDataHelpers.h>
#include <cstddef>
#include <cstdint>

namespace lotato {
namespace ingest_platform {

inline constexpr size_t kPublicKeyBytes = PUB_KEY_SIZE;

inline const char* protocol_name() { return "meshcore"; }

/** Default ``version`` when ``LOTATO_INGESTOR_VERSION`` is not set at compile time. */
inline const char* default_ingestor_version() { return "meshcore-esp32"; }

/** Potato-mesh ``user.role`` string, or nullptr to omit the field. */
inline const char* role_for_advert_type(uint8_t adv_type) {
  switch (adv_type) {
    case ADV_TYPE_CHAT:
      return "COMPANION";
    case ADV_TYPE_REPEATER:
      return "REPEATER";
    case ADV_TYPE_ROOM:
      return "ROOM_SERVER";
    case ADV_TYPE_SENSOR:
      return "SENSOR";
    default:
      return nullptr;
  }
}

}  // namespace ingest_platform
}  // namespace lotato
