#include "core.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <set>
#include <chrono>
#include <cstdlib>
#include <sstream>

namespace {
Json::Value parsed(const std::string& s){Json::CharReaderBuilder b;Json::Value v;std::string errors;std::istringstream stream(s);if(!Json::parseFromStream(b,stream,&v,&errors))throw std::runtime_error(errors);return v;}
Json::Value diff(const std::string& b,const std::string& a,Json::Value cfg=lens::defaultConfig()) {return lens::compare(lens::normalize(b,"app1","BEFORE",cfg),lens::normalize(a,"app1","AFTER",cfg),cfg);}
size_t changed(const Json::Value& rows){size_t n=0;for(auto& r:rows)n+=r["DIFF_FLAG"].asInt();return n;}
std::string read(const std::filesystem::path& path){std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("Cannot open "+path.string());return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};}
}
TEST(Normalize,NestedObjectsAndTechnicalExclusions){auto out=lens::normalize(R"({"appData":{"request_id":"a","createDateTime":"b"},"obj":{"inner":{"x":12}}})","app1","BEFORE",lens::defaultConfig());ASSERT_EQ(out.size(),1u);EXPECT_EQ(out[0]["PATH"],"obj.inner.x");EXPECT_EQ(out[0]["VALUE"],"12");auto cfg=lens::defaultConfig();cfg["exclude_paths"]=Json::arrayValue;EXPECT_EQ(changed(diff(R"({"appData":{"request_id":"a"}})",R"({"appData":{"request_id":"b"}})",cfg)),1u);}
TEST(Normalize,ReorderedObjectArrays){EXPECT_EQ(changed(diff(R"({"decisionResult":{"product_trials":[{"product_id":1,"decision":"REJECT"},{"product_id":2,"decision":"APPROVE"}]}})",R"({"decisionResult":{"product_trials":[{"product_id":2,"decision":"APPROVE"},{"product_id":1,"decision":"REJECT"}]}})")),0u);}
TEST(Normalize,CompositeKeys){auto rows=diff(R"({"decisionResult":{"available_offers":[{"product_id":1,"type":"standard","rank":1}]}})",R"({"decisionResult":{"available_offers":[{"product_id":1,"type":"manual","rank":2},{"product_id":1,"type":"standard","rank":1}]}})");EXPECT_EQ(rows.size(),6u);EXPECT_EQ(changed(rows),3u);}
TEST(Normalize,VectorsPreserveIndicesAndIgnoreBuckets){auto rows=diff(R"({"decisionResult":{"scoring":[{"modelName":"M","value":[0.42,0.58]}]}})",R"({"decisionResult":{"scoring":[{"modelName":"M","value":[0.58,0.42]}]}})");EXPECT_EQ(changed(rows),2u);for(auto& r:rows)if(r["DIFF_FLAG"].asInt())EXPECT_EQ(r["ADJUSTED_BEFORE"],r["ADJUSTED_AFTER"]);}
TEST(Normalize,MissingNullFalseZeroEmptyAndStringTypes){auto rows=diff(R"({"a":null,"b":false,"c":"NULL","d":0,"e":"","f":null})",R"({"b":null,"c":null,"d":false,"e":"","f":"NULL","g":null})");EXPECT_EQ(changed(rows),6u);std::map<std::string,Json::Value> map;for(auto& row:rows)map[row["KEY"].asString()]=row;EXPECT_EQ(map["a"]["CHANGE_TYPE"],"ATTRIBUTE_LOST");EXPECT_EQ(map["g"]["CHANGE_TYPE"],"ATTRIBUTE_NEW");EXPECT_EQ(map["f"]["TYPE_BEFORE"],"NULL");EXPECT_EQ(map["f"]["TYPE_AFTER"],"STRING");EXPECT_EQ(map["e"]["CHANGE_TYPE"],"SAME");}
TEST(Normalize,ExactNumberCanonicalization){EXPECT_EQ(lens::canonicalNumber("1.0"),"1");EXPECT_EQ(lens::canonicalNumber("10e-1"),"1");EXPECT_EQ(lens::canonicalNumber("-0.000"),"0");EXPECT_EQ(lens::canonicalNumber("12300e-2"),"123");EXPECT_EQ(lens::canonicalNumber("1e-3"),"0.001");EXPECT_EQ(changed(diff(R"({"n":1,"big":123456789012345678901234567890,"v":0.123456789012345678901})",R"({"n":1.00,"big":123456789012345678901234567890,"v":0.123456789012345678902})")),1u);}
TEST(Normalize,DuplicateKeysAreErrors){
 EXPECT_THROW(diff(R"({"decisionResult":{"product_trials":[{"product_id":1},{"product_id":1}]}})","{}"),std::exception);
 EXPECT_THROW(diff(R"({"x":1,"x":2})","{}"),std::exception);}
