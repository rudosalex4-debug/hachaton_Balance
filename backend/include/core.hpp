#pragma once
#include <json/json.h>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lens {
enum class ValueType : uint8_t { Null, Boolean, Number, String };
struct StringInterner {
    std::vector<std::string> strings;
    std::unordered_map<std::string, uint32_t> ids;
    uint32_t intern(const std::string& value);
    const std::string& get(uint32_t id) const;
};
struct FlatRow {
    uint32_t app, path, arrayKey, value, adjusted;
    ValueType type;
    bool after;
};
struct ComparisonRow {
    uint32_t app, path, arrayKey;
    FlatRow before{}, after{};
    bool hasBefore{}, hasAfter{};
    uint8_t change{}; // SAME, VALUE_CHANGED, ATTRIBUTE_NEW, ATTRIBUTE_LOST
};
struct AggregateFact {
    Json::Value dimensions;
    std::vector<uint32_t> applications;
};
struct ApplicationSegment { uint32_t app; Json::Value fields; };
Json::Value defaultConfig();
std::string canonicalNumber(const std::string& value);
Json::Value normalize(const std::string& raw, const std::string& appId,
                      const std::string& source, const Json::Value& config);
Json::Value compare(const Json::Value& beforeRows, const Json::Value& afterRows,
                    const Json::Value& config);
// Fast path for a single application's two canonical, sorted normalize() outputs.
Json::Value compareNormalizedPair(const Json::Value& beforeRows, const Json::Value& afterRows);
Json::Value segments(const std::string& beforeRaw, const std::string& afterRaw,
                     const std::string& appId, const Json::Value& config = defaultConfig());
// Reuse already normalized native rows; inputs must contain one application's
// BEFORE/AFTER rows. This avoids parsing the same documents a second time.
Json::Value segmentsFromNormalized(const Json::Value& beforeRows, const Json::Value& afterRows,
                                  const std::string& appId);
// comparisons must be the output of compare(beforeRows, afterRows) for this application.
Json::Value segmentsFromNormalized(const Json::Value& beforeRows, const Json::Value& afterRows,
                                   const std::string& appId, const Json::Value& comparisons);
using ComparisonBatch = std::function<void(const Json::Value&)>;
using ComparisonSource = std::function<void(const ComparisonBatch&)>;
// The database source yields disjoint rows with unique comparison identities.
Json::Value aggregateBatches(const ComparisonSource& produce, const Json::Value& segments,
                             const Json::Value& config);
Json::Value aggregate(const Json::Value& comparisons, const Json::Value& segments,
                      const Json::Value& config);
void validateNormalized(const Json::Value& rows, const std::string& appId = "", const std::string& source = "");
void validateComparison(const Json::Value& rows, const std::string& appId = "");
void validateFacts(const Json::Value& facts);
}
