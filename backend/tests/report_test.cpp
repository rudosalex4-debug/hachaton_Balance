#include "reports.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <limits>

namespace {
Json::Value parseReportJson(const std::string& input){Json::CharReaderBuilder builder;auto reader=std::unique_ptr<Json::CharReader>(builder.newCharReader());Json::Value value;std::string error;if(!reader->parse(input.data(),input.data()+input.size(),&value,&error))throw std::runtime_error(error);return value;}
Json::Value checksConfig(){return parseReportJson(R"([{"id":"check_one","name":"One","params":{"app_ids_limit":1}}])");}
Json::Value resultRow(){return parseReportJson(R"({"CHECK_ID":"check_one","CHECK_NAME":"One","LABEL":"Example","SOURCE":"AFTER","ARGUMENT":"score","CNT":2,"TOTAL":3,"METRIC":0.25,"APP_IDS":["A"],"RESULT":"FAIL","CONFIDENCE":"LOW"})");}
Json::Value wrap(const Json::Value& row){Json::Value rows(Json::arrayValue);rows.append(row);return rows;}
}
TEST(CheckContract,AcceptsLimitedSamplesWithoutConfusingThemWithCount){
 EXPECT_NO_THROW(lens::validateCheckResults(wrap(resultRow()),checksConfig(),{"A","B","C"}));}
TEST(CheckContract,RejectsForeignIdentityAndInvalidMetrics){auto row=resultRow();row["CHECK_ID"]="foreign";
 EXPECT_THROW(lens::validateCheckResults(wrap(row),checksConfig(),{"A","B","C"}),std::exception);row=resultRow();row["METRIC"]="not a number";
 EXPECT_THROW(lens::validateCheckResults(wrap(row),checksConfig(),{"A","B","C"}),std::exception);row=resultRow();row["METRIC"]=std::numeric_limits<double>::infinity();
 EXPECT_THROW(lens::validateCheckResults(wrap(row),checksConfig(),{"A","B","C"}),std::exception);}
TEST(CheckContract,RejectsWrongCountsAndMembers){auto row=resultRow();row["CNT"]=4;
 EXPECT_THROW(lens::validateCheckResults(wrap(row),checksConfig(),{"A","B","C"}),std::exception);row=resultRow();row["APP_IDS"][0]="foreign";
 EXPECT_THROW(lens::validateCheckResults(wrap(row),checksConfig(),{"A","B","C"}),std::exception);row=resultRow();row["APP_IDS"].append("A");
 EXPECT_THROW(lens::validateCheckResults(wrap(row),checksConfig(),{"A","B","C"}),std::exception);row=resultRow();row["CONFIDENCE"]="MEDIUM";
 EXPECT_THROW(lens::validateCheckResults(wrap(row),checksConfig(),{"A","B","C"}),std::exception);}
TEST(Report,EscapesUserContentAndKeepsSafeOfflineDrilldown){auto report=parseReportJson(R"({"run_id":"abc123","name":"<script>alert(1)</script>","finished_at":"now","summary":{"total_apps":3,"changed_apps":2,"decision_changed":1,"changed_without_decision":1},"checks_summary":{"checks":1}})");auto row=resultRow();row["LABEL"]="<img src=x onerror=alert(2)>";row["ARGUMENT"]="\" onclick=\"alert(3)";row["APP_IDS"][0]="A&x=<bad>";auto html=lens::renderReportHtml(report,wrap(row),"https://example.test");
 EXPECT_EQ(html.find("<script>"),std::string::npos);
 EXPECT_EQ(html.find("<img src=x"),std::string::npos);
 EXPECT_NE(html.find("&lt;script&gt;"),std::string::npos);
 EXPECT_NE(html.find("class=\"low\""),std::string::npos);
 EXPECT_NE(html.find("https://example.test/?run=abc123&amp;tab=pivot"),std::string::npos);
 EXPECT_NE(html.find("app=A%26x%3D%3Cbad%3E"),std::string::npos);
 EXPECT_NE(html.find("Показано 1 из 2"),std::string::npos);}
TEST(Report,RejectsExecutableBaseAndShowsEmptyChecks){auto report=parseReportJson(R"({"run_id":"abc","name":"Example","finished_at":"now","summary":{},"checks_summary":{"checks":0}})");
 EXPECT_THROW(lens::renderReportHtml(report,Json::Value(Json::arrayValue),"javascript:alert(1)"),std::exception);auto html=lens::renderReportHtml(report,Json::Value(Json::arrayValue),"http://localhost:8088");
 EXPECT_NE(html.find("Проверки не вернули строк"),std::string::npos);}
