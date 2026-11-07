#include "exports.hpp"
#include <gtest/gtest.h>
#include <sstream>
#include <stdexcept>

namespace {

TEST(Exports, JsonDocumentRoundTripsNestedRowsAndEmptyResults) {
    Json::Value first(Json::objectValue);
    first["name"] = "строка\n\"&";
    first["related"].append("I-1");
    first["related"].append(Json::Value());
    Json::Value second(Json::objectValue);
    second["count"] = Json::UInt64(9007199254740993ULL);
    second["active"] = false;
    auto format = lens::ExportFormat::json;
    auto text = lens::exportPrefix(format, {}) + lens::exportRow(format, first, {}, true) +
                lens::exportRow(format, second, {}, false) + lens::exportSuffix(format);
    Json::Value parsed;
    std::istringstream stream(text);
    Json::CharReaderBuilder reader;
    std::string error;
    ASSERT_TRUE(Json::parseFromStream(reader, stream, &parsed, &error)) << error;
    ASSERT_EQ(parsed.size(), 2u);
    EXPECT_EQ(parsed[0], first);
    ASSERT_TRUE(parsed[1]["count"].isIntegral());
    EXPECT_EQ(parsed[1]["count"].asUInt64(), Json::UInt64(9007199254740993ULL));
    ASSERT_TRUE(parsed[1]["active"].isBool());
    EXPECT_FALSE(parsed[1]["active"].asBool());

    std::istringstream empty(lens::exportPrefix(format, {}) + lens::exportSuffix(format));
    ASSERT_TRUE(Json::parseFromStream(reader, empty, &parsed, &error)) << error;
    EXPECT_TRUE(parsed.isArray());
    EXPECT_TRUE(parsed.empty());
}

TEST(Exports, XmlEscapesMarkupAndPreservesCharactersOutsideXml10) {
    Json::Value row(Json::objectValue);
    row["a\"&\n"]["x"].append("<tag>&\r\n");
    row["a\"&\n"]["x"].append(Json::Value());
    row["control"] = std::string("before\x01") + "after";
    row[std::string("key\x02")] = true;
    const auto out = lens::exportRow(lens::ExportFormat::xml, row, {}, true);
    EXPECT_NE(out.find("name=\"a&quot;&amp;&#10;\" type=\"object\""), std::string::npos);
    EXPECT_NE(out.find("<item type=\"string\">&lt;tag&gt;&amp;&#13;&#10;</item>"), std::string::npos);
    EXPECT_NE(out.find("<item type=\"null\"/>"), std::string::npos);
    EXPECT_NE(out.find("encoding=\"json\">&quot;before\\u0001after&quot;"), std::string::npos);
    EXPECT_NE(out.find("name-json=\"&quot;key\\u0002&quot;\""), std::string::npos);
    EXPECT_EQ(out.find('\x01'), std::string::npos);
    EXPECT_EQ(out.find('\x02'), std::string::npos);
}

TEST(Exports, CsvQuotesRecordsAndSeparatesMissingNullEmptyAndLiteralMarkers) {
    const std::vector<std::string> columns = {"id","a,b","empty","null","late","literal","members"};
    Json::Value row(Json::objectValue);
    row["id"] = 17;
    row["a,b"] = "quoted \"hello\"\r\nnext";
    row["empty"] = "";
    row["null"] = Json::Value();
    row["literal"] = "\\N";
    row["members"].append("a,b");
    row["members"].append("two");
    auto format = lens::ExportFormat::csv;
    EXPECT_EQ(lens::exportPrefix(format, columns), "id,\"a,b\",empty,null,late,literal,members\r\n");
    EXPECT_EQ(lens::exportRow(format, row, columns, true),
              "17,\"quoted \"\"hello\"\"\r\nnext\",,\\N,\\M,\\\\N,\"[\"\"a,b\"\",\"\"two\"\"]\"\r\n");
    row["late"] = true;
    EXPECT_NE(lens::exportRow(format, row, columns, false).find(",true,"), std::string::npos);
}

TEST(Exports, InvalidArgumentsDoNotYieldPartialRows) {
    EXPECT_THROW(lens::exportFormat("xlsx"), std::invalid_argument);
    EXPECT_THROW(lens::exportRow(lens::ExportFormat::csv, Json::Value(Json::arrayValue), {}, true),
                 std::invalid_argument);
}

} // namespace