TEST(Normalize,UnknownArraysRequireConfiguration){
 EXPECT_THROW(diff(R"({"new":[1,2]})","{}"),std::exception);}
TEST(Normalize,ValueModeSupportsMultisetsExplicitly){auto cfg=lens::defaultConfig();cfg["array_rules"]["set"]=parsed(R"({"mode":"BY_VALUE","allow_duplicates":true})");auto rows=diff(R"({"set":["a","b","a"]})",R"({"set":["b","a","a"]})",cfg);EXPECT_EQ(changed(rows),0u);EXPECT_EQ(rows.size(),3u);auto typed=diff(R"({"set":[1,"1",null,"NULL",false,"false"]})","{}",cfg);EXPECT_EQ(typed.size(),6u);}
TEST(Normalize,ModelPredictorsStaySeparate){auto rows=diff(R"({"decisionResult":{"scoring":[{"modelName":"A","features":{"age":20}},{"modelName":"B","features":{"age":21}}]}})",R"({"decisionResult":{"scoring":[{"modelName":"A","features":{"age":20}},{"modelName":"B","features":{"age":22}}]}})");EXPECT_EQ(changed(rows),1u);for(auto& r:rows)if(r["DIFF_FLAG"].asInt())EXPECT_EQ(r["ARRAY_KEY"],"B");}
TEST(Normalize,EscapedCompositeKeysDoNotCollide){auto cfg=lens::defaultConfig();cfg["array_rules"]["items"]=parsed(R"({"mode":"field","fields":["a","b"]})");EXPECT_EQ(diff(R"({"items":[{"a":"x~y","b":"z"},{"a":"x","b":"y~z"}]})","{}",cfg).size(),4u);}
TEST(Aggregate,DistinctAppsAndDrillDownAcrossAttributesAndSources){auto rows=diff(R"({"a":1,"b":1})",R"({"a":2,"b":2})");auto cfg=lens::defaultConfig();cfg["aggregate_dimensions"]=parsed(R"(["DIFF_FLAG"])");auto seg=parsed(R"([{"APP_ID":"app1"}])");auto facts=lens::aggregate(rows,seg,cfg);ASSERT_EQ(facts.size(),1u);EXPECT_EQ(facts[0]["CNT"].asUInt64(),1u);ASSERT_EQ(facts[0]["APP_IDS"].size(),1u);EXPECT_EQ(facts[0]["APP_IDS"][0],"app1");lens::validateFacts(facts);cfg["aggregate_dimensions"]=parsed(R"(["SOURCE","DIFF_FLAG"])");facts=lens::aggregate(rows,seg,cfg);EXPECT_EQ(facts.size(),2u);for(auto& f:facts)EXPECT_EQ(f["CNT"].asUInt64(),1u);}
TEST(Contracts,RejectMalformedPythonResults){
 EXPECT_THROW(lens::validateNormalized(parsed(R"([{"APP_ID":"a"}])")),std::exception);auto rows=diff(R"({"a":1})",R"({"a":2})");rows[0]["DIFF_FLAG"]=0;
 EXPECT_THROW(lens::validateComparison(rows),std::exception);
 EXPECT_THROW(lens::validateFacts(parsed(R"([{"CNT":2,"APP_IDS":["a","a"]}])")),std::exception);}
