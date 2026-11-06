#pragma once
#include <json/json.h>
#include <set>
#include <string>
namespace lens {
void validateCheckResults(const Json::Value& rows,const Json::Value& checks,const std::set<std::string>& appIds);
std::string renderReportHtml(const Json::Value& report,const Json::Value& checks,const std::string& applicationBase);
}
