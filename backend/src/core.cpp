#include "core.hpp"
#include "pipeline_profile.hpp"
#include <simdjson.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <charconv>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
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
bool glob(std::string_view pattern,std::string_view value) { size_t p=0,v=0,star=std::string::npos,mark=0;while(v<value.size()){if(p<pattern.size()&&(pattern[p]=='?'||pattern[p]==value[v])){++p;++v;}else if(p<pattern.size()&&pattern[p]=='*'){star=p++;mark=v;}else if(star!=std::string::npos){p=star+1;v=++mark;}else return false;}while(p<pattern.size()&&pattern[p]=='*')++p;return p==pattern.size();}
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
// Canonical decimals have no redundant leading or fractional trailing zeros.
// With equal integer widths their magnitudes therefore sort lexicographically.
int decimalCompare(std::string_view left, std::string_view right) {
    bool negative = left.front() == '-';
    if (negative != (right.front() == '-')) return negative ? -1 : 1;
    if (negative) { left.remove_prefix(1); right.remove_prefix(1); }
    auto integerWidth = [](std::string_view number) {
        return std::min(number.size(), number.find('.'));
    };
    auto a = integerWidth(left), b = integerWidth(right);
    int order = a < b ? -1 : a > b ? 1 : left.compare(right);
    // compare() only promises the sign of its result.
    if (order == 0) return 0;
    return (order < 0) != negative ? -1 : 1;
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
class BucketRules {
    struct Rule {
        std::string pattern;
        const Json::Value& definition;
        std::vector<std::string> boundaries;
    };
    const Json::Value& definitions;
    std::optional<std::vector<Rule>> rules;
    // Path IDs belong to this document's dictionary. A null match also saves
    // a repeated glob scan for numeric attributes outside all intervals.
    std::unordered_map<uint32_t, const Rule*> byPath;

    const Rule* resolve(const std::string& path) {
        if (!rules) {
            rules.emplace();
            rules->reserve(definitions.size());
            for (const auto& pattern : definitions.getMemberNames())
                rules->push_back({pattern, definitions[pattern], {}});
        }
        for (auto& rule : *rules) {
            if (!glob(rule.pattern, path)) continue;
            if (rule.boundaries.empty()) {
                if (!rule.definition.isArray() || rule.definition.empty())
                    throw std::runtime_error("Bucket edges must be a nonempty array");
                rule.boundaries.reserve(rule.definition.size());
                for (const auto& edge : rule.definition) {
                    auto label = numericEdge(edge);
                    if (!rule.boundaries.empty() && decimalCompare(label, rule.boundaries.back()) <= 0)
                        throw std::runtime_error("Bucket edges must be finite and ascending");
                    rule.boundaries.push_back(std::move(label));
                }
            }
            return &rule;
        }
        return nullptr;
    }
public:
    explicit BucketRules(const Json::Value& config) : definitions(config["bucket_rules"]) {}

    std::optional<std::string> adjusted(const Scalar& value, uint32_t pathId, const std::string& path) {
        auto [entry, inserted] = byPath.try_emplace(pathId, nullptr);
        if (inserted) entry->second = resolve(path);
        if (!entry->second) return std::nullopt;
        const auto& edges = entry->second->boundaries;
        auto high = std::lower_bound(edges.begin(), edges.end(), value.value,
            [](const auto& edge, const auto& number) { return decimalCompare(edge, number) < 0; });
        std::string label = "(";
        label += high == edges.begin() ? "-inf" : *std::prev(high);
        label += "; ";
        return high == edges.end() ? label + "+inf)" : label + *high + "]";
    }
};

class Normalizer {
    const Json::Value& config;std::string app,source;StringInterner dictionary;std::vector<FlatRow> rows;
    BucketRules buckets;
    const uint32_t appKey;
    const bool after;
    Json::UInt maxDepth = 0;
    std::optional<Json::UInt> maxRows;
    bool excluded(const std::string& path){
        for (const auto& entry : config["exclude_paths"]) {
            // String references avoid allocating both a pattern and its dotted
            // prefix at every nested node. Preserve JsonCpp's scalar coercions.
            const char* begin;
            const char* end;
            std::string fallback;
            std::string_view pattern;
            if (entry.getString(&begin, &end)) pattern = std::string_view(begin, end - begin);
            else { fallback = entry.asString(); pattern = fallback; }
            if (path == pattern || (path.size() > pattern.size() && path[pattern.size()] == '.' &&
                                    path.compare(0, pattern.size(), pattern) == 0) ||
                glob(pattern, path)) return true;
        }
        return false;
    }
    void walk(simdjson::ondemand::value value,const std::string& path,const std::vector<std::string>& keys,size_t depth){
        if(depth>maxDepth)throw std::runtime_error("JSON nesting exceeds max_depth");
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
    void emit(const Scalar& value, const std::string& path, const std::vector<std::string>& keys) {
        if (!maxRows) maxRows = config.get("max_rows_per_app", 10000).asUInt();
        if (rows.size() >= *maxRows) throw std::runtime_error("Normalized response exceeds max_rows_per_app");
        auto pathId = dictionary.intern(path);
        auto valueId = dictionary.intern(value.value);
        auto adjustedId = valueId;
        if (value.type == ValueType::Number) {
            if (auto interval = buckets.adjusted(value, pathId, path)) adjustedId = dictionary.intern(*interval);
        }
        rows.push_back({appKey, pathId, dictionary.intern(join(keys, "|")), valueId, adjustedId, value.type, after});
    }
public:
    Normalizer(const Json::Value& cfg, std::string id, std::string src)
        : config(cfg), app(std::move(id)), source(std::move(src)), buckets(cfg),
          appKey(dictionary.intern(app)), after(source == "AFTER") {}

    Json::Value run(const std::string& raw) {
        if (source != "BEFORE" && source != "AFTER") throw std::runtime_error("SOURCE must be BEFORE or AFTER");
        if (raw.size() > config.get("max_json_bytes", 10*1024*1024).asUInt())
            throw std::runtime_error("JSON exceeds max_json_bytes");
        simdjson::ondemand::parser parser;
        auto padded = simdjson::padded_string(raw);
        auto doc = parser.iterate(padded);
        if (doc.type().value() != simdjson::ondemand::json_type::object)
            throw std::runtime_error("Response must be a JSON object");
        maxDepth = config.get("max_depth", 256).asUInt();
        walk(doc.get_value().value(), "", {}, 0);
        {
            OperationTimer timer(&PipelineProfile::keySort);
            auto key = [&](const FlatRow& row) {
                return std::tie(dictionary.get(row.path), dictionary.get(row.arrayKey));
            };
            std::sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) { return key(a) < key(b); });
        }
        Json::Value result(Json::arrayValue);
        {
            OperationTimer timer(&PipelineProfile::renderRows);
            for (const auto& row : rows) {
                auto out = structure(dictionary.get(row.path));
                out["APP_ID"] = app;
                out["ARRAY_KEY"] = dictionary.get(row.arrayKey);
                out["SOURCE"] = source;
                out["VALUE"] = dictionary.get(row.value);
                out["ADJUSTED_VALUE"] = dictionary.get(row.adjusted);
                out["VALUE_TYPE"] = typeName(row.type);
                result.append(std::move(out));
            }
        }
        validateNormalized(result, app, source);
        return result;
    }
};

