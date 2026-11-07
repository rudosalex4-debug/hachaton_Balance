#pragma once

#include <drogon/HttpResponse.h>
#include <string>

namespace lens {
class Storage;

// Call after the route has authorized the owner and checked completion.
drogon::HttpResponsePtr makeExportResponse(const Storage& storage, const std::string& run,
                                          const std::string& artifact, const std::string& format);
} // namespace lens