TEST(Reference,FullProvidedDataset){const char* root=std::getenv("LENS_DATASET");if(!root)GTEST_SKIP()<<"Set LENS_DATASET to run the 2002-application reference test";auto begin=std::chrono::steady_clock::now();auto path=std::filesystem::path(root);if(std::filesystem::exists(path/"RESULTS"))path/="RESULTS";std::map<std::string,size_t> counts;size_t apps=0,changedApps=0,decisions=0,normalized=0,comparison=0;auto cfg=lens::defaultConfig();for(auto& entry:std::filesystem::directory_iterator(path/"BEFORE_MASTER")){if(entry.path().extension()!=".json")continue;auto id=entry.path().stem().string(),b=read(entry.path()),a=read(path/"AFTER_BETA"/entry.path().filename());auto nb=lens::normalize(b,id,"BEFORE",cfg),na=lens::normalize(a,id,"AFTER",cfg);auto rows=lens::compare(nb,na,cfg);++apps;normalized+=nb.size()+na.size();comparison+=rows.size();changedApps+=changed(rows)>0;for(auto& r:rows){++counts[r["CHANGE_TYPE"].asString()];if(r["PATH"]=="decisionResult.decision")decisions+=r["DIFF_FLAG"].asInt();}}EXPECT_EQ(apps,2002u);EXPECT_EQ(changedApps,502u);EXPECT_EQ(decisions,385u);EXPECT_EQ(normalized,572221u);EXPECT_EQ(comparison,289533u);EXPECT_EQ(counts["SAME"],275121u);EXPECT_EQ(counts["VALUE_CHANGED"],7567u);EXPECT_EQ(counts["ATTRIBUTE_NEW"],3835u);EXPECT_EQ(counts["ATTRIBUTE_LOST"],3010u);std::cout<<"Native full dataset: "<<apps<<" apps, "<<changedApps<<" changed, "<<comparison<<" rows; "<<std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count()<<" seconds\n";}
TEST(Reference,TeachingExampleTwelveFourteenAndSixDifferences){
 auto cfg=lens::defaultConfig();cfg["exclude_paths"].append("company.decisionResult.calculatedAt");cfg["array_rules"]["company.scoreCardResult"]=parsed(R"({"mode":"field","fields":["name"]})");cfg["array_rules"]["company.rejectReasons"]=parsed(R"({"mode":"value"})");
 const std::string before=R"({"company":{"decisionResult":{"decision":"APPROVE","businessDecision":"OFFER","calculatedAt":"2026-07-07T21:45:50.123Z"},"scoreCardResult":[{"name":"TAKEUP_RATE_IP","value":0.42,"version":"v3"},{"name":"PD_RETAIL","value":0.11,"version":"v7"}],"limitResult":{"limit":117325,"currency":"RUB"},"rejectReasons":["AGELOW@21","NOINCOME"]}})";
 const std::string after=R"({"company":{"decisionResult":{"decision":"VERIFICATION","businessDecision":"OFFER","riskSegment":"R2","calculatedAt":"2026-07-07T22:03:11.480Z"},"scoreCardResult":[{"name":"PD_RETAIL","value":0.11,"version":"v7"},{"name":"TAKEUP_RATE_IP","value":0.58,"version":"v4"}],"limitResult":{"limit":96500,"currency":"RUB"},"rejectReasons":["NOINCOME","AGELOW@21","DOCS_EXPIRED"]}})";
 auto b=lens::normalize(before,"example","BEFORE",cfg),a=lens::normalize(after,"example","AFTER",cfg);EXPECT_EQ(b.size(),12u);EXPECT_EQ(a.size(),14u);auto rows=lens::compare(b,a,cfg);EXPECT_EQ(rows.size(),14u);EXPECT_EQ(changed(rows),6u);
}

