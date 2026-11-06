#include "storage.hpp"
#include <fstream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <set>
#include <vector>
#include <tuple>
#include <algorithm>

namespace lens {
std::string encode(const Json::Value& v){
 // A run serializes hundreds of thousands of rows. Build the immutable codec
 // configuration once per thread; JsonCpp resets its document state on write.
 thread_local const std::unique_ptr<Json::StreamWriter> writer=[] {
  Json::StreamWriterBuilder builder;builder["indentation"]="";
  return std::unique_ptr<Json::StreamWriter>(builder.newStreamWriter());
 }();
 std::ostringstream stream;writer->write(v,&stream);return stream.str();
}
Json::Value decode(const std::string& s){
 thread_local const std::unique_ptr<Json::CharReader> reader=[] {
  Json::CharReaderBuilder builder;builder["rejectDupKeys"]=true;
  return std::unique_ptr<Json::CharReader>(builder.newCharReader());
 }();
 Json::Value value;std::string errors;
 if(!reader->parse(s.data(),s.data()+s.size(),&value,&errors))throw std::runtime_error("Invalid JSON: "+errors);
 return value;
}
std::string timestamp(){auto t=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());std::tm out{};gmtime_r(&t,&out);std::ostringstream s;s<<std::put_time(&out,"%Y-%m-%dT%H:%M:%SZ");return s.str();}
static std::string table(const std::string& kind){if(kind=="configs")return "run_configs";if(kind=="datasets"||kind=="procedures"||kind=="runs")return kind;throw HttpError(400,"Unknown resource");}
std::unique_ptr<pqxx::connection> Storage::connection()const{return std::make_unique<pqxx::connection>(connectionString_);}
void Storage::initialize(const std::string& path){std::ifstream in(path);if(!in)throw std::runtime_error("Missing database migration: "+path);std::ostringstream s;s<<in.rdbuf();auto c=connection();pqxx::work tx(*c);tx.exec(s.str());tx.exec("UPDATE runs SET data=data || '{\"status\":\"failed\",\"error\":\"Server restarted; rerun the saved configuration\"}'::jsonb WHERE data->>'status' IN ('queued','running')");tx.commit();}
Json::Value Storage::list(const std::string& kind,const std::string& owner)const{auto c=connection();pqxx::read_transaction tx(*c);Json::Value out(Json::arrayValue);for(auto r:tx.exec_params("SELECT data FROM "+table(kind)+" WHERE owner=$1 OR owner IS NULL ORDER BY COALESCE(data->>'created_at','') DESC,id",owner)){auto item=decode(r[0].as<std::string>());if(kind=="runs")item.removeMember("snapshot");out.append(item);}return out;}
Json::Value Storage::get(const std::string& kind,const std::string& id,const std::string& owner)const{auto c=connection();pqxx::read_transaction tx(*c);auto r=tx.exec_params("SELECT data,owner FROM "+table(kind)+" WHERE id=$1",id);if(r.empty())throw HttpError(404,"Object not found");if(!r[0][1].is_null()&&r[0][1].as<std::string>()!=owner)throw HttpError(403,"Object belongs to another user");return decode(r[0][0].as<std::string>());}
Json::Value Storage::getRunInternal(const std::string& id)const{auto c=connection();pqxx::read_transaction tx(*c);auto r=tx.exec_params("SELECT data FROM runs WHERE id=$1",id);if(r.empty())throw std::runtime_error("Run not found");return decode(r[0][0].as<std::string>());}
void Storage::save(const std::string& kind,const Json::Value& v,const std::string& owner){auto c=connection();pqxx::work tx(*c);tx.exec_params("INSERT INTO "+table(kind)+"(id,owner,data) VALUES($1,NULLIF($2,''),$3::jsonb) ON CONFLICT(id) DO UPDATE SET data=EXCLUDED.data WHERE "+table(kind)+".owner IS NOT DISTINCT FROM EXCLUDED.owner",v["id"].asString(),owner,encode(v));tx.commit();}
void Storage::patchRun(const std::string& id,const Json::Value& p){auto c=connection();pqxx::work tx(*c);tx.exec_params("UPDATE runs SET data=data || $2::jsonb WHERE id=$1",id,encode(p));tx.commit();}
void Storage::log(const std::string& id,const std::string& stage,const std::string& message,long done,long total,const std::string& procedure){Json::Value v;v["stage"]=stage;v["message"]=message.substr(0,16000);v["done"]=Json::Int64(done);v["total"]=Json::Int64(total);v["procedure_id"]=procedure;v["created_at"]=timestamp();auto c=connection();pqxx::work tx(*c);tx.exec_params("INSERT INTO run_logs(run_id,data) VALUES($1,$2::jsonb)",id,encode(v));tx.commit();}
Json::Value Storage::logs(const std::string& id)const{auto c=connection();pqxx::read_transaction tx(*c);Json::Value out(Json::arrayValue);for(auto r:tx.exec_params("SELECT data FROM run_logs WHERE run_id=$1 ORDER BY id",id))out.append(decode(r[0].as<std::string>()));return out;}
void Storage::append(const std::string& run,const Json::Value& normalized,const Json::Value& comparisons,const Json::Value& segments){auto c=connection();pqxx::work tx(*c);
 if(!normalized.empty()){auto stream=pqxx::stream_to::table(tx,{"normalized_values"},{"run_id","app_id","source","path","array_key","data"});for(const auto& r:normalized)stream<<std::make_tuple(run,r["APP_ID"].asString(),r["SOURCE"].asString(),r["PATH"].asString(),r["ARRAY_KEY"].asString(),encode(r));stream.complete();}
 if(!comparisons.empty()){auto stream=pqxx::stream_to::table(tx,{"comparison_values"},{"run_id","app_id","path","array_key","diff_flag","change_type","data"});for(const auto& r:comparisons)stream<<std::make_tuple(run,r["APP_ID"].asString(),r["PATH"].asString(),r["ARRAY_KEY"].asString(),r["DIFF_FLAG"].asInt(),r["CHANGE_TYPE"].asString(),encode(r));stream.complete();}
 if(!segments.empty()){auto stream=pqxx::stream_to::table(tx,{"application_segments"},{"run_id","app_id","data"});for(const auto& r:segments)stream<<std::make_tuple(run,r["APP_ID"].asString(),encode(r));stream.complete();}tx.commit();}
