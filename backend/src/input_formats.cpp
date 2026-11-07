#include "input_formats.hpp"
#include "core.hpp"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <simdjson.h>
#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace lens {
namespace {

constexpr size_t maxBytes = 10 * 1024 * 1024;
constexpr size_t maxNodes = 100000;
constexpr size_t maxDepth = 256;
constexpr size_t maxColumns = 1024;

[[noreturn]] void fail(const std::string& message) {
    throw std::invalid_argument("Input mapping: " + message);
}

enum class Kind { Unspecified, Object, Array, String, Number, Boolean, Null };

Kind parseKind(const std::string& name) {
    static const std::map<std::string, Kind> kinds {
        {"string",Kind::String},{"number",Kind::Number},{"boolean",Kind::Boolean},
        {"null",Kind::Null},{"object",Kind::Object},{"array",Kind::Array}
    };
    auto found = kinds.find(name);
    if (found == kinds.end()) fail("unknown type '" + name + "'");
    return found->second;
}

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

void options(const Json::Value& profile, std::initializer_list<std::string_view> allowed) {
    if (!profile.isObject() && !profile.isNull()) fail("profile must be an object");
    if (profile.isNull()) return;
    for (const auto& key : profile.getMemberNames())
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            fail("unknown profile option '" + key + "'");
}

std::string option(const Json::Value& profile, const std::string& name, const std::string& fallback) {
    if (!profile.isMember(name)) return fallback;
    if (!profile[name].isString() || profile[name].asString().empty()) fail(name + " must be a nonempty string");
    return profile[name].asString();
}

void mappingObject(const Json::Value& profile, const std::string& name) {
    if (profile.isMember(name) && !profile[name].isObject()) fail(name + " must be an object");
}

std::string childPath(const std::string& parent, const std::string& key) {
    auto path = parent + "/";
    for (char c : key) {
        if (c == '~') path += "~0";
        else if (c == '/') path += "~1";
        else path += c;
    }
    return path;
}

std::vector<std::string> pointerParts(const std::string& path) {
    if (path.empty()) return {};
    if (path.front() != '/') fail("JSON Pointer must begin with '/'");
    std::vector<std::string> result;
    size_t start = 1;
    for (;;) {
        const auto slash = path.find('/', start);
        const auto end = slash == std::string::npos ? path.size() : slash;
        std::string key;
        for (size_t i = start; i < end; ++i) {
            if (path[i] != '~') { key.push_back(path[i]); continue; }
            if (++i == end || (path[i] != '0' && path[i] != '1')) fail("invalid JSON Pointer escape");
            key.push_back(path[i] == '0' ? '~' : '/');
        }
        if (key.empty()) fail("empty JSON member names are unsupported");
        result.push_back(std::move(key));
        if (result.size() > maxDepth) fail("path exceeds nesting limit");
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return result;
}

struct Node {
    Kind kind = Kind::Unspecified;
    std::string value;
    std::map<std::string, Node> children;
};

struct Tree {
    Node root;
    size_t nodes = 1;

    Node& child(Node& parent, const std::string& key) {
        if (key.empty()) fail("empty member name");
        auto [found, inserted] = parent.children.try_emplace(key);
        if (inserted && ++nodes > maxNodes) fail("document exceeds node limit");
        return found->second;
    }

    Node& pointer(const std::string& path) {
        Node* current = &root;
        for (const auto& key : pointerParts(path)) current = &child(*current,key);
        return *current;
    }
};

void setValue(Node& node, Kind kind, const std::string& text, const std::string& path) {
    if (node.kind != Kind::Unspecified) fail("duplicate declaration at '" + path + "'");
    if (path == "/appData/application_id" && kind != Kind::String) fail("application_id must be a string");
    node.kind = kind;
    if (kind == Kind::String) { node.value = text; return; }
    const auto clean = trim(text);
    if (kind == Kind::Number) {
        // Validate grammar and precision bounds. Keep the exact original lexeme,
        // including exponent notation, until the normalizer canonicalizes it.
        canonicalNumber(clean);
        node.value = clean;
    } else if (kind == Kind::Boolean) {
        if (clean != "true" && clean != "false") fail("expected true or false at '" + path + "'");
        node.value = clean;
    } else if (kind == Kind::Null) {
        if (!clean.empty() && clean != "null") fail("invalid null at '" + path + "'");
    } else if (!clean.empty() && clean != (kind == Kind::Array ? "[]" : "{}")) {
        fail("container declarations require an empty value at '" + path + "'");
    }
}

bool indexKey(const std::string& key) {
    return !key.empty() && (key.size() == 1 || key[0] != '0') &&
        std::all_of(key.begin(),key.end(),[](char c){return c >= '0' && c <= '9';});
}

class JsonOutput {
    std::string output;
    void append(std::string_view text) {
        if (text.size() > maxBytes - output.size()) fail("converted JSON exceeds 10 MiB");
        output.append(text);
    }
    void quote(const std::string& text) {
        static constexpr char hex[] = "0123456789abcdef";
        append("\"");
        size_t first = 0;
        for (size_t i = 0; i < text.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (c >= 0x20 && c != '"' && c != '\\') continue;
            append(std::string_view(text).substr(first,i-first));
            if (c == '"') append("\\\"");
            else if (c == '\\') append("\\\\");
            else {
                const char escape[] = {'\\','u','0','0',hex[c>>4],hex[c&15]};
                append(std::string_view(escape,sizeof(escape)));
            }
            first = i+1;
        }
        append(std::string_view(text).substr(first));
        append("\"");
    }
    void write(const Node& node, bool root = false) {
        Kind kind = node.kind;
        if (kind == Kind::Unspecified) {
            // Numbers are array indices only for inferred all-numeric children.
            // Explicit object declarations permit numeric object member names.
            const bool array = !root && !node.children.empty() &&
                std::all_of(node.children.begin(),node.children.end(),[](const auto& entry){return indexKey(entry.first);});
            kind = array ? Kind::Array : Kind::Object;
        }
        if (root && kind != Kind::Object) fail("response root must be an object");
        if (kind != Kind::Object && kind != Kind::Array && !node.children.empty())
            fail("a scalar and its descendants occupy the same JSON Pointer");
        if (kind == Kind::Object) {
            append("{");
            bool first = true;
            for (const auto& [name,child] : node.children) {
                if (!first) append(",");
                first = false;
                quote(name); append(":"); write(child);
            }
            append("}");
        } else if (kind == Kind::Array) {
            append("[");
            for (size_t i = 0; i < node.children.size(); ++i) {
                auto child = node.children.find(std::to_string(i));
                if (child == node.children.end()) fail("array indices must be dense and start at zero");
                if (i) append(",");
                write(child->second);
            }
            append("]");
        } else if (kind == Kind::String) quote(node.value);
        else if (kind == Kind::Null) append("null");
        else append(node.value);
    }
public:
    std::string render(const Node& node) {
        auto appData = node.children.find("appData");
        if (appData != node.children.end()) {
            const auto& children = appData->second.children;
            auto id = children.find("application_id");
            if (id != children.end() && id->second.kind != Kind::String)
                fail("application_id must be a string");
        }
        write(node,true);
        return std::move(output);
    }
};

// Callback receives one complete RFC 4180 record, including quoted newlines.
template<class Function>
void csvRecords(std::string_view data, char delimiter, Function consume) {
    enum class State { Start, Unquoted, Quoted, AfterQuote };
    State state = State::Start;
    std::vector<std::string> record;
    std::string field;
    size_t rows = 0;
    bool pending = false;
    auto finishField = [&] {
        if (record.size() >= maxColumns) fail("CSV column limit exceeded");
        record.push_back(std::move(field)); field.clear(); state = State::Start;
    };
    auto finishRecord = [&] {
        finishField();
        if (++rows > maxNodes + 1) fail("CSV row limit exceeded");
        consume(record,rows); record.clear(); pending = false;
    };
    for (size_t i = 0; i < data.size(); ++i) {
        char c = data[i];
        pending = true;
        if (state == State::Quoted) {
            if (c == '"') state = State::AfterQuote;
            else field += c;
        } else if (c == delimiter) finishField();
        else if (c == '\r' || c == '\n') {
            if (c == '\r' && i+1 < data.size() && data[i+1] == '\n') ++i;
            finishRecord();
        } else if (c == '"' && state == State::AfterQuote) {
            field += '"'; state = State::Quoted;
        } else if (c == '"' && state == State::Start) state = State::Quoted;
        else {
            if (state == State::AfterQuote || c == '"') fail("invalid quoting in CSV record " + std::to_string(rows+1));
            field += c; state = State::Unquoted;
        }
    }
    if (state == State::Quoted) fail("unterminated quoted CSV field");
    if (pending) finishRecord();
    if (rows < 2) fail("CSV requires a header and at least one data record");
}

std::string fromCsv(std::string_view data, const Json::Value& profile) {
    options(profile,{"layout","delimiter","columns","fields"});
    const auto delimiter = option(profile,"delimiter",",");
    if (delimiter.size() != 1 || std::string(",;\t").find(delimiter[0]) == std::string::npos)
        fail("CSV delimiter must be comma, semicolon or tab");
    const auto layout = option(profile,"layout","pointer");
    if (layout != "pointer" && layout != "record") fail("CSV layout must be pointer or record");
    const bool wide = layout == "record";
    if (wide && profile.isMember("columns")) fail("columns is only valid in pointer layout");
    if (!wide && profile.isMember("fields")) fail("fields is only valid in record layout");
    mappingObject(profile,"columns"); mappingObject(profile,"fields");
    if (wide && profile["fields"].empty()) fail("record layout requires fields");
    const auto& mapping = profile["columns"];
    options(mapping,{"pointer","type","value"});
    std::vector<std::string> headings;
    std::vector<std::pair<std::string,Kind>> fields;
    size_t pointer = 0, type = 0, value = 0;
    Tree tree;
    csvRecords(data,delimiter[0],[&](const auto& record,size_t line) {
        if (line == 1) {
            headings = record;
            std::set<std::string> unique(record.begin(),record.end());
            if (unique.size() != record.size() || unique.count("")) fail("duplicate or empty CSV heading");
            if (!wide) {
                auto find = [&](const char* key) {
                    auto name = option(mapping,key,key);
                    auto found = std::find(record.begin(),record.end(),name);
                    if (found == record.end()) fail("missing CSV column '" + name + "'");
                    return static_cast<size_t>(found-record.begin());
                };
                pointer = find("pointer"); type = find("type"); value = find("value");
                if (record.size()!=3 || pointer==type || pointer==value || type==value)
                    fail("pointer layout requires exactly three distinct columns");
            } else {
                if (profile["fields"].size() != record.size()) fail("CSV fields must map every heading");
                for (const auto& heading : record) {
                    const auto& spec = profile["fields"][heading];
                    if (!spec.isObject() || !spec["pointer"].isString()) fail("missing pointer for CSV column '" + heading + "'");
                    options(spec,{"pointer","type"});
                    fields.emplace_back(spec["pointer"].asString(),parseKind(option(spec,"type","string")));
                }
            }
        } else {
            if (record.size() != headings.size()) fail("invalid number of columns in CSV record " + std::to_string(line));
            if (!wide) setValue(tree.pointer(record[pointer]),parseKind(record[type]),record[value],record[pointer]);
            else {
                if (line != 2) fail("record layout expects one application per file");
                for (size_t i = 0; i < fields.size(); ++i)
                    setValue(tree.pointer(fields[i].first),fields[i].second,record[i],fields[i].first);
            }
        }
    });
    return JsonOutput().render(tree.root);
}

std::string xmlString(const xmlChar* value) {
    return value ? reinterpret_cast<const char*>(value) : "";
}

// UTF-8 is checked before this scan, so UTF-16/32 cannot hide a declaration.
// Skip literal bodies, where the text "<!DOCTYPE" has no special meaning.
void rejectDoctype(std::string_view xml) {
    for (size_t pos = xml.find('<'); pos != std::string_view::npos; pos=xml.find('<',pos+1)) {
        auto tail = xml.substr(pos);
        if (tail.starts_with("<!DOCTYPE") || tail.starts_with("<!ENTITY"))
            fail("DTD and entity declarations are forbidden");
        const char* closing = tail.starts_with("<!--") ? "-->" :
                              tail.starts_with("<![CDATA[") ? "]]>" :
                              tail.starts_with("<?") ? "?>" : nullptr;
        if (closing) {
            const auto end = xml.find(closing,pos+2);
            if (end == std::string_view::npos) fail("unterminated XML section");
            pos = end + std::char_traits<char>::length(closing) - 1;
        }
    }
}

class XmlMapping {
    const Json::Value& profile;
    std::string namespaceUri;
    Tree tree;

    void checkElement(xmlNodePtr element) const {
        auto uri = element->ns ? xmlString(element->ns->href) : "";
        if (uri != namespaceUri) fail("unexpected XML namespace at " + xmlString(element->name));
    }

    std::string member(const std::string& parent, const std::string& source) const {
        return option(profile["rename"],childPath(parent,source),source);
    }

    Kind typeAt(const std::string& path, Kind inferred) const {
        const auto& types = profile["types"];
        if (!types.isMember(path)) return inferred;
        auto kind = parseKind(types[path].asString());
        if (path == "/appData/application_id" && kind != Kind::String) fail("application_id must be a string");
        return kind;
    }

    void leaf(const std::string& parent, const std::string& source, const std::string& value, Node& target) {
        const auto name = member(parent,source);
        const auto path = childPath(parent,name);
        setValue(tree.child(target,name),typeAt(path,Kind::String),value,path);
    }

    void read(xmlNodePtr element, Node& target, const std::string& path, size_t depth, bool item=false) {
        if (depth > maxDepth) fail("XML nesting limit exceeded");
        checkElement(element);
        // Every XML member is declared exactly once, including empty strings,
        // empty wrappers, and siblings that collide after renaming.
        if (target.kind != Kind::Unspecified) fail("duplicate XML field '" + path + "'");
        const bool wrapper = !item && profile["arrays"].isMember(path);
        std::vector<xmlNodePtr> elements;
        std::string text;
        for (xmlNodePtr child=element->children; child; child=child->next) {
            switch (child->type) {
                case XML_ELEMENT_NODE: elements.push_back(child); break;
                case XML_TEXT_NODE: case XML_CDATA_SECTION_NODE: text += xmlString(child->content); break;
                case XML_COMMENT_NODE: case XML_PI_NODE: break;
                default: fail("unsupported XML node at '" + path + "'");
            }
        }
        // xsi:nil represents a missing value at a present field, including a
        // nullable number or boolean. An empty ordinary element stays a string.
        auto nilAttr = xmlHasNsProp(element,BAD_CAST "nil",
            BAD_CAST "http://www.w3.org/2001/XMLSchema-instance");
        if (nilAttr) {
            std::unique_ptr<xmlChar,xmlFreeFunc> rawNil(
                xmlNodeListGetString(element->doc,nilAttr->children,1),xmlFree);
            const auto nil = trim(xmlString(rawNil.get()));
            if (nil != "true" && nil != "false" && nil != "1" && nil != "0")
                fail("invalid xsi:nil at '" + path + "'");
            xmlRemoveProp(nilAttr);
            if (nil == "true" || nil == "1") {
                if (!elements.empty() || element->properties || !trim(text).empty())
                    fail("xsi:nil conflicts with element content at '" + path + "'");
                setValue(target,Kind::Null,"",path);
                return;
            }
        }
        if (wrapper) {
            if (typeAt(path,Kind::Array) != Kind::Array || element->properties || !trim(text).empty())
                fail("array wrapper has conflicting content at '" + path + "'");
            target.kind = Kind::Array;
            auto tag = profile["arrays"][path].asString();
            for (size_t i = 0; i < elements.size(); ++i) {
                if (xmlString(elements[i]->name) != tag) fail("expected array item <" + tag + "> at '" + path + "'");
                // The item's semantic path is the wrapper's path. The synthetic
                // index is only a tree key and is never exposed to XML profiles.
                read(elements[i],tree.child(target,std::to_string(i)),path,depth+1,true);
            }
        } else readContents(element,target,path,depth,elements,text,item);
    }

    void readContents(xmlNodePtr element, Node& target, const std::string& path, size_t depth,
                      const std::vector<xmlNodePtr>& elements, const std::string& text, bool item=false) {
        const bool structure = !elements.empty() || element->properties || (path.empty() && !item);
        Kind kind = structure ? Kind::Object : Kind::String;
        if (item) {
            // The wrapper defines its items' scalar types separately so that a
            // vector's numeric items and a singleton/empty vector stay typed.
            if (profile["item_types"].isMember(path)) kind = parseKind(profile["item_types"][path].asString());
        } else kind = typeAt(path,kind);
        if (kind != Kind::Object) {
            if (structure || kind==Kind::Array) fail("XML shape differs from declared type at '" + path + "'");
            setValue(target,kind,text,path);
            return;
        }
        target.kind = Kind::Object;
        if (!elements.empty() && !trim(text).empty()) fail("mixed XML text and elements at '" + path + "'");
        if (!trim(text).empty() && !element->properties) fail("text conflicts with XML object at '" + path + "'");
        for (auto attr=element->properties; attr; attr=attr->next) {
            if (attr->ns) fail("qualified XML attributes require a separate mapping");
            auto value = xmlNodeListGetString(element->doc,attr->children,1);
            std::unique_ptr<xmlChar,xmlFreeFunc> owned(value,xmlFree);
            leaf(path,"@" + xmlString(attr->name),xmlString(value),target);
        }
        if (element->properties && elements.empty() && !text.empty()) leaf(path,"#text",text,target);
        for (auto child : elements) {
            auto name = member(path,xmlString(child->name));
            read(child,tree.child(target,name),childPath(path,name),depth+1);
        }
    }

public:
    explicit XmlMapping(const Json::Value& settings):profile(settings) {
        options(profile,{"root","namespace","arrays","types","item_types","rename"});
        namespaceUri = option(profile,"namespace","");
        for (const auto& name : {"arrays","types","item_types","rename"}) {
            mappingObject(profile,name);
            const auto& mapping = profile[name];
            if (mapping.isNull()) continue;
            for (const auto& path : mapping.getMemberNames()) {
                pointerParts(path);
                if (!mapping[path].isString() || mapping[path].asString().empty()) fail(std::string(name) + " requires nonempty strings");
                if (std::string_view(name) == "types" || std::string_view(name) == "item_types") parseKind(mapping[path].asString());
                if (std::string_view(name) == "item_types" && !profile["arrays"].isMember(path))
                    fail("item_types requires an XML array wrapper at '" + path + "'");
            }
        }
    }
    std::string convert(std::string_view raw) {
        rejectDoctype(raw);
        const auto flags = XML_PARSE_NONET | XML_PARSE_COMPACT | XML_PARSE_NOERROR | XML_PARSE_NOWARNING;
        // No DTD loading, entity substitution, recovery, XInclude processing or
        // XML_PARSE_HUGE. No process-global libxml options are modified.
        auto document = std::unique_ptr<xmlDoc,decltype(&xmlFreeDoc)>(
            xmlReadMemory(raw.data(),static_cast<int>(raw.size()),nullptr,"UTF-8",flags),xmlFreeDoc);
        if (!document || document->intSubset || document->extSubset) fail("invalid XML response or forbidden DTD");
        auto root = xmlDocGetRootElement(document.get());
        if (!root) fail("XML root element missing");
        if (option(profile,"root",xmlString(root->name)) != xmlString(root->name)) fail("unexpected XML root element");
        read(root,tree.root,"",0);
        return JsonOutput().render(tree.root);
    }
};

struct InputAdapter {
    std::string_view name;
    bool validateUtf8;
    std::string (*read)(std::string_view, const Json::Value&);
};

std::string fromJson(std::string_view text, const Json::Value& profile) {
    options(profile,{});
    return std::string(text);
}

std::string fromXml(std::string_view text, const Json::Value& profile) {
    return XmlMapping(profile).convert(text);
}

constexpr std::array adapters {
    InputAdapter{"json", false, fromJson},
    InputAdapter{"xml", true, fromXml},
    InputAdapter{"csv", true, fromCsv}
};

const InputAdapter* findAdapter(std::string_view format) noexcept {
    for (const auto& adapter : adapters)
        if (adapter.name == format) return &adapter;
    return nullptr;
}

} // namespace

bool supportsInputFormat(std::string_view format) noexcept {
    return findAdapter(format) != nullptr;
}

std::string readInputDocument(const std::string& format, const std::string& raw, const Json::Value& profile) {
    if (raw.size() > maxBytes) fail("input exceeds 10 MiB");
    const auto* adapter = findAdapter(format);
    if (!adapter) fail("unknown format '" + format + "'");
    if (adapter->validateUtf8 && (raw.find('\0') != std::string::npos || !simdjson::validate_utf8(raw.data(),raw.size())))
        fail("input must be UTF-8 without NUL bytes");
    std::string_view text(raw);
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    return adapter->read(text,profile);
}

} // namespace lens
