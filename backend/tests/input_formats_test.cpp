#include "input_formats.hpp"
#include "core.hpp"
#include <gtest/gtest.h>
#include <sstream>

namespace {
Json::Value jsonValue(const std::string& text) {
    Json::CharReaderBuilder builder;
    Json::Value value;
    std::string errors;
    std::istringstream stream(text);
    if (!Json::parseFromStream(builder,stream,&value,&errors)) throw std::runtime_error(errors);
    return value;
}
const Json::Value emptyProfile(Json::objectValue);
}

TEST(InputFormats,XmlAndCsvProduceTheSameTypedNativeRowsAsJson) {
    const auto expected = std::string(R"({"appData":{"application_id":"0008"},"decisionResult":{"product_trials":[{"product_id":10,"score":0.123456789123456789123456789},{"product_id":20,"score":null}]},"ok":false})");
    const auto xml = std::string(R"(<response xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"><appData><id>0008</id></appData><decisionResult><product_trials><item id="10"><score>0.123456789123456789123456789</score></item><item id="20"><score xsi:nil="true"/></item></product_trials></decisionResult><ok>false</ok></response>)");
    const auto profile = jsonValue(R"({"root":"response","arrays":{"/decisionResult/product_trials":"item"},"rename":{"/appData/id":"application_id","/decisionResult/product_trials/@id":"product_id"},"types":{"/decisionResult/product_trials/product_id":"number","/decisionResult/product_trials/score":"number","/ok":"boolean"}})");
    const std::string csv =
        "pointer,type,value\r\n"
        "/decisionResult/product_trials/1/score,null,\r\n"
        "/decisionResult/product_trials/0/product_id,number,10\r\n"
        "/appData/application_id,string,0008\r\n"
        "/decisionResult/product_trials/0/score,number,0.123456789123456789123456789\r\n"
        "/ok,boolean,false\r\n"
        "/decisionResult/product_trials/1/product_id,number,20\r\n";
    auto config = lens::defaultConfig();
    const auto normalized = lens::normalize(expected,"0008","BEFORE",config);
    EXPECT_EQ(lens::normalize(lens::readInputDocument("xml",xml,profile),"0008","BEFORE",config),normalized);
    EXPECT_EQ(lens::normalize(lens::readInputDocument("csv",csv,emptyProfile),"0008","BEFORE",config),normalized);
    EXPECT_EQ(lens::readInputDocument("json",expected,emptyProfile),expected);
}

TEST(InputFormats,CsvSupportsQuotedNewlinesEscapedPointersAndExplicitShapes) {
    const std::string data = "\xEF\xBB\xBF" "pointer;type;value\r\n"
        "/appData/application_id;string;007\r\n"
        "/a~1b~0c;string;\" line1;\r\n\"\"line2\"\" \"\r\n"
        "/numeric/0;string;zero\r\n/numeric;object;\r\n"
        "/empty;array;[]\r\n/n;null;\r\n/zero;number;0\r\n/blank;string;\r\n";
    auto doc = jsonValue(lens::readInputDocument("csv", data,jsonValue(R"({"delimiter":";"})")));
    EXPECT_EQ(doc["appData"]["application_id"],"007");
    EXPECT_EQ(doc["a/b~c"].asString()," line1;\r\n\"line2\" ");
    EXPECT_EQ(doc["numeric"]["0"],"zero");
    EXPECT_TRUE(doc["empty"].isArray());
    EXPECT_TRUE(doc["n"].isNull());
    EXPECT_TRUE(doc["zero"].isNumeric());
    EXPECT_TRUE(doc["blank"].isString());
}

TEST(InputFormats,CsvColumnMappingsSupportLongAndRecordLayouts) {
    auto longProfile=jsonValue(R"({"columns":{"pointer":"route","type":"kind","value":"data"}})");
    EXPECT_EQ(jsonValue(lens::readInputDocument("csv","data,kind,route\ntrue,boolean,/ok",longProfile))["ok"],true);
    auto record=jsonValue(R"({"layout":"record","fields":{"id":{"pointer":"/appData/application_id"},"score":{"pointer":"/v/0","type":"number"}}})");
    auto doc=jsonValue(lens::readInputDocument("csv","score,id\n12,002",record));
    EXPECT_EQ(doc["v"][0],12);
    EXPECT_EQ(doc["appData"]["application_id"],"002");
    EXPECT_THROW(lens::readInputDocument("csv","score,id\n12,002\n13,003",record),std::exception);
    EXPECT_THROW(lens::readInputDocument("csv","extra,id\n12,002",record),std::exception);
}