// These keys live only during a validation pass. Their bytes are owned by the
// immutable input rows, so distinct attributes need no allocation or JSON encoding.
using AttributeIdentity = std::array<std::string_view, 4>;
struct IdentityHash {
    size_t operator()(const AttributeIdentity& key) const noexcept {
        size_t hash = 0;
        for (auto field : key)
            hash ^= std::hash<std::string_view>{}(field) + 0x9e3779b9U + (hash << 6) + (hash >> 2);
        return hash;
    }
};
std::string_view identityField(const Json::Value& field) {
    const char *begin = nullptr, *end = nullptr;
    if (!field.getString(&begin, &end)) throw std::runtime_error("Expected a string in attribute identity");
    return {begin, static_cast<size_t>(end - begin)};
}
AttributeIdentity attributeIdentity(const Json::Value& row, std::string_view source = {}) {
    return {identityField(row["APP_ID"]), identityField(row["PATH"]), identityField(row["ARRAY_KEY"]), source};
}
// Dictionary IDs establish equality; their allocation order need not match the display order.
int compareKeys(const FlatRow& a, const FlatRow& b, const StringInterner& strings) {
    if (a.app != b.app) return strings.get(a.app).compare(strings.get(b.app));
    if (a.path != b.path) return strings.get(a.path).compare(strings.get(b.path));
    if (a.arrayKey != b.arrayKey) return strings.get(a.arrayKey).compare(strings.get(b.arrayKey));
    return 0;
}