TEST(Normalize,EmptyCompositeAndNestedKeysKeepTheirPosition){
 auto cfg=lens::defaultConfig();cfg["array_rules"]["items"]=parsed(R"({"mode":"field","fields":["a","b"]})");
 auto rows=lens::normalize(R"({"items":[{"a":"","b":"x"},{"a":"x","b":""}]})","app1","BEFORE",cfg);
 ASSERT_EQ(rows.size(),4u);std::set<std::string> keys;for(const auto& r:rows)keys.insert(r["ARRAY_KEY"].asString());
 EXPECT_EQ(keys,(std::set<std::string>{"~x","x~"}));
 cfg["array_rules"]["outer"]=parsed(R"({"mode":"field","fields":["id"]})");cfg["array_rules"]["outer.inner"]=parsed(R"({"mode":"field","fields":["id"]})");
 rows=lens::normalize(R"({"outer":[{"id":"","inner":[{"id":"x","value":2}]}]})","app1","BEFORE",cfg);
 for(const auto& r:rows)if(r["PATH"]=="outer.inner.value")EXPECT_EQ(r["ARRAY_KEY"],"|x");
}
TEST(Normalize,StableRowOrderingAndStringMode){
 auto cfg=lens::defaultConfig();cfg["array_rules"]["values"]="BY_VALUE";
 EXPECT_EQ(lens::normalize(R"({"z":0,"values":["b","a"],"a":1})","app1","BEFORE",cfg),lens::normalize(R"({"a":1,"values":["a","b"],"z":0})","app1","BEFORE",cfg));
}
TEST(Normalize,BucketRulesValidateEntireSequence){
 auto cfg=lens::defaultConfig();cfg["bucket_rules"]["v"]=parsed("[100,50]");
 
 EXPECT_THROW(lens::normalize(R"({"v":1})","app1","BEFORE",cfg),std::exception);
 cfg["bucket_rules"]["v"]=parsed(R"([100,"200oops"])");
 
 EXPECT_THROW(lens::normalize(R"({"v":1})","app1","BEFORE",cfg),std::exception);
}
TEST(Normalize,LongNumbersAndMalformedNumberGrammar){
 EXPECT_EQ(lens::canonicalNumber(std::string(20000,'1')),std::string(20000,'1'));
 for(const auto& s:{"", "-", "01", "1.", ".1", "1e", "1e+", "NaN", "+1", "--1"})
 EXPECT_THROW(lens::canonicalNumber(s),std::exception);
}
TEST(Contracts,RejectSemanticallyInconsistentComparison){
 auto rows=diff(R"({"a":1})",R"({"a":2})");rows[0]["CHANGE_TYPE"]="SAME";rows[0]["DIFF_FLAG"]=0;
 
 EXPECT_THROW(lens::validateComparison(rows),std::exception);
 rows=diff(R"({"a":true})",R"({"a":false})");rows[0]["VALUE_AFTER"]="FALSE";
 
 EXPECT_THROW(lens::validateComparison(rows),std::exception);
 rows=diff(R"({"a":1})","{}");rows[0]["TYPE_AFTER"]="NUMBER";
 
 EXPECT_THROW(lens::validateComparison(rows),std::exception);
 rows=diff(R"({"a":null})",R"({"a":null})");rows[0]["VALUE_BEFORE"]="null";
 
 EXPECT_THROW(lens::validateComparison(rows),std::exception);
}
TEST(Segments,CustomArraysAndDetailedProductContext){
 auto cfg=lens::defaultConfig();cfg["array_rules"]["custom"]="BY_INDEX";
 std::string raw=R"({"custom":[1,2],"decisionResult":{"decision":"APPROVE","product_trials":[{"product_id":2,"product_name":"Z","scoring":{"pd":{"modelName":"B"}},"cutOff":{"name":"C2","value":0.7}},{"product_id":1,"product_name":"A","scoring":{"pd":{"modelName":"A"}},"cutOff":{"name":"C1","value":0.8}}]}})";
 auto seg=lens::segments(raw,raw,"app1",cfg);
 EXPECT_EQ(seg["PRODUCTS"],"A;Z");EXPECT_EQ(seg["PD_MODEL_BEFORE"],"A;B");EXPECT_EQ(seg["CUTOFF_BEFORE"],"C1;C2");
 ASSERT_EQ(seg["PRODUCT_CONTEXT_BEFORE"].size(),2u);EXPECT_EQ(seg["PRODUCT_CONTEXT_BEFORE"][0]["PRODUCT_ID"],"1");EXPECT_EQ(seg["PRODUCT_CONTEXT_BEFORE"][0]["PD_MODEL"],"A");EXPECT_EQ(seg["PRODUCT_CONTEXT_BEFORE"][0]["ATTRIBUTES"].size(),5u);
 EXPECT_EQ(seg["CHANGED"],0);
}
TEST(Aggregate,MultipleAppsAndDuplicateSegments){
 auto rows=diff(R"({"a":1,"b":1})",R"({"a":2,"b":2})");auto others=rows;for(auto& r:others){r["APP_ID"]="app2";rows.append(r);}
 auto cfg=lens::defaultConfig();cfg["aggregate_dimensions"]=parsed(R"(["DIFF_FLAG"])");
 auto seg=parsed(R"([{"APP_ID":"app1"},{"APP_ID":"app2"}])");auto facts=lens::aggregate(rows,seg,cfg);
 ASSERT_EQ(facts.size(),1u);EXPECT_EQ(facts[0]["CNT"].asUInt64(),2u);EXPECT_EQ(facts[0]["APP_IDS"],parsed(R"(["app1","app2"] )"));
 seg.append(seg[0]);
 EXPECT_THROW(lens::aggregate(rows,seg,cfg),std::exception);
}