TEST(InputFormats,XmlSupportsNamespaceTypedVectorsAttributesAndEmptyObjects) {
    auto profile=jsonValue(R"({"namespace":"urn:spr","root":"response","arrays":{"/v":"n","/empty":"item"},"item_types":{"/v":"number"},"types":{"/emptyObject":"object","/cost/#text":"number","/absent":"null"}})");
    std::string xml="\xEF\xBB\xBF" R"(<s:response xmlns:s="urn:spr"><s:v><s:n>1.5</s:n></s:v><s:empty/><s:cost unit="RUB">42</s:cost><s:emptyObject/><s:absent/><s:text><![CDATA[<!DOCTYPE literal>]]></s:text></s:response>)";
    auto doc=jsonValue(lens::readInputDocument("xml",xml,profile));
    EXPECT_EQ(doc["v"][0],1.5);
    EXPECT_TRUE(doc["empty"].isArray());
    EXPECT_EQ(doc["cost"]["@unit"],"RUB");
    EXPECT_EQ(doc["cost"]["#text"],42);
    EXPECT_TRUE(doc["emptyObject"].isObject());
    EXPECT_TRUE(doc["absent"].isNull());
    EXPECT_EQ(doc["text"],"<!DOCTYPE literal>");
}

TEST(InputFormats,RejectsMalformedDuplicateSparseAndUnsafeData) {
    for(const std::string body : {"/a,string,x\n/a,string,y", "/a,string,x\n/a/b,string,y",
            "/a,number,01", "/a,number,NaN", "/a,boolean,yes", "/appData/application_id,number,2",
            "/a,array,\n/a/1,string,x", "/a/2,string,x", "/a~Z,string,x",
            "/a,string,\"unfinished", "/a,string,un\"quoted", "/a,string,\"x\"tail", "/a,string,ok,extra"}) {
        SCOPED_TRACE(body);
        EXPECT_THROW(lens::readInputDocument("csv","pointer,type,value\n"+body,emptyProfile),std::exception);
    }
    for (const std::string xml : {"<r><x>a</x><x>b</x></r>","<r><broken></r>","<r><a/>mixed</r>",
            "<!DOCTYPE r SYSTEM 'file:///etc/passwd'><r/>",
            "<!DOCTYPE r [<!ENTITY x SYSTEM 'http://example.invalid/x'>]><r>&x;</r>",
            "<r xmlns='urn:unexpected'/>","<r>&unknown;</r>"}) {
        SCOPED_TRACE(xml);
        EXPECT_THROW(lens::readInputDocument("xml",xml,emptyProfile),std::exception);
    }
    EXPECT_THROW(lens::readInputDocument("csv",std::string("a\0b",3),emptyProfile),std::exception);
    EXPECT_THROW(lens::readInputDocument("xml","<r><x>1</x></r>",jsonValue(R"({"arrays":{"/x":"item"}})")),std::exception);
    EXPECT_THROW(lens::readInputDocument("xml","<r><a/><b/></r>",jsonValue(R"({"rename":{"/b":"a"}})")),std::exception);
    EXPECT_THROW(lens::readInputDocument("json",std::string(10*1024*1024+1,' '),emptyProfile),std::exception);
}

TEST(InputFormats,JsonBomIsRemovedForPythonAndOtherContentIsPreserved) {
    std::string raw=" { \"appData\" : {\"application_id\":\"001\"} }\n";
    EXPECT_EQ(lens::readInputDocument("json","\xEF\xBB\xBF"+raw,emptyProfile),raw);
}

TEST(InputFormats,RegistryIdentifiesSupportedResponseFormats) {
    for (auto format : {"json","xml","csv"}) EXPECT_TRUE(lens::supportsInputFormat(format));
    for (auto format : {"","xls","JSON"}) {
        EXPECT_FALSE(lens::supportsInputFormat(format));
        EXPECT_THROW(lens::readInputDocument(format,"{}",emptyProfile),std::exception);
    }
}