std::vector<FlatRow> nativeRows(const Json::Value& rows, StringInterner& strings) {
    std::vector<FlatRow> out;
    out.reserve(rows.size());
    for (const auto& row : rows) {
        auto type = parseType(row["VALUE_TYPE"].asString());
        auto value = row["VALUE"].asString();
        if (type == ValueType::Number) value = canonicalNumber(value);
        out.push_back({strings.intern(row["APP_ID"].asString()), strings.intern(row["PATH"].asString()),
                       strings.intern(row["ARRAY_KEY"].asString()), strings.intern(value),
                       strings.intern(row["ADJUSTED_VALUE"].asString()), type, row["SOURCE"] == "AFTER"});
    }
    {
        OperationTimer timer(&PipelineProfile::keySort);
        auto less = [&](const FlatRow& a, const FlatRow& b) { return compareKeys(a, b, strings) < 0; };
        if (!std::is_sorted(out.begin(), out.end(), less)) std::sort(out.begin(), out.end(), less);
    }
    return out;
}

template<typename Visitor>
void mergeNativeRows(const std::vector<FlatRow>& before, const std::vector<FlatRow>& after,
                     const StringInterner& strings, Visitor visit) {
    size_t b = 0, a = 0;
    while (b < before.size() || a < after.size()) {
        int order;
        if (b == before.size()) order = 1;
        else if (a == after.size()) order = -1;
        else order = compareKeys(before[b], after[a], strings);
        const FlatRow* left = order <= 0 ? &before[b++] : nullptr;
        const FlatRow* right = order >= 0 ? &after[a++] : nullptr;
        bool same = left && right && left->type == right->type && left->value == right->value;
        if (!visit(left, right, same)) break;
    }
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
    if(!rows.isArray())throw std::runtime_error("normalize must return an array");
    std::unordered_set<AttributeIdentity, IdentityHash> keys;
    keys.reserve(rows.size());
    for(auto& row:rows){for(auto name:{"APP_ID","SOURCE","PREFIX","CLASS","SUFFIX","ARRAY_KEY","KEY","PATH","VALUE","ADJUSTED_VALUE","VALUE_TYPE"})if(!row[name].isString())throw std::runtime_error(std::string("Normalized string missing: ")+name);
        if(!appId.empty()&&row["APP_ID"].asString()!=appId)throw std::runtime_error("Normalized APP_ID mismatch");
        auto src=row["SOURCE"].asString();if((src!="BEFORE"&&src!="AFTER")||(!source.empty()&&src!=source))throw std::runtime_error("Normalized SOURCE mismatch");
        auto type=parseType(row["VALUE_TYPE"].asString());auto value=row["VALUE"].asString();if(type==ValueType::Number)canonicalNumber(value);if(type==ValueType::Null&&value!="NULL")throw std::runtime_error("Null VALUE must be NULL");if(type==ValueType::Boolean&&value!="true"&&value!="false")throw std::runtime_error("Invalid boolean VALUE");
        if(!keys.insert(attributeIdentity(row, identityField(row["SOURCE"]))).second)throw std::runtime_error("Duplicate normalized attribute");
    }
}

