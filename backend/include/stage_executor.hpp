#pragma once
#include <json/json.h>
#include <functional>
#include <string>

namespace lens {
Json::Value executePython(const std::string& kind, const std::string& code,
    const Json::Value& input, const Json::Value& config,
    const std::function<bool()>& cancelled,
    const std::function<void(const std::string&)>& log);
}
