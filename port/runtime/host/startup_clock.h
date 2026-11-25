// Startup timing and the log's first line. Host-only text: nothing here reaches game state.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include "product_identity.h"

namespace host::startup {
// " (+1234 ms)": milliseconds since this process was created, appended after a startup line's
inline std::string suffix(uint64_t ms) { return " (+" + std::to_string(ms) + " ms)"; }

// Crash reports read the version and executable from this line: "<product> <version>, <exe>".
inline std::string log_header(const char* version, const char* exe) {
  std::string out = std::string(product::kName) + " " + (version ? version : "") + ", " + (exe ? exe : "");
  return out;
}
}  // namespace host::startup