namespace {
struct ComparisonSide {
    Json::Value value{Json::StaticString("")};
    Json::Value adjusted{Json::StaticString("")};
    Json::Value type{Json::StaticString("")};
    bool present = false;
};

void appendComparison(Json::Value& result, Json::Value out, ComparisonSide before, ComparisonSide after) {
    bool same = before.present && after.present && before.type == after.type && before.value == after.value;
    out[Json::StaticString("VALUE_BEFORE")] = std::move(before.value);
    out[Json::StaticString("VALUE_AFTER")] = std::move(after.value);
    out[Json::StaticString("ADJUSTED_BEFORE")] = std::move(before.adjusted);
    out[Json::StaticString("ADJUSTED_AFTER")] = std::move(after.adjusted);
    out[Json::StaticString("TYPE_BEFORE")] = std::move(before.type);
    out[Json::StaticString("TYPE_AFTER")] = std::move(after.type);
    out[Json::StaticString("PRESENT_BEFORE")] = before.present;
    out[Json::StaticString("PRESENT_AFTER")] = after.present;
    out[Json::StaticString("DIFF_FLAG")] = same ? 0 : 1;
    const char* change;
    if (!before.present) change = "ATTRIBUTE_NEW";
    else if (!after.present) change = "ATTRIBUTE_LOST";
    else change = same ? "SAME" : "VALUE_CHANGED";
    out[Json::StaticString("CHANGE_TYPE")] = Json::StaticString(change);
    result.append(std::move(out));
}

}

Json::Value compareNormalizedPair(const Json::Value& beforeRows, const Json::Value& afterRows) {
    // Both sides come from native normalize for the same APP_ID. Their values
    // are canonical and each side is already sorted by (PATH, ARRAY_KEY).
    auto before = beforeRows.begin(), after = afterRows.begin();
    const auto beforeEnd = beforeRows.end(), afterEnd = afterRows.end();
    Json::Value result(Json::arrayValue);
    auto fields = [](const Json::Value* row) -> ComparisonSide {
        if (!row) return {};
        return {(*row)["VALUE"], (*row)["ADJUSTED_VALUE"], (*row)["VALUE_TYPE"], true};
    };
    OperationTimer timer(&PipelineProfile::renderRows);
    while (before != beforeEnd || after != afterEnd) {
        int order;
        if (before == beforeEnd) order = 1;
        else if (after == afterEnd) order = -1;
        else {
            order = identityField((*before)["PATH"]).compare(identityField((*after)["PATH"]));
            if (order == 0)
                order = identityField((*before)["ARRAY_KEY"]).compare(identityField((*after)["ARRAY_KEY"]));
        }
        const Json::Value* left = order <= 0 ? &*before : nullptr;
        const Json::Value* right = order >= 0 ? &*after : nullptr;
        const auto& original = left ? *left : *right;
        Json::Value out(Json::objectValue);
        for (const char* field : {"APP_ID", "PATH", "PREFIX", "CLASS", "SUFFIX", "KEY", "ARRAY_KEY"})
            out[Json::StaticString(field)] = original[field];
        appendComparison(result, std::move(out), fields(left), fields(right));
        if (left) ++before;
        if (right) ++after;
    }
    return result;
}

Json::Value compare(const Json::Value& beforeRows, const Json::Value& afterRows, const Json::Value&) {
    validateNormalized(beforeRows, "", "BEFORE");
    validateNormalized(afterRows, "", "AFTER");
    StringInterner strings;
    auto before = nativeRows(beforeRows, strings);
    auto after = nativeRows(afterRows, strings);
    Json::Value result(Json::arrayValue);
    OperationTimer timer(&PipelineProfile::renderRows);
    auto fields = [&](const FlatRow* row) -> ComparisonSide {
        if (!row) return {};
        return {strings.get(row->value), strings.get(row->adjusted), typeName(row->type), true};
    };
    mergeNativeRows(before, after, strings, [&](const FlatRow* left, const FlatRow* right, bool) {
        const auto& row = left ? *left : *right;
        auto out = structure(strings.get(row.path));
        out["APP_ID"] = strings.get(row.app);
        out["ARRAY_KEY"] = strings.get(row.arrayKey);
        appendComparison(result, std::move(out), fields(left), fields(right));
        return true;
    });
    return result;
}

void validateComparison(const Json::Value& rows,const std::string& appId){
    if(!rows.isArray())throw std::runtime_error("compare must return an array");
    std::unordered_set<AttributeIdentity, IdentityHash> seen;
    seen.reserve(rows.size());
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
        if(!seen.insert(attributeIdentity(row)).second)throw std::runtime_error("Duplicate comparison attribute");
    }
}

