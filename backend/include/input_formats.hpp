#pragma once
#include <json/json.h>
#include <string>
#include <string_view>

namespace lens {
bool supportsInputFormat(std::string_view format) noexcept;

// Convert one response into the JSON document accepted by both normalizers.
// An empty profile selects JSON passthrough, XML element mapping or CSV pointers.
std::string readInputDocument(const std::string& format, const std::string& raw,
                              const Json::Value& profile);
}