void Storage::facts(const std::string& run,const Json::Value& rows){
 auto c=connection();pqxx::work tx(*c);
 {
  auto stream=pqxx::stream_to::table(tx,{"aggregate_facts"},{"run_id","fact_id","data"});
  size_t index=0;
  for(const auto& row:rows){
   std::string id=std::to_string(++index);
   auto fact=row;
   fact["FACT_ID"]=id;fact["id"]=id;
   stream<<std::make_tuple(run,id,encode(fact));
  }
  stream.complete();
 }
 {
  auto stream=pqxx::stream_to::table(tx,{"aggregate_members"},{"run_id","fact_id","app_id"});
  size_t index=0;
  // validateFacts already verifies unique string APP_IDS and CNT. Stream the
  // original membership directly instead of materializing a second full copy.
  for(const auto& row:rows){const auto id=std::to_string(++index);for(const auto& app:row["APP_IDS"])stream<<std::make_tuple(run,id,app.asString());}
  stream.complete();
 }
 tx.commit();
}
void Storage::saveChecks(const std::string& run,const Json::Value& rows){
 auto c=connection();pqxx::work tx(*c);
 auto stream=pqxx::stream_to::table(tx,{"run_check_results"},{"run_id","row_id","check_id","result","confidence","data"});
 int index=0;for(const auto& original:rows){auto row=original;row["id"]=std::to_string(++index);row["check_id"]=row["CHECK_ID"];stream<<std::make_tuple(run,index,row["CHECK_ID"].asString(),row["RESULT"].asString(),row["CONFIDENCE"].asString(),encode(row));}
 stream.complete();tx.commit();
}
Json::Value Storage::checkPage(const std::string& run,const std::map<std::string,std::string>& filters,int offset,int limit)const{
 auto c=connection();pqxx::read_transaction tx(*c);std::string where=" WHERE run_id="+tx.quote(run);
 for(const auto& [key,value]:filters){if(key!="result"&&key!="confidence"&&key!="check_id")throw HttpError(400,"Unsupported check filter");if(key=="result"&&value!="PASS"&&value!="FAIL")throw HttpError(400,"Invalid RESULT filter");if(key=="confidence"&&value!="HIGH"&&value!="LOW")throw HttpError(400,"Invalid CONFIDENCE filter");where+=" AND "+key+"="+tx.quote(value);}
 Json::Value out;out["items"]=Json::Value(Json::arrayValue);out["total"]=Json::Int64(tx.exec("SELECT count(*) FROM run_check_results"+where)[0][0].as<long long>());out["offset"]=offset;out["limit"]=limit;
 for(auto row:tx.exec("SELECT data FROM run_check_results"+where+" ORDER BY row_id OFFSET "+std::to_string(offset)+" LIMIT "+std::to_string(limit)))out["items"].append(decode(row[0].as<std::string>()));return out;
}
Json::Value Storage::checkSummary(const std::string& run)const{
 auto c=connection();pqxx::read_transaction tx(*c);auto row=tx.exec_params("SELECT count(*),count(*) FILTER(WHERE result='PASS'),count(*) FILTER(WHERE result='FAIL'),count(*) FILTER(WHERE confidence='LOW'),count(DISTINCT check_id) FROM run_check_results WHERE run_id=$1",run)[0];Json::Value out;int index=0;for(auto key:{"total","pass","fail","low_confidence","checks_with_rows"})out[key]=Json::Int64(row[index++].as<long long>());return out;
}
Json::Value Storage::page(const std::string& run,const std::string& type,const std::map<std::string,std::string>& filters,int offset,int limit)const{static const std::map<std::string,std::string> tables={{"normalized","normalized_values"},{"comparison","comparison_values"},{"segments","application_segments"},{"facts","aggregate_facts"}};auto found=tables.find(type);if(found==tables.end())throw HttpError(404,"Unknown data view");auto c=connection();pqxx::read_transaction tx(*c);std::string where=" WHERE run_id="+tx.quote(run);const std::map<std::string,std::string> fields={{"app_id","APP_ID"},{"path","PATH"},{"array_key","ARRAY_KEY"},{"source","SOURCE"},{"diff_flag","DIFF_FLAG"},{"change_type","CHANGE_TYPE"}};for(const auto& [name,key]:fields){auto f=filters.find(name);if(f!=filters.end()&&!f->second.empty()){bool indexed=(name=="app_id"&&type!="facts")||((name=="path"||name=="array_key")&&(type=="normalized"||type=="comparison"))||(name=="source"&&type=="normalized")||((name=="diff_flag"||name=="change_type")&&type=="comparison");if(indexed)where+=" AND "+name+"="+tx.quote(f->second);else where+=" AND data->>"+tx.quote(key)+"="+tx.quote(f->second);}}std::string order=type=="facts"?"fact_id::bigint":type=="segments"?"app_id":type=="normalized"?"app_id,source,path,array_key":"app_id,path,array_key";Json::Value out;out["total"]=Json::Int64(tx.exec("SELECT count(*) FROM "+found->second+where)[0][0].as<long long>());out["offset"]=offset;out["limit"]=limit;out["items"]=Json::Value(Json::arrayValue);for(auto r:tx.exec("SELECT data FROM "+found->second+where+" ORDER BY "+order+" OFFSET "+std::to_string(offset)+" LIMIT "+std::to_string(limit)))out["items"].append(decode(r[0].as<std::string>()));return out;}
Json::Value Storage::factApps(const std::string& run,const std::string& fact,int offset,int limit)const{auto c=connection();pqxx::read_transaction tx(*c);if(tx.exec_params("SELECT 1 FROM aggregate_facts WHERE run_id=$1 AND fact_id=$2",run,fact).empty())throw HttpError(404,"Fact not found");Json::Value out;out["items"]=Json::Value(Json::arrayValue);out["total"]=Json::Int64(tx.exec_params("SELECT count(*) FROM aggregate_members WHERE run_id=$1 AND fact_id=$2",run,fact)[0][0].as<long long>());out["offset"]=offset;out["limit"]=limit;for(auto r:tx.exec_params("SELECT app_id FROM aggregate_members WHERE run_id=$1 AND fact_id=$2 ORDER BY app_id OFFSET $3 LIMIT $4",run,fact,offset,limit)){Json::Value a;a["APP_ID"]=r[0].as<std::string>();out["items"].append(a);}return out;}
Json::Value Storage::application(const std::string& run,const std::string& app,int offset,int limit)const{Json::Value out;auto seg=page(run,"segments",{{"app_id",app}},0,1);if(seg["items"].empty())throw HttpError(404,"Application not found");out["segment"]=seg["items"][0];auto comparisons=page(run,"comparison",{{"app_id",app}},offset,limit);out["comparison"]=comparisons["items"];out["total"]=comparisons["total"];out["offset"]=offset;out["limit"]=limit;return out;}