Json::Value segments(const std::string& beforeRaw,const std::string& afterRaw,const std::string& appId,const Json::Value& cfg){
    auto b=normalize(beforeRaw,appId,"BEFORE",cfg),a=normalize(afterRaw,appId,"AFTER",cfg);
    return segmentsFromNormalized(b,a,appId);
}

namespace {
Json::Value segmentFields(const Json::Value& b, const Json::Value& a, const std::string& appId) {
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
    return out;
}

bool externalPath(const std::string& path) {
    return path.starts_with("characteristics.") || path.starts_with("ext_data.");
}

void setChangeFlags(Json::Value& segment, bool changed, bool external) {
    segment["CHANGED"] = changed ? 1 : 0;
    segment["EXTERNAL_CHANGED"] = external ? 1 : 0;
    segment["INPUT_CHANGED"] = external ? 1 : 0;
}
}

Json::Value segmentsFromNormalized(const Json::Value& b, const Json::Value& a, const std::string& appId) {
    validateNormalized(b, appId, "BEFORE");
    validateNormalized(a, appId, "AFTER");
    auto segment = segmentFields(b, a, appId);
    StringInterner strings;
    auto before = nativeRows(b, strings);
    auto after = nativeRows(a, strings);
    bool changed = false, external = false;
    mergeNativeRows(before, after, strings, [&](const FlatRow* left, const FlatRow* right, bool same) {
        if (!same) {
            changed = true;
            external = externalPath(strings.get((left ? left : right)->path));
        }
        return !external;
    });
    setChangeFlags(segment, changed, external);
    return segment;
}

Json::Value segmentsFromNormalized(const Json::Value& b, const Json::Value& a, const std::string& appId,
                                   const Json::Value& comparisons) {
    // Both inputs have been validated by normalize or compare. Segments require
    // all their rows to belong to the requested application.
    if (!appId.empty()) {
        for (const auto* rows : {&b, &a})
            for (const auto& row : *rows)
                if (row["APP_ID"].asString() != appId) throw std::runtime_error("Normalized APP_ID mismatch");
    }
    auto segment = segmentFields(b, a, appId);
    bool changed = false, external = false;
    for (const auto& row : comparisons) {
        if (!row["DIFF_FLAG"].asInt()) continue;
        changed = true;
        if (externalPath(row["PATH"].asString())) { external = true; break; }
    }
    setChangeFlags(segment, changed, external);
    return segment;
}

