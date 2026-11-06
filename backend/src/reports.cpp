#include "reports.hpp"
#include <cmath>
#include <map>
#include <sstream>
#include <vector>
#include <stdexcept>

namespace lens {
void validateCheckResults(const Json::Value& rows,const Json::Value& checks,const std::set<std::string>& appIds){
 if(!rows.isArray()||rows.size()>100000)throw std::runtime_error("Checks must emit at most 100000 result rows");
 std::map<std::string,const Json::Value*> selected;for(const auto& check:checks)selected[check["id"].asString()]=&check;
 for(const auto& row:rows){
  for(auto key:{"CHECK_ID","CHECK_NAME","LABEL","SOURCE","ARGUMENT","RESULT","CONFIDENCE"})if(!row[key].isString()||row[key].asString().size()>16000)throw std::runtime_error(std::string("Invalid check result string: ")+key);
  const auto found=selected.find(row["CHECK_ID"].asString());if(found==selected.end()||row["CHECK_NAME"]!=(*found->second)["name"])throw std::runtime_error("Check result references an unknown procedure");
  if(row["SOURCE"]!="BEFORE"&&row["SOURCE"]!="AFTER")throw std::runtime_error("Check SOURCE must be BEFORE or AFTER");
  if(row["RESULT"]!="PASS"&&row["RESULT"]!="FAIL")throw std::runtime_error("Check RESULT must be PASS or FAIL");
  if(row["CONFIDENCE"]!="HIGH"&&row["CONFIDENCE"]!="LOW")throw std::runtime_error("Check CONFIDENCE must be HIGH or LOW");
  if(!row["CNT"].isIntegral()||!row["TOTAL"].isIntegral()||!row["CNT"].isUInt64()||!row["TOTAL"].isUInt64()||row["CNT"].asUInt64()>row["TOTAL"].asUInt64()||row["TOTAL"].asUInt64()>appIds.size())throw std::runtime_error("Invalid check CNT/TOTAL");
  if(!row["METRIC"].isNumeric()||!std::isfinite(row["METRIC"].asDouble()))throw std::runtime_error("Check METRIC must be a finite number");
  const auto& params=(*found->second)["params"];const auto limit=params.get("app_ids_limit",20);
  if(!limit.isUInt64()||!row["APP_IDS"].isArray()||row["APP_IDS"].size()>row["CNT"].asUInt64()||row["APP_IDS"].size()>limit.asUInt64())throw std::runtime_error("Invalid check APP_IDS sample size");
  std::set<std::string> seen;for(const auto& app:row["APP_IDS"])if(!app.isString()||!appIds.count(app.asString())||!seen.insert(app.asString()).second)throw std::runtime_error("Check APP_IDS must contain unique known applications");
 }
}
namespace {
std::string escapeHtml(const std::string& value){std::string out;for(char c:value)switch(c){case '&':out+="&amp;";break;case '<':out+="&lt;";break;case '>':out+="&gt;";break;case '"':out+="&quot;";break;case '\'':out+="&#39;";break;default:out+=c;}return out;}
std::string urlEncode(const std::string& text){const char* hex="0123456789ABCDEF";std::string out;for(unsigned char c:text){if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')out+=char(c);else{out+='%';out+=hex[c>>4];out+=hex[c&15];}}return out;}
std::string display(const Json::Value& value){if(value.isString())return value.asString();if(value.isNull())return "—";Json::StreamWriterBuilder writer;writer["indentation"]="";return Json::writeString(writer,value);}
}
std::string renderReportHtml(const Json::Value& report,const Json::Value& checks,const std::string& applicationBase){
 // Only HTTP(S) app origins are accepted; arbitrary user strings never become
 // executable URL schemes in an offline report.
 if(applicationBase.rfind("http://",0)!=0&&applicationBase.rfind("https://",0)!=0)throw std::runtime_error("Report application base must be HTTP(S)");
 const auto run=report["run_id"].asString();const auto link=applicationBase+"/?run="+urlEncode(run);
 std::ostringstream out;out<<"<!doctype html><html lang=\"ru\"><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Отчёт — "<<escapeHtml(report["name"].asString())<<"</title><style>body{font:15px/1.5 system-ui,sans-serif;background:#f3f5f7;color:#20303d;margin:0;padding:32px}main{max-width:1400px;margin:auto}h1{font-size:28px}a{color:#176e7c}.cards{display:flex;flex-wrap:wrap;gap:16px}.card{background:white;border:1px solid #dce3e8;padding:16px;border-radius:12px;min-width:170px}.card strong{display:block;font-size:26px}.table{overflow:auto;background:white;border:1px solid #dce3e8;border-radius:12px;margin-top:24px}table{border-collapse:collapse;width:100%}th,td{text-align:left;padding:12px;border-bottom:1px solid #e5eaee;vertical-align:top}th{background:#edf2f5}tr.fail{background:#fff0ee}tr.low{background:#f6f6f4;color:#777}small{display:block;color:#667;max-width:70em}.badge{font-weight:700}.links{margin:20px 0}code{overflow-wrap:anywhere}.apps a{display:inline-block;margin:0 8px 4px 0}</style><main><h1>"<<escapeHtml(report["name"].asString())<<"</h1><p>Проверки и изменения СПР · запуск <code>"<<escapeHtml(run)<<"</code></p><small>Завершён: "<<escapeHtml(report["finished_at"].asString())<<". Документ содержит сохранённый результат полностью завершённого запуска.</small><p class=\"links\"><a href=\""<<escapeHtml(link+"&tab=pivot")<<"\">Открыть сводную таблицу в приложении</a></p><div class=\"cards\">";
 const auto& summary=report["summary"];
 for(const auto& item:std::vector<std::pair<std::string,std::string>>{{"Заявок","total_apps"},{"С отличиями","changed_apps"},{"Смена решения","decision_changed"},{"Отличия при прежнем решении","changed_without_decision"}})out<<"<div class=\"card\">"<<item.first<<"<strong>"<<escapeHtml(display(summary[item.second]))<<"</strong></div>";
 out<<"</div><h2>Единый отчёт проверок</h2><small>LOW отмечает слабый сигнал и выделяется мягко. APP_IDS — ограниченная выборка: её размер не заменяет CNT. Наблюдаемые изменения сами по себе не доказывают причину смены решения.</small><div class=\"table\"><table><thead><tr><th>Проверка</th><th>Срез</th><th>Аргумент</th><th>Числа</th><th>Метрика</th><th>Результат</th><th>Уверенность</th><th>Заявки из выборки</th></tr></thead><tbody>";
 for(const auto& row:checks){const bool low=row["CONFIDENCE"]=="LOW";out<<"<tr class=\""<<(low?"low":row["RESULT"]=="FAIL"?"fail":"pass")<<"\"><td>"<<escapeHtml(row["LABEL"].asString())<<"<small>"<<escapeHtml(row["CHECK_NAME"].asString())<<"</small></td><td>"<<escapeHtml(row["SOURCE"].asString())<<"</td><td>"<<escapeHtml(row["ARGUMENT"].asString())<<"</td><td>"<<escapeHtml(display(row["CNT"]))<<" / "<<escapeHtml(display(row["TOTAL"]))<<"</td><td>"<<escapeHtml(display(row["METRIC"]))<<"</td><td class=\"badge\">"<<escapeHtml(row["RESULT"].asString())<<"</td><td>"<<escapeHtml(row["CONFIDENCE"].asString())<<"</td><td class=\"apps\">";for(const auto& app:row["APP_IDS"])out<<"<a href=\""<<escapeHtml(link+"&tab=comparison&app="+urlEncode(app.asString()))<<"\">"<<escapeHtml(app.asString())<<"</a>";out<<"<small>Показано "<<row["APP_IDS"].size()<<" из "<<escapeHtml(display(row["CNT"]))<<"</small></td></tr>";}
 if(checks.empty())out<<"<tr><td colspan=\"8\">Проверки не вернули строк. Выбрано проверок: "<<escapeHtml(display(report["checks_summary"]["checks"]))<<".</td></tr>";
 out<<"</tbody></table></div><p><a href=\""<<escapeHtml(link+"&tab=report")<<"\">Открыть полный отчёт в приложении</a></p></main></html>";return out.str();
}
}