Json::Value Storage::query(const std::string& run,const Json::Value& request,bool applications)const {
 static const std::set<std::string> allowed={"APP_ID","PREFIX","CLASS","SUFFIX","ARRAY_KEY","KEY","PATH","VALUE_ADJUSTED","ADJUSTED_VALUE","VALUE","VALUE_TYPE","SOURCE","DIFF_FLAG","CHANGE_TYPE","PRODUCTS","PRODUCT_IDS","PARTNER_TYPE","ROUTING_METHOD","DECISION_CHANGED","DECISION_BEFORE","DECISION_AFTER","PD_MODEL_BEFORE","PD_MODEL_AFTER","CUTOFF_BEFORE","CUTOFF_AFTER","REJECT_REASON_BEFORE","REJECT_REASON_AFTER","TOP_OFFER_BEFORE","TOP_OFFER_AFTER","SCORE_OKB_BEFORE","SCORE_OKB_AFTER","CHANGED","INPUT_CHANGED","EXTERNAL_CHANGED"};
 auto dims=request.get("dimensions",Json::Value(Json::arrayValue));auto filters=request.get("filters",Json::Value(Json::objectValue));
 if(!dims.isArray()||dims.size()>16||(!applications&&dims.empty())||!filters.isObject()||filters.size()>32)throw HttpError(400,"Provide 1..16 dimensions and at most 32 filters");
 auto validate=[&](const std::string& key){if(!allowed.count(key))throw HttpError(400,"Unsupported dimension: "+key);};
 std::set<std::string> unique;for(const auto& dim:dims){if(!dim.isString())throw HttpError(400,"Dimensions must be strings");validate(dim.asString());if(!unique.insert(dim.asString()).second)throw HttpError(400,"Duplicate dimension");}for(auto& key:filters.getMemberNames()){validate(key);const auto& filter=filters[key];auto scalarValid=[&](const Json::Value& value){if(value.isObject()||value.isArray()||(value.isString()&&value.asString().size()>4096))throw HttpError(400,"Filter values must be scalars (strings up to 4096 bytes)");if(key=="SOURCE"&&value!="BEFORE"&&value!="AFTER")throw HttpError(400,"SOURCE must be BEFORE or AFTER");if((key=="DIFF_FLAG"||key=="DECISION_CHANGED"||key=="CHANGED"||key=="INPUT_CHANGED"||key=="EXTERNAL_CHANGED")&&(!value.isInt()||(value.asInt()!=0&&value.asInt()!=1)))throw HttpError(400,"Flag filters must be integer 0 or 1");};if(filter.isArray()){if(filter.empty()||filter.size()>100)throw HttpError(400,"Filter lists require 1..100 values");for(const auto& value:filter)scalarValid(value);}else scalarValid(filter);}
 auto pageNumber=[&](const char* key,int fallback,int maximum){if(!request.isMember(key))return fallback;if(!request[key].isInt()||request[key].asInt()<0)throw HttpError(400,"Invalid pagination");return std::min(maximum,request[key].asInt());};int offset=pageNumber("offset",0,100000000),limit=std::max(1,pageNumber("limit",100,1000));
 auto c=connection();pqxx::read_transaction tx(*c);
 const std::set<std::string> rowFields={"APP_ID","PREFIX","CLASS","SUFFIX","ARRAY_KEY","KEY","PATH","VALUE_ADJUSTED","ADJUSTED_VALUE","VALUE","VALUE_TYPE","SOURCE","DIFF_FLAG","CHANGE_TYPE"};
 bool needsSegments=false;for(const auto& dim:dims)needsSegments|=!rowFields.count(dim.asString());for(const auto& key:filters.getMemberNames())needsSegments|=!rowFields.count(key);
 auto expression=[&](const std::string& key)->std::string{
  if(key=="APP_ID")return "to_jsonb(c.app_id)";
  if(key=="PATH")return "to_jsonb(c.path)";
  if(key=="ARRAY_KEY")return "to_jsonb(c.array_key)";
  if(key=="DIFF_FLAG")return "to_jsonb(c.diff_flag)";
  if(key=="CHANGE_TYPE")return "to_jsonb(c.change_type)";
  if(key=="SOURCE")return "to_jsonb(side)";
  if(key=="VALUE_TYPE")return "c.data->('TYPE_'||side)";
  if(key=="VALUE_ADJUSTED"||key=="ADJUSTED_VALUE"||key=="VALUE")return "CASE WHEN (c.data->>('PRESENT_'||side))::boolean THEN c.data->('"+std::string(key=="VALUE"?"VALUE_":"ADJUSTED_")+"'||side) ELSE to_jsonb('(отсутствует)'::text) END";
  return std::string(rowFields.count(key)?"c.data->":"s.data->")+tx.quote(key);
 };
 std::string dimensions="jsonb_build_object(";bool first=true;for(const auto& dim:dims){if(!first)dimensions+=",";first=false;dimensions+=tx.quote(dim.asString())+","+expression(dim.asString());}dimensions+=")";
 std::string base="WITH filtered AS (SELECT c.app_id"+(applications?std::string():","+dimensions+" AS dimensions")+" FROM comparison_values c";
 if(needsSegments)base+=" JOIN application_segments s ON s.run_id=c.run_id AND s.app_id=c.app_id";
 base+=" CROSS JOIN (VALUES ('BEFORE'),('AFTER')) sides(side) WHERE c.run_id="+tx.quote(run);
 const std::map<std::string,std::string> indexed={{"APP_ID","c.app_id"},{"PATH","c.path"},{"ARRAY_KEY","c.array_key"},{"DIFF_FLAG","c.diff_flag"},{"CHANGE_TYPE","c.change_type"},{"SOURCE","side"}};
 for(const auto& key:filters.getMemberNames()){
  const auto& filter=filters[key];bool nativeColumn=indexed.count(key)>0;
  auto compatible=[&](const Json::Value& value){return key=="DIFF_FLAG"?value.isInt():value.isString();};
  if(filter.isArray()){for(const auto& value:filter)nativeColumn&=compatible(value);}else nativeColumn&=compatible(filter);
  std::string field=nativeColumn?indexed.at(key):expression(key);
  auto quoted=[&](const Json::Value& value){return nativeColumn?tx.quote(value.isString()?value.asString():std::to_string(value.asInt())):tx.quote(encode(value))+"::jsonb";};
  if(filter.isNull()){base+=" AND (("+field+") IS NULL OR ("+field+") = 'null'::jsonb)";continue;}
  base+=" AND ("+field+")";
  if(filter.isArray()){base+=" IN (";bool initial=true;for(const auto& value:filter){if(!initial)base+=",";initial=false;base+=quoted(value);}base+=")";}else base+=" = "+quoted(filter);
 }
 base+=")";
 if(applications)base+=", grouped AS MATERIALIZED (SELECT DISTINCT app_id FROM filtered)";
 else base+=", grouped AS MATERIALIZED (SELECT dimensions,COUNT(DISTINCT app_id) AS count FROM filtered GROUP BY dimensions)";
 const std::string relation=applications?"jsonb_build_object('APP_ID',app_id)":"dimensions || jsonb_build_object('CNT',count,'counts',count)";
 const std::string order=applications?"app_id":"dimensions::text";
 // Compute groups once for both pagination and count, including empty pages.
 auto result=tx.exec(base+" SELECT (SELECT count(*) FROM grouped),COALESCE((SELECT jsonb_agg(page.item) FROM (SELECT "+relation+" AS item FROM grouped ORDER BY "+order+" OFFSET "+std::to_string(offset)+" LIMIT "+std::to_string(limit)+") page),'[]'::jsonb)")[0];
 Json::Value out;out["total"]=Json::Int64(result[0].as<long long>());out["items"]=decode(result[1].as<std::string>());out["offset"]=offset;out["limit"]=limit;return out;
}

}