Json::Value aggregateBatches(const ComparisonSource& produce, const Json::Value& segmentRows,
                             const Json::Value& config) {
    if (!segmentRows.isArray()) throw std::runtime_error("Segments must be an array");
    std::unordered_map<std::string, const Json::Value*> segmentsByApp;
    segmentsByApp.reserve(segmentRows.size());
    for (const auto& segment : segmentRows) {
        const auto& app = segment["APP_ID"];
        if (!app.isString() || !segmentsByApp.emplace(app.asString(), &segment).second)
            throw std::runtime_error("Invalid or duplicate segment APP_ID");
    }

    auto dims = config.isMember("aggregate_dimensions")
        ? config["aggregate_dimensions"] : defaultConfig()["aggregate_dimensions"];
    if (!dims.isArray() || dims.empty())
        throw std::runtime_error("aggregate_dimensions must be a nonempty array");
    std::vector<std::string> dimensions;
    dimensions.reserve(dims.size());
    std::unordered_set<std::string> seenDimensions;
    for (const auto& dim : dims) {
        if (!dim.isString() || !seenDimensions.insert(dim.asString()).second)
            throw std::runtime_error("Aggregate dimensions must be unique strings");
        dimensions.push_back(dim.asString());
    }

    struct Side {
        const char *name, *present, *adjusted, *value, *type;
    };
    constexpr Side sides[] = {
        {"BEFORE", "PRESENT_BEFORE", "ADJUSTED_BEFORE", "VALUE_BEFORE", "TYPE_BEFORE"},
        {"AFTER",  "PRESENT_AFTER",  "ADJUSTED_AFTER",  "VALUE_AFTER",  "TYPE_AFTER"},
    };
    const Json::Value missing("(отсутствует)");
    StringInterner strings;
    std::unordered_map<std::string, AggregateFact> groups;
    produce([&](const Json::Value& comparisons) {
        validateComparison(comparisons);
        for (const auto& row : comparisons) {
            auto app = row["APP_ID"].asString();
            auto segment = segmentsByApp.find(app);
            if (segment == segmentsByApp.end()) throw std::runtime_error("Missing application segment: " + app);
            auto id = strings.intern(app);
            for (const auto& side : sides) {
                Json::Value chosen;
                // Copy only requested dimensions. Segments may contain entire product detail tables.
                for (const auto& key : dimensions) {
                    if (key == "SOURCE") chosen[key] = side.name;
                    else if (key == "VALUE_ADJUSTED" || key == "ADJUSTED_VALUE")
                        chosen[key] = row[side.present].asBool() ? row[side.adjusted] : missing;
                    else if (key == "VALUE")
                        chosen[key] = row[side.present].asBool() ? row[side.value] : missing;
                    else if (key == "VALUE_TYPE") chosen[key] = row[side.type];
                    else if (auto field = segment->second->find(key.data(), key.data() + key.size()))
                        chosen[key] = *field;
                    else if (auto field = row.find(key.data(), key.data() + key.size()))
                        chosen[key] = *field;
                    else throw std::runtime_error("Unsupported aggregate dimension: " + key);
                }
                auto [entry, inserted] = groups.try_emplace(dump(chosen));
                auto& group = entry->second;
                if (inserted) group.dimensions = std::move(chosen);
                if (group.applications.empty() || group.applications.back() != id)
                    group.applications.push_back(id);
            }
        }
    });

    // Sort pointers to the stored groups; copying their serialized keys can duplicate large dimensions.
    std::vector<decltype(groups)::value_type*> ordered;
    ordered.reserve(groups.size());
    for (auto& entry : groups) ordered.push_back(&entry);
    std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) { return a->first < b->first; });

    Json::Value result(Json::arrayValue);
    for (auto* entry : ordered) {
        auto& group = entry->second;
        auto& ids = group.applications;
        std::sort(ids.begin(), ids.end(), [&](auto a, auto b) { return strings.get(a) < strings.get(b); });
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        auto fact = std::move(group.dimensions);
        fact["CNT"] = Json::UInt64(ids.size());
        fact["counts"] = fact["CNT"];
        fact["id"] = Json::UInt64(result.size()) + 1;
        fact["APP_IDS"] = Json::arrayValue;
        for (auto id : ids) fact["APP_IDS"].append(strings.get(id));
        result.append(std::move(fact));
    }
    return result;
}

Json::Value aggregate(const Json::Value& comparisons, const Json::Value& segments, const Json::Value& config) {
    return aggregateBatches([&](const ComparisonBatch& accept) { accept(comparisons); }, segments, config);
}
void validateFacts(const Json::Value& facts){if(!facts.isArray())throw std::runtime_error("aggregate must return an array");for(auto& fact:facts){if(!fact["APP_IDS"].isArray()||!fact["CNT"].isUInt64())throw std::runtime_error("Fact requires APP_IDS and integer CNT");std::set<std::string> ids;for(auto& id:fact["APP_IDS"]){if(!id.isString())throw std::runtime_error("APP_IDS must be strings");ids.insert(id.asString());}if(ids.size()!=fact["APP_IDS"].size()||ids.size()!=fact["CNT"].asUInt64())throw std::runtime_error("CNT must equal unique APP_IDS without duplicate members");if(fact.isMember("counts")&&(!fact["counts"].isUInt64()||fact["counts"].asUInt64()!=fact["CNT"].asUInt64()))throw std::runtime_error("counts must equal CNT");if(fact.isMember("SOURCE")&&fact["SOURCE"]!="BEFORE"&&fact["SOURCE"]!="AFTER")throw std::runtime_error("Invalid fact SOURCE");}}
}