TEST(Normalize,BucketsUseExactBoundariesAndHugeNumbers){
 auto cfg=lens::defaultConfig();cfg["bucket_rules"]["v"]=parsed("[0,0.3,0.6,1]");
 auto value=[&](const std::string& v){return lens::normalize("{\"v\":"+v+"}","app1","BEFORE",cfg)[0]["ADJUSTED_VALUE"].asString();};
 EXPECT_EQ(value("0.6"),"(0.3; 0.6]");EXPECT_EQ(value("0.60000000000000000000000001"),"(0.6; 1]");
 EXPECT_EQ(value("-1e999"),"(-inf; 0]");EXPECT_EQ(value("1e999"),"(1; +inf)");
 cfg["bucket_rules"]["v"]=parsed(R"(["-1.5","-1","0","2"])");
 EXPECT_EQ(value("-1.2"),"(-1.5; -1]");EXPECT_EQ(value("1.999999999999999999999"),"(0; 2]");
}

TEST(Contracts,RejectDuplicateMembersAndInconsistentCounts){
 
 EXPECT_THROW(lens::validateFacts(parsed(R"([{"CNT":1,"APP_IDS":["a","a"]}])")),std::exception);
 
 EXPECT_THROW(lens::validateFacts(parsed(R"([{"CNT":1,"counts":2,"APP_IDS":["a"]}])")),std::exception);
}

TEST(Normalize,OrdinaryAtSignsRemainCompatibleWithOrganizerChecks){
 auto cfg=lens::defaultConfig();cfg["array_rules"]["items"]=parsed(R"({"mode":"value","allow_duplicates":true})");
 auto rows=lens::normalize(R"({"items":["AGELOW@21","@str:1","1",1,"x@0","x"]})","app1","BEFORE",cfg);
 ASSERT_EQ(rows.size(),6u);std::set<std::string> keys;for(const auto& row:rows)keys.insert(row["ARRAY_KEY"].asString());
 EXPECT_EQ(keys.count("AGELOW@21@0"),1u);EXPECT_EQ(keys.count("@str:1@0"),1u);EXPECT_EQ(keys.count("%40str:1@0"),1u);
 EXPECT_EQ(keys.count("x@0@0"),1u);EXPECT_EQ(keys.count("x@0"),1u);
 cfg["array_rules"]["items"]["allow_duplicates"]=false;
 rows=lens::normalize(R"({"items":["AGELOW@21"]})","app1","BEFORE",cfg);EXPECT_EQ(rows[0]["ARRAY_KEY"],"AGELOW@21");
}

