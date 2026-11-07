#include "exports.hpp"

#include <sstream>
#include <stdexcept>
#include <string_view>

namespace lens {
namespace {

std::string compactJson(const Json::Value& value, bool asciiOnly = false) {
    struct Encoder {
        explicit Encoder(bool ascii) {
            Json::StreamWriterBuilder settings;
            settings["indentation"] = "";
            settings["commentStyle"] = "None";
            settings["emitUTF8"] = !ascii;
            writer.reset(settings.newStreamWriter());
        }
        std::unique_ptr<Json::StreamWriter> writer;
        std::ostringstream output;
    };
    thread_local Encoder utf8(false), ascii(true);
    auto& encoder = asciiOnly ? ascii : utf8;
    encoder.output.str({});
    encoder.output.clear();
    if (encoder.writer->write(value, &encoder.output) != 0)
        throw std::runtime_error("Unable to serialize export value");
    return encoder.output.str();
}

bool isXmlText(std::string_view value) {
    // PostgreSQL returns valid UTF-8. XML 1.0 excludes some valid JSON code points.
    for (unsigned char c : value)
        if (c < 32 && c != '\t' && c != '\r' && c != '\n') return false;
    return value.find("\xef\xbf\xbe") == value.npos && value.find("\xef\xbf\xbf") == value.npos;
}

void escapeXml(std::string& out, std::string_view text) {
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            // Character references survive XML attribute and line-end normalization.
            case '\r': out += "&#13;"; break;
            case '\n': out += "&#10;"; break;
            case '\t': out += "&#9;"; break;
            default: out += c;
        }
    }
}

void appendXmlValue(std::string& out, const Json::Value& value, const std::string* name = nullptr) {
    const char* tag = name ? "field" : "item";
    out += '<';
    out += tag;
    if (name) {
        const bool literal = isXmlText(*name);
        out += literal ? " name=\"" : " name-json=\"";
        escapeXml(out, literal ? *name : compactJson(Json::Value(*name), true));
        out += '"';
    }
    if (value.isNull()) {
        out += " type=\"null\"/>";
        return;
    }
    if (value.isObject()) {
        out += " type=\"object\">";
        for (auto it = value.begin(); it != value.end(); ++it) {
            const auto key = it.name();
            appendXmlValue(out, *it, &key);
        }
    } else if (value.isArray()) {
        out += " type=\"array\">";
        for (const auto& item : value) appendXmlValue(out, item);
    } else if (value.isString()) {
        auto text = value.asString();
        if (isXmlText(text)) {
            out += " type=\"string\">";
        } else {
            out += " type=\"string\" encoding=\"json\">";
            text = compactJson(value, true);
        }
        escapeXml(out, text);
    } else {
        out += value.isBool() ? " type=\"boolean\">" : " type=\"number\">";
        out += compactJson(value);
    }
    out += "</";
    out += tag;
    out += '>';
}

void appendCsvField(std::string& out, const std::string& field) {
    if (field.find_first_of(",\"\r\n") == field.npos) {
        out += field;
        return;
    }
    out += '"';
    for (char c : field) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
}

std::string csvValue(const Json::Value& row, const std::string& column) {
    if (!row.isMember(column)) return "\\M";
    const auto& value = row[column];
    if (value.isNull()) return "\\N";
    if (!value.isString()) return compactJson(value);
    auto text = value.asString();
    // Reserve a single leading backslash for null/missing. An empty string
    // remains an ordinary empty CSV field.
    if (!text.empty() && text.front() == '\\') text.insert(text.begin(), '\\');
    return text;
}

} // namespace

ExportFormat exportFormat(const std::string& name) {
    if (name == "json") return ExportFormat::json;
    if (name == "xml") return ExportFormat::xml;
    if (name == "csv") return ExportFormat::csv;
    throw std::invalid_argument("format must be json, xml or csv");
}

std::string exportPrefix(ExportFormat format, const std::vector<std::string>& columns) {
    if (format == ExportFormat::json) return "[";
    if (format == ExportFormat::xml) return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<rows>\n";
    std::string header;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (i) header += ',';
        appendCsvField(header, columns[i]);
    }
    return header + "\r\n";
}

std::string exportRow(ExportFormat format, const Json::Value& row,
                      const std::vector<std::string>& columns, bool firstRow) {
    if (!row.isObject()) throw std::invalid_argument("Export row must be an object");
    if (format == ExportFormat::json) return (firstRow ? "\n" : ",\n") + compactJson(row);
    std::string out;
    if (format == ExportFormat::xml) {
        out = "<row>";
        for (auto it = row.begin(); it != row.end(); ++it) {
            const auto key = it.name();
            appendXmlValue(out, *it, &key);
        }
        return out + "</row>\n";
    }
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (i) out += ',';
        appendCsvField(out, csvValue(row, columns[i]));
    }
    return out + "\r\n";
}

std::string exportSuffix(ExportFormat format) {
    if (format == ExportFormat::json) return "\n]\n";
    if (format == ExportFormat::xml) return "</rows>\n";
    return {};
}

} // namespace lens