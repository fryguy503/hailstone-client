#include "core/build_info.h"

#include "build_info.inc"

namespace hailstone {

const char* build_version() { return HAILSTONE_BUILD_VERSION; }
const char* display_version() { return HAILSTONE_DISPLAY_VERSION; }
const char* release_version() { return HAILSTONE_RELEASE_VERSION; }
int build_date() { return HAILSTONE_BUILD_DATE; }

} // namespace hailstone
