#pragma once
#include <json/json.h>
#include "pqxx_compat.hpp"
#include <memory>
#include <string>
#include <map>

namespace lens {
std::string encode(const Json::Value&);
Json::Value decode(const std::string&);
std::string timestamp();
struct HttpError : std::runtime_error { int status; HttpError(int s,const std::string& m):std::runtime_error(m),status(s){} };
class Storage {
 std::string connectionString_;
 public:
 explicit Storage(std::string connectionString):connectionString_(std::move(connectionString)){}
 std::unique_ptr<pqxx::connection> connection() const;
 void initialize(const std::string& migration);
 Json::Value list(const std::string& kind,const std::string& owner) const;
 Json::Value get(const std::string& kind,const std::string& id,const std::string& owner) const;
 Json::Value getRunInternal(const std::string& id) const;
 void save(const std::string& kind,const Json::Value& value,const std::string& owner);
 void patchRun(const std::string& id,const Json::Value& patch);
 void log(const std::string& id,const std::string& stage,const std::string& message,long done=0,long total=0,const std::string& procedure="");
 Json::Value logs(const std::string& id) const;
 void append(const std::string& run,const Json::Value& normalized,const Json::Value& comparisons,const Json::Value& segments);
 void saveChecks(const std::string& run,const Json::Value& rows);
 Json::Value checkPage(const std::string& run,const std::map<std::string,std::string>& filters,int offset,int limit) const;
 Json::Value checkSummary(const std::string& run) const;
 void facts(const std::string& run,const Json::Value& values);
 Json::Value page(const std::string& run,const std::string& type,const std::map<std::string,std::string>& filters,int offset,int limit) const;
 Json::Value factApps(const std::string& run,const std::string& fact,int offset,int limit) const;
 Json::Value application(const std::string& run,const std::string& id,int offset=0,int limit=1000) const;
 Json::Value query(const std::string& run,const Json::Value& request,bool applications=false) const;
};
}

