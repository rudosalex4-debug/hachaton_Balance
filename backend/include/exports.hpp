#pragma once

#include <json/json.h>
#include <string>
#include <vector>

namespace lens {

enum class ExportFormat { json, xml, csv };

ExportFormat exportFormat(const std::string& name);
std::string exportPrefix(ExportFormat format, const std::vector<std::string>& columns);
std::string exportRow(ExportFormat format, const Json::Value& row,
                      const std::vector<std::string>& columns, bool firstRow);
std::string exportSuffix(ExportFormat format);

} // namespace lens