#ifdef LENS_FIXTURES_DIR
namespace {
Json::Value csvRows(const std::filesystem::path& path){
 std::istringstream file(read(path));std::string line;std::vector<std::string> header;Json::Value rows(Json::arrayValue);
 auto columns=[](std::string line){if(!line.empty()&&line.back()=='\r')line.pop_back();std::vector<std::string> result;size_t begin=0,end;do{end=line.find(';',begin);result.push_back(line.substr(begin,end==std::string::npos?end:end-begin));begin=end==std::string::npos?end:end+1;}while(end!=std::string::npos);return result;};
 std::getline(file,line);header=columns(line);if(!header.empty()&&header[0].rfind("\xef\xbb\xbf",0)==0)header[0].erase(0,3);
 while(std::getline(file,line)){if(line.empty()||line=="\r")continue;auto fields=columns(line);if(fields.size()!=header.size())throw std::runtime_error("Malformed reference CSV");Json::Value row;for(size_t i=0;i<header.size();++i)row[header[i]]=fields[i];rows.append(row);}return rows;
}
std::string referencePath(std::string path){
 // The organizer's CSV introduces illustrative nodes absent from its JSON.
 for(const auto& pair:std::vector<std::pair<std::string,std::string>>{{"company.scoreCardResult.scoreCard.","company.scoreCardResult."},{"company.limitResult.Limit.","company.limitResult."},{"company.rejectReasons.reason.code","company.rejectReasons"}}){auto pos=path.find(pair.first);if(pos!=std::string::npos)path.replace(pos,pair.first.size(),pair.second);}return path;
}
std::string referenceBucket(std::string value){
 const std::map<std::string,std::string> labels={{"(0.3 - 0.6]","(0.3; 0.6]"},{"(0 - 0.3]","(0; 0.3]"},{"(100K - 250K]","(100000; 250000]"},{"(0 - 100K]","(0; 100000]"}};auto it=labels.find(value);return it==labels.end()?value:it->second;
}
}
TEST(Reference,ActualTeachingFilesMatchEveryComparisonValue){
 auto root=std::filesystem::path(LENS_FIXTURES_DIR)/"example";auto cfg=lens::defaultConfig();
 cfg["exclude_paths"].append("company.decisionResult.calculatedAt");cfg["array_rules"]["company.scoreCardResult"]=parsed(R"({"mode":"field","fields":["name"]})");cfg["array_rules"]["company.rejectReasons"]=parsed(R"({"mode":"value"})");cfg["bucket_rules"]["company.limitResult.limit"]=parsed("[0,100000,250000]");
 auto b=lens::normalize(read(root/"01_response_BEFORE.json"),"1001","BEFORE",cfg),a=lens::normalize(read(root/"02_response_AFTER.json"),"1001","AFTER",cfg);auto compared=lens::compare(b,a,cfg);
 EXPECT_EQ(b.size(),12u);EXPECT_EQ(a.size(),14u);EXPECT_EQ(b.size()+a.size(),26u);EXPECT_EQ(compared.size(),14u);EXPECT_EQ(changed(compared),6u);
 auto expected=csvRows(root/"06_comparison.csv");ASSERT_EQ(compared.size(),expected.size());
 std::map<std::pair<std::string,std::string>,Json::Value> actual;for(const auto& row:compared)actual[{row["PATH"].asString(),row["ARRAY_KEY"].asString()}]=row;
 for(const auto& row:expected){auto it=actual.find({referencePath(row["PATH"].asString()),row["ARRAY_KEY"].asString()});ASSERT_NE(it,actual.end())<<row;const auto& result=it->second;
  for(auto key:{"APP_ID","VALUE_BEFORE","VALUE_AFTER","CHANGE_TYPE"})EXPECT_EQ(result[key],row[key])<<key<<" "<<row;
  EXPECT_EQ(result["DIFF_FLAG"].asInt(),std::stoi(row["DIFF_FLAG"].asString()));
  for(auto key:{"ADJUSTED_BEFORE","ADJUSTED_AFTER"})EXPECT_EQ(result[key].asString(),referenceBucket(row[key].asString()))<<key<<" "<<row;
 }
}
TEST(Reference,RealSamplesRetainSegmentsAndStableArrayKeys){
 auto root=std::filesystem::path(LENS_FIXTURES_DIR)/"samples";auto cfg=lens::defaultConfig();size_t count=0;
 for(const auto& entry:std::filesystem::directory_iterator(root/"BEFORE")){
  if(entry.path().extension()!=".json")continue;++count;auto id=entry.path().stem().string();auto b=read(entry.path()),a=read(root/"AFTER"/entry.path().filename());auto rows=lens::compare(lens::normalize(b,id,"BEFORE",cfg),lens::normalize(a,id,"AFTER",cfg),cfg);lens::validateComparison(rows,id);
  auto segment=lens::segments(b,a,id,cfg);EXPECT_EQ(segment["APP_ID"],id);EXPECT_EQ(segment["CHANGED"].asInt(),changed(rows)>0?1:0);EXPECT_FALSE(segment["PRODUCT_CONTEXT_BEFORE"].empty());
  if(id=="I32554354"){
   EXPECT_EQ(segment["DECISION_BEFORE"],"REJECT");EXPECT_EQ(segment["DECISION_AFTER"],"APPROVE");bool found=false;
   for(const auto& row:rows)if(row["PATH"]=="decisionResult.product_trials.rules.hit"&&row["ARRAY_KEY"]=="15210|AGELOW@21"){found=true;EXPECT_EQ(row["CHANGE_TYPE"],"ATTRIBUTE_LOST");EXPECT_EQ(row["VALUE_BEFORE"],"true");}EXPECT_TRUE(found);
  }
 }
 EXPECT_EQ(count,4u);
}
#endif

