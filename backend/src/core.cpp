#include "core.hpp"
#include <simdjson.h>
#include <algorithm>
#include <cmath>
#include <charconv>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

namespace lens {
uint32_t StringInterner::intern(const std::string& value) {
    auto found = ids.find(value);
    if (found != ids.end()) return found->second;
    if (strings.size() >= std::numeric_limits<uint32_t>::max()) throw std::runtime_error("String dictionary exhausted");
    uint32_t id = static_cast<uint32_t>(strings.size());
    strings.push_back(value); ids.emplace(value, id); return id;
}
const std::string& StringInterner::get(uint32_t id) const { return strings.at(id); }

namespace {
std::string dump(const Json::Value& value) {
    // Key serialization is on the per-row hot path. JsonCpp resets its writer
    // state for each write; keep its immutable settings and stream per thread.
    // Keeping all other builder defaults preserves the existing canonical keys.
    struct Serializer {
        std::unique_ptr<Json::StreamWriter> writer;
        std::ostringstream stream;
        Serializer(){Json::StreamWriterBuilder builder;builder["indentation"]="";writer.reset(builder.newStreamWriter());}
    };
    thread_local Serializer serializer;
    serializer.stream.str(std::string{});
    serializer.stream.clear();
    serializer.writer->write(value,&serializer.stream);
    if(!serializer.stream)throw std::runtime_error("Cannot serialize JSON key");
    return serializer.stream.str();
}
Json::Value parse(const std::string& value) { Json::CharReaderBuilder b; b["rejectDupKeys"]=true; Json::Value out; std::string errors; auto r=std::unique_ptr<Json::CharReader>(b.newCharReader()); if(!r->parse(value.data(),value.data()+value.size(),&out,&errors)) throw std::runtime_error(errors); return out; }
std::string trim(std::string s) { auto a=s.find_first_not_of(" \n\t\r"); if(a==std::string::npos)return ""; auto b=s.find_last_not_of(" \n\t\r"); return s.substr(a,b-a+1); }
std::string typeName(ValueType type) { switch(type){case ValueType::Null:return "NULL";case ValueType::Boolean:return "BOOLEAN";case ValueType::Number:return "NUMBER";default:return "STRING";} }
ValueType parseType(const std::string& type) { if(type=="NULL"||type=="null")return ValueType::Null;if(type=="BOOLEAN"||type=="bool")return ValueType::Boolean;if(type=="NUMBER"||type=="number"||type=="int"||type=="float")return ValueType::Number;if(type=="STRING"||type=="str")return ValueType::String;throw std::runtime_error("Unsupported VALUE_TYPE: "+type); }
std::vector<std::string> split(const std::string& text, char delim='.') { std::vector<std::string> out; std::stringstream stream(text);std::string part;while(std::getline(stream,part,delim))out.push_back(part);return out; }
std::string join(const std::vector<std::string>& values,const std::string& sep) {std::string out;bool first=true;for(auto& x:values){if(!first)out+=sep;first=false;out+=x;}return out;}
bool glob(const std::string& pattern,const std::string& value) { size_t p=0,v=0,star=std::string::npos,mark=0;while(v<value.size()){if(p<pattern.size()&&(pattern[p]=='?'||pattern[p]==value[v])){++p;++v;}else if(p<pattern.size()&&pattern[p]=='*'){star=p++;mark=v;}else if(star!=std::string::npos){p=star+1;v=++mark;}else return false;}while(p<pattern.size()&&pattern[p]=='*')++p;return p==pattern.size();}
std::string escape(const std::string& text) {std::string out;for(unsigned char c:text){switch(c){case '%':out+="%25";break;case '|':out+="%7C";break;case '~':out+="%7E";break;default:out+=static_cast<char>(c);}}return out;}
struct Scalar {ValueType type;std::string value;};
Scalar scalar(simdjson::ondemand::value value) {
    using simdjson::ondemand::json_type;
    switch(value.type().value()) {
    case json_type::null: if(!value.is_null().value())throw std::runtime_error("Invalid null");return {ValueType::Null,"NULL"};
    case json_type::boolean:return {ValueType::Boolean,value.get_bool().value()?"true":"false"};
    case json_type::number:return {ValueType::Number,canonicalNumber(std::string(value.raw_json_token()))};
    case json_type::string:return {ValueType::String,std::string(value.get_string().value())};
    default:throw std::runtime_error("Array key must be a scalar");
    }
}
std::string identity(const Scalar& value) {
    auto result=escape(value.value);
    if(value.type==ValueType::String){if(value.value.rfind("@str:",0)==0)result.replace(0,1,"%40");bool ambiguous=value.value=="NULL"||value.value=="true"||value.value=="false";try{canonicalNumber(value.value);ambiguous=true;}catch(const std::exception&){}if(ambiguous)result="@str:"+result;}
    return result;
}
std::string pointer(const std::string& path){std::string out;for(auto& part:split(path)){out+='/';for(char c:part){if(c=='~')out+="~0";else if(c=='/')out+="~1";else out+=c;}}return out;}
Scalar fieldScalar(const std::string& raw,const std::string& field){simdjson::ondemand::parser p;auto data=simdjson::padded_string(raw);auto doc=p.iterate(data);return scalar(doc.at_pointer(pointer(field)).value());}
Json::Value structure(const std::string& path){auto parts=split(path);Json::Value out;out["PATH"]=path;out["PREFIX"]=parts.empty()?"":parts[0];out["CLASS"]=parts.size()>1?parts[1]:"";out["KEY"]=parts.empty()?"":parts.back();std::vector<std::string> suffix;if(parts.size()>3)suffix.assign(parts.begin()+2,parts.end()-1);out["SUFFIX"]=join(suffix,".");return out;}
int decimalCompare(const std::string& a,const std::string& b){
    if(a==b)return 0;
    bool an=a[0]=='-',bn=b[0]=='-';if(an!=bn)return an?-1:1;
    auto left=a.substr(an?1:0),right=b.substr(bn?1:0);
    auto li=left.find('.'),ri=right.find('.');if(li==std::string::npos)li=left.size();if(ri==std::string::npos)ri=right.size();
    int sign=an?-1:1;if(li!=ri)return li<ri?-sign:sign;
    if(left.find('.')==std::string::npos)left+='.';if(right.find('.')==std::string::npos)right+='.';
    size_t length=std::max(left.size(),right.size());left.resize(length,'0');right.resize(length,'0');
    return left<right?-sign:sign;
}
std::string numericEdge(const Json::Value& edge){
    if(edge.isString())return canonicalNumber(edge.asString());
    if(!edge.isNumeric())throw std::runtime_error("Bucket edge must be numeric");
    // Use a shortest round-trip representation: JsonCpp's default 17-digit
    // formatting would turn the configured 0.6 boundary into 0.59999999999999998.
    if(edge.type()==Json::realValue){
        char buffer[64];auto result=std::to_chars(buffer,buffer+sizeof(buffer),edge.asDouble());
        if(result.ec!=std::errc())throw std::runtime_error("Invalid bucket boundary");
        return canonicalNumber(std::string(buffer,result.ptr));
    }
    return canonicalNumber(dump(edge));
}
std::string bucket(const Scalar& value,const std::string& path,const Json::Value& config){
    if(value.type!=ValueType::Number)return value.value;
    const auto& rules=config["bucket_rules"];
    for(auto& key:rules.getMemberNames())if(glob(key,path)){
        const auto& edges=rules[key];if(!edges.isArray()||edges.empty())throw std::runtime_error("Bucket edges must be a nonempty array");
        std::vector<std::string> boundaries;
        for(const auto& edge:edges){
            auto label=numericEdge(edge);
            if(!boundaries.empty()&&decimalCompare(label,boundaries.back())<=0)throw std::runtime_error("Bucket edges must be finite and ascending");
            boundaries.push_back(label);
        }
        std::string low="-inf";
        for(const auto& label:boundaries){if(decimalCompare(value.value,label)<=0)return "("+low+"; "+label+"]";low=label;}
        return "("+low+"; +inf)";
    }
    return value.value;
}

class Normalizer {
    const Json::Value& config;std::string app,source;StringInterner dictionary;std::vector<FlatRow> rows;
    bool excluded(const std::string& path){for(const auto& entry:config["exclude_paths"]){auto x=entry.asString();if(path==x||path.rfind(x+".",0)==0||glob(x,path))return true;}return false;}
    void walk(simdjson::ondemand::value value,const std::string& path,const std::vector<std::string>& keys,size_t depth){
        if(depth>static_cast<size_t>(config.get("max_depth",256).asUInt()))throw std::runtime_error("JSON nesting exceeds max_depth");
        if(excluded(path)) { value.raw_json().value();return; }
        using simdjson::ondemand::json_type;
        auto kind=value.type().value();
        if(kind==json_type::object){
            std::unordered_set<std::string> names;
            for(auto field:value.get_object()){
                auto key=std::string(field.unescaped_key().value());
                if(!names.insert(key).second)throw std::runtime_error("Duplicate JSON member: "+path+"."+key);
                if(key.empty()||key.find('.')!=std::string::npos)throw std::runtime_error("Empty/dotted JSON key requires an extended path contract");
                walk(field.value().value(),path.empty()?key:path+"."+key,keys,depth+1);
            }
        }else if(kind==json_type::array){
            const auto& rule=config["array_rules"][path];
            if(rule.isNull())throw std::runtime_error("Configure array_rules for path: "+path);
            auto mode=rule.isString()?rule.asString():rule.get("mode","").asString();
            std::unordered_map<std::string,size_t> seen;size_t index=0;
            for(auto element:value.get_array()){
                std::string key;
                if(mode=="field"||mode=="BY_FIELD"){
                    std::string raw(element.raw_json().value());std::vector<std::string> parts;
                    if(!rule["fields"].isArray()||rule["fields"].empty())throw std::runtime_error("BY_FIELD requires fields");
                    for(auto& name:rule["fields"])parts.push_back(identity(fieldScalar(raw,name.asString())));
                    key=join(parts,"~");if(seen[key]++)throw std::runtime_error("Duplicate array key: "+path+" = "+key);
                    auto nestedKeys=keys;nestedKeys.push_back(key);simdjson::ondemand::parser p;auto data=simdjson::padded_string(raw);auto doc=p.iterate(data);walk(doc.get_value().value(),path,nestedKeys,depth+1);
                }else if(mode=="index"||mode=="BY_INDEX"){
                    auto nestedKeys=keys;nestedKeys.push_back(std::to_string(index));walk(element.value(),path,nestedKeys,depth+1);
                }else if(mode=="value"||mode=="BY_VALUE"){
                    auto val=scalar(element.value());key=identity(val);auto occurrence=seen[key]++;
                    if(rule.isObject()&&rule.get("allow_duplicates",false).asBool())key+="@"+std::to_string(occurrence);
                    else if(occurrence)throw std::runtime_error("Duplicate scalar array key: "+path);
                    auto nestedKeys=keys;nestedKeys.push_back(key);emit(val,path,nestedKeys);
                }else throw std::runtime_error("Unknown array mode: "+mode);
                ++index;
            }
        }else emit(scalar(value),path,keys);
    }
    void emit(const Scalar& val,const std::string& path,const std::vector<std::string>& keys){
        if(rows.size()>=config.get("max_rows_per_app",10000).asUInt())throw std::runtime_error("Normalized response exceeds max_rows_per_app");
        rows.push_back({dictionary.intern(app),dictionary.intern(path),dictionary.intern(join(keys,"|")),dictionary.intern(val.value),dictionary.intern(bucket(val,path,config)),val.type,source=="AFTER"});
    }
public:
    Normalizer(const Json::Value& cfg,std::string id,std::string src):config(cfg),app(std::move(id)),source(std::move(src)){}
    Json::Value run(const std::string& raw){
        if(source!="BEFORE"&&source!="AFTER")throw std::runtime_error("SOURCE must be BEFORE or AFTER");
        if(raw.size()>config.get("max_json_bytes",10*1024*1024).asUInt())throw std::runtime_error("JSON exceeds max_json_bytes");
        simdjson::ondemand::parser parser;auto padded=simdjson::padded_string(raw);auto doc=parser.iterate(padded);
        if(doc.type().value()!=simdjson::ondemand::json_type::object)throw std::runtime_error("Response must be a JSON object");
        walk(doc.get_value().value(),"",{},0);
        std::sort(rows.begin(),rows.end(),[&](const auto& x,const auto& y){return std::tie(dictionary.get(x.path),dictionary.get(x.arrayKey)) < std::tie(dictionary.get(y.path),dictionary.get(y.arrayKey));});
        Json::Value result(Json::arrayValue);
        for(auto& row:rows){auto out=structure(dictionary.get(row.path));out["APP_ID"]=dictionary.get(row.app);out["ARRAY_KEY"]=dictionary.get(row.arrayKey);out["SOURCE"]=source;out["VALUE"]=dictionary.get(row.value);out["ADJUSTED_VALUE"]=dictionary.get(row.adjusted);out["VALUE_TYPE"]=typeName(row.type);result.append(out);}
        validateNormalized(result,app,source);return result;
    }
};

std::string normalizedKey(const Json::Value& row){Json::Value k(Json::arrayValue);for(auto name:{"APP_ID","PATH","ARRAY_KEY"})k.append(row[name]);return dump(k);}
std::vector<FlatRow> nativeRows(const Json::Value& rows,StringInterner& strings){
    std::vector<FlatRow> out;out.reserve(rows.size());
    for(auto& row:rows){auto t=parseType(row["VALUE_TYPE"].asString());auto value=row["VALUE"].asString();if(t==ValueType::Number)value=canonicalNumber(value);out.push_back({strings.intern(row["APP_ID"].asString()),strings.intern(row["PATH"].asString()),strings.intern(row["ARRAY_KEY"].asString()),strings.intern(value),strings.intern(row["ADJUSTED_VALUE"].asString()),t,row["SOURCE"].asString()=="AFTER"});}
    std::sort(out.begin(),out.end(),[](const auto&a,const auto&b){return std::tie(a.app,a.path,a.arrayKey)<std::tie(b.app,b.path,b.arrayKey);});return out;
}
}

std::string canonicalNumber(const std::string& input){
    auto text=trim(input);
    if(text.empty()||text.size()>100000)throw std::runtime_error("Number exceeds canonical size limit");
    size_t cursor=text[0]=='-'?1:0;
    auto digit=[&](size_t p){return p<text.size()&&text[p]>='0'&&text[p]<='9';};
    auto invalid=[&](){throw std::runtime_error("Invalid JSON number: "+text.substr(0,80));};
    if(!digit(cursor))invalid();
    if(text[cursor]=='0')++cursor;else while(digit(cursor))++cursor;
    if(cursor<text.size()&&text[cursor]=='.'){++cursor;if(!digit(cursor))invalid();while(digit(cursor))++cursor;}
    if(cursor<text.size()&&(text[cursor]=='e'||text[cursor]=='E')){++cursor;if(cursor<text.size()&&(text[cursor]=='+'||text[cursor]=='-'))++cursor;if(!digit(cursor))invalid();while(digit(cursor))++cursor;}
    if(cursor!=text.size())invalid();
    bool negative=text[0]=='-';if(negative)text.erase(0,1);long long exponent=0;auto pos=text.find_first_of("eE");
    if(pos!=std::string::npos){try{exponent=std::stoll(text.substr(pos+1));}catch(...){throw std::runtime_error("Number exponent exceeds resource limit");}text.resize(pos);}
    if(exponent < -100000 || exponent > 100000)throw std::runtime_error("Number exponent exceeds resource limit");
    pos=text.find('.');if(pos!=std::string::npos){exponent-=static_cast<long long>(text.size()-pos-1);text.erase(pos,1);}
    auto first=text.find_first_not_of('0');if(first==std::string::npos)return "0";text.erase(0,first);
    while(text.size()>1&&text.back()=='0'){text.pop_back();++exponent;}
    if(std::abs(exponent)>100000||text.size()>100000)throw std::runtime_error("Number exceeds canonical size limit");
    if(exponent>=0)text.append(static_cast<size_t>(exponent),'0');
    else {auto decimal=static_cast<long long>(text.size())+exponent;if(decimal>0)text.insert(static_cast<size_t>(decimal),1,'.');else text="0."+std::string(static_cast<size_t>(-decimal),'0')+text;}
    return negative?"-"+text:text;
}

Json::Value defaultConfig(){return parse(R"JSON({
 "exclude_paths":["appData.request_id","appData.createDateTime"],
 "array_rules":{
  "decisionResult.available_offers":{"mode":"field","fields":["product_id","type"]},
  "decisionResult.product_trials":{"mode":"field","fields":["product_id"]},
  "decisionResult.product_trials.rules":{"mode":"field","fields":["name"]},
  "decisionResult.product_trials.scoring.scoreModels":{"mode":"field","fields":["modelName"]},
  "decisionResult.scoring":{"mode":"field","fields":["modelName"]},
  "decisionResult.scoring.value":{"mode":"index"},
  "ext_data":{"mode":"field","fields":["body.action"]},
  "product_catalog":{"mode":"field","fields":["mfo_id","product_id"]},
  "tariff_matrix":{"mode":"field","fields":["mfo_id","product_id"]}
 },
 "bucket_rules":{"*.value":[0,0.3,0.6,1],"*.features.sumActiveDeals":[0,10000,50000,100000,250000,1000000],"*score_okb":[0,400,600,700,800,1000],"*score_equ":[0,400,600,700,800,1000]},
 "aggregate_dimensions":["PREFIX","CLASS","SUFFIX","ARRAY_KEY","KEY","PATH","VALUE_ADJUSTED","SOURCE","DIFF_FLAG","CHANGE_TYPE","PRODUCTS","PARTNER_TYPE","ROUTING_METHOD","DECISION_CHANGED"],
 "max_json_bytes":10485760,"max_rows_per_app":10000,"max_depth":256
})JSON");}

Json::Value normalize(const std::string& raw,const std::string& appId,const std::string& source,const Json::Value& config){return Normalizer(config,appId,source).run(raw);}

void validateNormalized(const Json::Value& rows,const std::string& appId,const std::string& source){
    if(!rows.isArray())throw std::runtime_error("normalize must return an array");std::unordered_set<std::string> keys;
    for(auto& row:rows){for(auto name:{"APP_ID","SOURCE","PREFIX","CLASS","SUFFIX","ARRAY_KEY","KEY","PATH","VALUE","ADJUSTED_VALUE","VALUE_TYPE"})if(!row[name].isString())throw std::runtime_error(std::string("Normalized string missing: ")+name);
        if(!appId.empty()&&row["APP_ID"].asString()!=appId)throw std::runtime_error("Normalized APP_ID mismatch");
        auto src=row["SOURCE"].asString();if((src!="BEFORE"&&src!="AFTER")||(!source.empty()&&src!=source))throw std::runtime_error("Normalized SOURCE mismatch");
        auto type=parseType(row["VALUE_TYPE"].asString());auto value=row["VALUE"].asString();if(type==ValueType::Number)canonicalNumber(value);if(type==ValueType::Null&&value!="NULL")throw std::runtime_error("Null VALUE must be NULL");if(type==ValueType::Boolean&&value!="true"&&value!="false")throw std::runtime_error("Invalid boolean VALUE");
        if(!keys.insert(src+normalizedKey(row)).second)throw std::runtime_error("Duplicate normalized attribute");
    }
}

Json::Value compare(const Json::Value& beforeRows,const Json::Value& afterRows,const Json::Value&){
    validateNormalized(beforeRows,"","BEFORE");validateNormalized(afterRows,"","AFTER");StringInterner strings;
    auto before=nativeRows(beforeRows,strings),after=nativeRows(afterRows,strings);std::vector<ComparisonRow> compared;compared.reserve(before.size()+after.size());size_t b=0,a=0;
    auto key=[](const FlatRow&r){return std::tuple{r.app,r.path,r.arrayKey};};
    while(b<before.size()||a<after.size()){
        ComparisonRow out;
        if(a==after.size()||(b<before.size()&&key(before[b])<key(after[a]))){out.before=before[b++];out.hasBefore=true;out.change=3;}
        else if(b==before.size()||key(after[a])<key(before[b])){out.after=after[a++];out.hasAfter=true;out.change=2;}
        else {out.before=before[b++];out.after=after[a++];out.hasBefore=out.hasAfter=true;out.change=out.before.type==out.after.type&&out.before.value==out.after.value?0:1;}
        const auto&r=out.hasBefore?out.before:out.after;out.app=r.app;out.path=r.path;out.arrayKey=r.arrayKey;compared.push_back(out);
    }
    std::sort(compared.begin(),compared.end(),[&](const auto& x,const auto& y){return std::tie(strings.get(x.app),strings.get(x.path),strings.get(x.arrayKey)) < std::tie(strings.get(y.app),strings.get(y.path),strings.get(y.arrayKey));});
    Json::Value result(Json::arrayValue);const char* names[]={"SAME","VALUE_CHANGED","ATTRIBUTE_NEW","ATTRIBUTE_LOST"};
    for(auto& row:compared){auto out=structure(strings.get(row.path));out["APP_ID"]=strings.get(row.app);out["ARRAY_KEY"]=strings.get(row.arrayKey);out["VALUE_BEFORE"]=row.hasBefore?strings.get(row.before.value):"";out["VALUE_AFTER"]=row.hasAfter?strings.get(row.after.value):"";out["ADJUSTED_BEFORE"]=row.hasBefore?strings.get(row.before.adjusted):"";out["ADJUSTED_AFTER"]=row.hasAfter?strings.get(row.after.adjusted):"";out["PRESENT_BEFORE"]=row.hasBefore;out["PRESENT_AFTER"]=row.hasAfter;out["TYPE_BEFORE"]=row.hasBefore?typeName(row.before.type):"";out["TYPE_AFTER"]=row.hasAfter?typeName(row.after.type):"";out["DIFF_FLAG"]=row.change==0?0:1;out["CHANGE_TYPE"]=names[row.change];result.append(out);}
    return result;
}

void validateComparison(const Json::Value& rows,const std::string& appId){
    if(!rows.isArray())throw std::runtime_error("compare must return an array");std::unordered_set<std::string> seen;
    for(auto& row:rows){for(auto n:{"APP_ID","PREFIX","CLASS","SUFFIX","ARRAY_KEY","KEY","PATH","VALUE_BEFORE","VALUE_AFTER","ADJUSTED_BEFORE","ADJUSTED_AFTER","TYPE_BEFORE","TYPE_AFTER","CHANGE_TYPE"})if(!row[n].isString())throw std::runtime_error(std::string("Comparison string missing: ")+n);
        if(!appId.empty()&&row["APP_ID"].asString()!=appId)throw std::runtime_error("Comparison APP_ID mismatch");
        auto type=row["CHANGE_TYPE"].asString();if(type!="SAME"&&type!="VALUE_CHANGED"&&type!="ATTRIBUTE_NEW"&&type!="ATTRIBUTE_LOST")throw std::runtime_error("Invalid CHANGE_TYPE");
        if(!row["DIFF_FLAG"].isInt()||row["DIFF_FLAG"].asInt()!=(type=="SAME"?0:1))throw std::runtime_error("Invalid DIFF_FLAG");
        if(!row["PRESENT_BEFORE"].isBool()||!row["PRESENT_AFTER"].isBool())throw std::runtime_error("Presence flags must be boolean");
        auto pb=row["PRESENT_BEFORE"].asBool(),pa=row["PRESENT_AFTER"].asBool();if(pb!=(type!="ATTRIBUTE_NEW")||pa!=(type!="ATTRIBUTE_LOST"))throw std::runtime_error("Presence flags disagree with change");
        for(auto side:{"BEFORE","AFTER"}){
            auto t=row[std::string("TYPE_")+side].asString(),v=row[std::string("VALUE_")+side].asString();
            if(row[std::string("PRESENT_")+side].asBool()){
                auto kind=parseType(t);
                if(kind==ValueType::Number)canonicalNumber(v);
                if(kind==ValueType::Null&&v!="NULL")throw std::runtime_error("Null comparison VALUE must be NULL");
                if(kind==ValueType::Boolean&&v!="true"&&v!="false")throw std::runtime_error("Invalid comparison boolean VALUE");
            }else if(!t.empty()||!v.empty()||!row[std::string("ADJUSTED_")+side].asString().empty())throw std::runtime_error("Absent side must have empty value/type fields");
        }
        if(pb&&pa){
            auto bt=parseType(row["TYPE_BEFORE"].asString()),at=parseType(row["TYPE_AFTER"].asString());
            auto bv=row["VALUE_BEFORE"].asString(),av=row["VALUE_AFTER"].asString();
            if(bt==ValueType::Number)bv=canonicalNumber(bv);if(at==ValueType::Number)av=canonicalNumber(av);
            if((bt==at&&bv==av)!=(type=="SAME"))throw std::runtime_error("CHANGE_TYPE disagrees with typed original values");
        }
        if(!seen.insert(normalizedKey(row)).second)throw std::runtime_error("Duplicate comparison attribute");
    }
}

Json::Value segments(const std::string& beforeRaw,const std::string& afterRaw,const std::string& appId,const Json::Value& cfg){
    auto b=normalize(beforeRaw,appId,"BEFORE",cfg),a=normalize(afterRaw,appId,"AFTER",cfg);
    return segmentsFromNormalized(b,a,appId);
}

Json::Value segmentsFromNormalized(const Json::Value& b,const Json::Value& a,const std::string& appId){
    validateNormalized(b,appId,"BEFORE");validateNormalized(a,appId,"AFTER");
    auto first=[](const Json::Value& rows,const std::string& path){for(auto& r:rows)if(r["PATH"].asString()==path)return r["VALUE"].asString();return std::string();};
    auto collect=[](const Json::Value& rows,const std::string& path){std::set<std::string> result;for(auto& r:rows)if(r["PATH"].asString()==path&&!r["VALUE"].asString().empty())result.insert(r["VALUE"].asString());return result;};
    auto combine=[&](const std::string& path){auto values=collect(b,path),other=collect(a,path);values.insert(other.begin(),other.end());return join(std::vector<std::string>(values.begin(),values.end()),";");};
    Json::Value out;out["APP_ID"]=appId;out["DECISION_BEFORE"]=first(b,"decisionResult.decision");out["DECISION_AFTER"]=first(a,"decisionResult.decision");out["DECISION_CHANGED"]=out["DECISION_BEFORE"]==out["DECISION_AFTER"]?0:1;
    out["PRODUCTS"]=combine("decisionResult.product_trials.product_name");out["PRODUCT_IDS"]=combine("decisionResult.product_trials.product_id");if(out["PRODUCTS"].asString().empty())out["PRODUCTS"]=out["PRODUCT_IDS"];
    out["PARTNER_TYPE"]=combine("decisionResult.product_trials.partner_type");out["ROUTING_METHOD"]=combine("decisionResult.product_trials.routing_method");
    for(const auto& side:std::vector<std::pair<std::string,const Json::Value*>>{{"BEFORE",&b},{"AFTER",&a}}){
        auto& rows=*side.second;auto suffix="_"+side.first;
        for(auto& field:std::vector<std::pair<std::string,std::string>>{{"PD_MODEL","scoring.pd.modelName"},{"CUTOFF","cutOff.name"},{"REJECT_REASON","rejectReason"}}){auto values=collect(rows,"decisionResult.product_trials."+field.second);out[field.first+suffix]=join(std::vector<std::string>(values.begin(),values.end()),";");}
        out["SCORE_OKB"+suffix]=first(rows,"characteristics.score_okb");
        // Summary fields above use sorted unique values. Preserve every product's
        // individual context and typed attributes for unambiguous drill-down.
        std::map<std::string,Json::Value> contexts;
        const std::map<std::string,std::string> productFields={
            {"product_id","PRODUCT_ID"},{"product_name","PRODUCT_NAME"},
            {"partner_type","PARTNER_TYPE"},{"routing_method","ROUTING_METHOD"},
            {"scoring.pd.modelName","PD_MODEL"},{"cutOff.name","CUTOFF"},
            {"rejectReason","REJECT_REASON"},{"decision","DECISION"}};
        const std::string prefix="decisionResult.product_trials.";
        for(const auto& r:rows){
            auto path=r["PATH"].asString();if(path.rfind(prefix,0)!=0)continue;
            auto key=r["ARRAY_KEY"].asString();auto productKey=key.substr(0,key.find('|'));
            auto& detail=contexts[productKey];detail["ARRAY_KEY"]=productKey;
            detail["ATTRIBUTES"].append(r);
            auto f=productFields.find(path.substr(prefix.size()));
            if(f!=productFields.end())detail[f->second]=r["VALUE"];
        }
        out["PRODUCT_CONTEXT"+suffix]=Json::arrayValue;
        for(auto& [key,detail]:contexts)out["PRODUCT_CONTEXT"+suffix].append(std::move(detail));
        std::map<std::string,std::string> products;std::map<std::string,long double> ranks;
        for(auto& r:rows){auto path=r["PATH"].asString(),key=r["ARRAY_KEY"].asString();if(path=="decisionResult.available_offers.product_id")products[key]=r["VALUE"].asString();if(path=="decisionResult.available_offers.rank")ranks[key]=std::stold(r["VALUE"].asString());}
        std::string top;long double rank=std::numeric_limits<long double>::infinity();for(auto& [key,product]:products){auto val=ranks.count(key)?ranks[key]:std::numeric_limits<long double>::max();if(top.empty()||val<rank){rank=val;top=product;}}out["TOP_OFFER"+suffix]=top;
    }
    // Only two boolean flags are needed here. Merge typed native rows without
    // materializing a second full comparison JSON for every application.
    StringInterner dictionary;auto before=nativeRows(b,dictionary),after=nativeRows(a,dictionary);
    size_t bi=0,ai=0;bool changed=false,external=false;
    auto key=[](const FlatRow& row){return std::tuple{row.app,row.path,row.arrayKey};};
    while(bi<before.size()||ai<after.size()){
        const FlatRow* row=nullptr;bool different=true;
        if(ai==after.size()||(bi<before.size()&&key(before[bi])<key(after[ai])))row=&before[bi++];
        else if(bi==before.size()||key(after[ai])<key(before[bi]))row=&after[ai++];
        else{row=&before[bi];different=before[bi].type!=after[ai].type||before[bi].value!=after[ai].value;++bi;++ai;}
        if(different){changed=true;const auto& path=dictionary.get(row->path);if(path.rfind("characteristics.",0)==0||path.rfind("ext_data.",0)==0){external=true;break;}}
    }
    out["CHANGED"]=changed?1:0;out["EXTERNAL_CHANGED"]=external?1:0;out["INPUT_CHANGED"]=external?1:0;return out;
}

Json::Value aggregate(const Json::Value& comparisons,const Json::Value& segmentRows,const Json::Value& config){
    validateComparison(comparisons);std::unordered_map<std::string,const Json::Value*> segmentsByApp;
    if(!segmentRows.isArray())throw std::runtime_error("Segments must be an array");
    for(auto& s:segmentRows){if(!s["APP_ID"].isString()||!segmentsByApp.emplace(s["APP_ID"].asString(),&s).second)throw std::runtime_error("Invalid or duplicate segment APP_ID");}
    auto dims=config.isMember("aggregate_dimensions")?config["aggregate_dimensions"]:defaultConfig()["aggregate_dimensions"];
    if(!dims.isArray()||dims.empty())throw std::runtime_error("aggregate_dimensions must be a nonempty array");
    std::set<std::string> dimensions;
    for(const auto& d:dims){if(!d.isString()||!dimensions.insert(d.asString()).second)throw std::runtime_error("Aggregate dimensions must be unique strings");}
    StringInterner strings;std::unordered_map<std::string,AggregateFact> groups;
    for(auto& row:comparisons){auto app=row["APP_ID"].asString();auto seg=segmentsByApp.find(app);if(seg==segmentsByApp.end())throw std::runtime_error("Missing application segment: "+app);
        for(const std::string side:{"BEFORE","AFTER"}){
            Json::Value chosen;
            // Project requested dimensions directly. Copying the whole segment
            // for each attribute would repeatedly clone all product details.
            for(const auto& d:dims){
                auto key=d.asString();
                if(key=="SOURCE")chosen[key]=side;
                else if(key=="VALUE_ADJUSTED"||key=="ADJUSTED_VALUE")chosen[key]=row["PRESENT_"+side].asBool()?row["ADJUSTED_"+side]:Json::Value("(отсутствует)");
                else if(key=="VALUE")chosen[key]=row["PRESENT_"+side].asBool()?row["VALUE_"+side]:Json::Value("(отсутствует)");
                else if(key=="VALUE_TYPE")chosen[key]=row["TYPE_"+side];
                else if(seg->second->isMember(key))chosen[key]=(*seg->second)[key];
                else if(row.isMember(key))chosen[key]=row[key];
                else throw std::runtime_error("Unsupported aggregate dimension: "+key);
            }
            auto key=dump(chosen);auto& group=groups[key];group.dimensions=std::move(chosen);group.applications.push_back(strings.intern(app));
        }
    }
    std::vector<std::string> keys;for(auto& pair:groups)keys.push_back(pair.first);std::sort(keys.begin(),keys.end());Json::Value result(Json::arrayValue);size_t index=0;
    for(auto& key:keys){auto& group=groups[key];auto& ids=group.applications;std::sort(ids.begin(),ids.end());ids.erase(std::unique(ids.begin(),ids.end()),ids.end());auto fact=group.dimensions;fact["CNT"]=Json::UInt64(ids.size());fact["counts"]=Json::UInt64(ids.size());fact["id"]=Json::UInt64(++index);std::vector<std::string> appIds;for(auto id:ids)appIds.push_back(strings.get(id));std::sort(appIds.begin(),appIds.end());fact["APP_IDS"]=Json::arrayValue;for(auto& id:appIds)fact["APP_IDS"].append(id);result.append(fact);}
    return result;
}
void validateFacts(const Json::Value& facts){if(!facts.isArray())throw std::runtime_error("aggregate must return an array");for(auto& fact:facts){if(!fact["APP_IDS"].isArray()||!fact["CNT"].isUInt64())throw std::runtime_error("Fact requires APP_IDS and integer CNT");std::set<std::string> ids;for(auto& id:fact["APP_IDS"]){if(!id.isString())throw std::runtime_error("APP_IDS must be strings");ids.insert(id.asString());}if(ids.size()!=fact["APP_IDS"].size()||ids.size()!=fact["CNT"].asUInt64())throw std::runtime_error("CNT must equal unique APP_IDS without duplicate members");if(fact.isMember("counts")&&(!fact["counts"].isUInt64()||fact["counts"].asUInt64()!=fact["CNT"].asUInt64()))throw std::runtime_error("counts must equal CNT");if(fact.isMember("SOURCE")&&fact["SOURCE"]!="BEFORE"&&fact["SOURCE"]!="AFTER")throw std::runtime_error("Invalid fact SOURCE");}}
}