TEST(Segments,ReusedNormalizationPreservesTypedFlagsAndContext){
 auto cfg=lens::defaultConfig();cfg["array_rules"]["custom"]="BY_VALUE";
 const std::string before=R"({"custom":[1,2],"characteristics":{"score_okb":710},"decisionResult":{"decision":"REJECT","product_trials":[{"product_id":1,"product_name":"A"}]}})";
 for(const auto& after:{before,
   std::string(R"({"custom":[2,1],"characteristics":{"score_okb":"710"},"decisionResult":{"decision":"REJECT","product_trials":[{"product_id":1,"product_name":"A"}]}})"),
   std::string(R"({"custom":[1,2],"characteristics":{"score_okb":710},"decisionResult":{"decision":"APPROVE","product_trials":[{"product_id":1,"product_name":"A"}]}})")}){
  auto b=lens::normalize(before,"app1","BEFORE",cfg),a=lens::normalize(after,"app1","AFTER",cfg);
  auto segment=lens::segmentsFromNormalized(b,a,"app1");EXPECT_EQ(segment,lens::segments(before,after,"app1",cfg));
  auto comparisons=lens::compare(b,a,cfg);bool any=false,external=false;
  for(const auto& row:comparisons)if(row["DIFF_FLAG"].asInt()){any=true;auto path=row["PATH"].asString();external=external||path.rfind("characteristics.",0)==0||path.rfind("ext_data.",0)==0;}
  EXPECT_EQ(segment["CHANGED"].asInt(),any?1:0);EXPECT_EQ(segment["EXTERNAL_CHANGED"].asInt(),external?1:0);
 }
 auto b=lens::normalize(before,"app1","BEFORE",cfg);Json::Value empty(Json::arrayValue);
 auto lost=lens::segmentsFromNormalized(b,empty,"app1");EXPECT_EQ(lost["CHANGED"],1);EXPECT_EQ(lost["EXTERNAL_CHANGED"],1);
 auto added=lens::segmentsFromNormalized(empty,lens::normalize(before,"app1","AFTER",cfg),"app1");EXPECT_EQ(added["CHANGED"],1);EXPECT_EQ(added["EXTERNAL_CHANGED"],1);
 EXPECT_THROW(lens::segmentsFromNormalized(b,empty,"foreign"),std::exception);
}

TEST(Aggregate,RepeatedSerializationPreservesStructuredDimensionTypes){
 auto cfg=lens::defaultConfig();cfg["aggregate_dimensions"]=parsed(R"(["CONTEXT"])");
 const auto contexts=parsed(R"([null,false,0,"0",true,"",["a",null,3],{"nested":{"quotes":"\"\\\n","unicode":"\u0410"},"values":[true,false,0,"0"]}])");
 auto base=diff(R"({"nested":{"v":1}})",R"({"nested":{"v":1}})");
 auto exercise=[&](){
  Json::Value rows(Json::arrayValue),segments(Json::arrayValue);
  for(Json::ArrayIndex i=0;i<=contexts.size();++i){
   std::string app="app"+std::to_string(i);auto row=base[0];row["APP_ID"]=app;rows.append(row);
   Json::Value segment;segment["APP_ID"]=app;segment["CONTEXT"]=contexts[i%contexts.size()];segments.append(segment);
  }
  auto facts=lens::aggregate(rows,segments,cfg);lens::validateFacts(facts);return facts;
 };
 auto expected=exercise();ASSERT_EQ(expected.size(),contexts.size());
 for(const auto& context:contexts){size_t found=0;for(const auto& fact:expected)if(fact["CONTEXT"]==context){++found;EXPECT_EQ(fact["CNT"].asUInt64(),context.isNull()?2u:1u);}EXPECT_EQ(found,1u);}
 for(int iteration=0;iteration<10;++iteration)EXPECT_EQ(exercise(),expected);
 // Concurrent runs must not share mutable writer or output-buffer state.
 std::vector<std::future<Json::Value>> tasks;
 for(int thread=0;thread<4;++thread)tasks.push_back(std::async(std::launch::async,exercise));
 for(auto& task:tasks)EXPECT_EQ(task.get(),expected);
}
