#include "export_http.hpp"

#include "exports.hpp"
#include "storage.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <unistd.h>

namespace lens {
namespace {

struct Artifact {
    const char* table;
    const char* ordering;
    std::vector<std::string> emptyColumns;
};

Artifact artifactSpec(const std::string& name) {
    if (name == "normalized") return {"normalized_values", "app_id,source,path,array_key",
        {"APP_ID","SOURCE","PATH","PREFIX","CLASS","SUFFIX","ARRAY_KEY","KEY","VALUE","VALUE_TYPE","ADJUSTED_VALUE"}};
    if (name == "comparisons") return {"comparison_values", "app_id,path,array_key",
        {"APP_ID","PATH","PREFIX","CLASS","SUFFIX","ARRAY_KEY","KEY","VALUE_BEFORE","VALUE_AFTER",
         "TYPE_BEFORE","TYPE_AFTER","PRESENT_BEFORE","PRESENT_AFTER","ADJUSTED_BEFORE","ADJUSTED_AFTER","DIFF_FLAG","CHANGE_TYPE"}};
    if (name == "facts") return {"aggregate_facts", "fact_id::bigint",
        {"FACT_ID","CNT","counts","APP_IDS"}};
    if (name == "checks") return {"run_check_results", "row_id",
        {"id","CHECK_ID","CHECK_NAME","LABEL","SOURCE","ARGUMENT","CNT","TOTAL","METRIC","APP_IDS","RESULT","CONFIDENCE","check_id"}};
    throw HttpError(400, "artifact must be normalized, comparisons, facts or checks");
}

std::shared_ptr<FILE> openSpool() {
    // /tmp is a 64 MiB tmpfs in Compose. Use the writable uploads volume and
    // unlink immediately: the descriptor stays open, even across a slow download.
    const auto configured = std::getenv("UPLOAD_ROOT");
    auto pattern = (std::filesystem::path(configured ? configured : "/uploads") / ".export-XXXXXX").string();
    const int fd = ::mkstemp(pattern.data());
    if (fd == -1) throw HttpError(507, "Cannot create an export file on the uploads volume");
    if (::unlink(pattern.c_str()) == -1) {
        ::close(fd);
        throw HttpError(507, "Cannot create an anonymous export file");
    }
    FILE* file = ::fdopen(fd, "w+b");
    if (!file) {
        ::close(fd);
        throw HttpError(507, "Cannot open the export file");
    }
    return {file, std::fclose};
}

void writeBytes(FILE* file, const std::string& bytes) {
    if (std::fwrite(bytes.data(), 1, bytes.size(), file) != bytes.size())
        throw HttpError(507, "Cannot store the complete export on the uploads volume");
}

std::vector<std::string> csvColumns(pqxx::read_transaction& tx, const std::string& from,
                                    const std::vector<std::string>& fallback) {
    // Discover keys across the whole run, including fields added by user
    // procedures only to later rows. Binary collation gives a stable header.
    const auto result = tx.exec("SELECT DISTINCT key COLLATE \"C\" AS col FROM "
                                "(SELECT jsonb_object_keys(data) AS key " + from +
                                ") keys ORDER BY col LIMIT 4097");
    if (result.empty()) return fallback;
    if (result.size() > 4096) throw HttpError(422, "CSV export supports up to 4096 distinct columns");
    std::vector<std::string> columns;
    std::size_t headerBytes = 0;
    for (auto record : result) {
        columns.push_back(record[0].as<std::string>());
        headerBytes += columns.back().size();
        if (headerBytes > 16 * 1024 * 1024) throw HttpError(422, "CSV header exceeds 16 MiB");
    }
    return columns;
}

} // namespace

drogon::HttpResponsePtr makeExportResponse(const Storage& storage, const std::string& run,
                                          const std::string& artifact, const std::string& formatName) {
    const auto spec = artifactSpec(artifact);
    const auto format = exportFormat(formatName);
    auto file = openSpool();
    {
        auto connection = storage.connection();
        pqxx::read_transaction tx(*connection);
        tx.exec("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");
        const auto from = "FROM " + std::string(spec.table) + " WHERE run_id=" + tx.quote(run);
        std::vector<std::string> columns;
        if (format == ExportFormat::csv) columns = csvColumns(tx, from, spec.emptyColumns);
        writeBytes(file.get(), exportPrefix(format, columns));
        tx.exec("DECLARE export_rows NO SCROLL CURSOR FOR SELECT data " + from + " ORDER BY " + spec.ordering);
        bool first = true;
        while (true) {
            const auto batch = tx.exec("FETCH FORWARD 32 FROM export_rows");
            if (batch.empty()) break;
            for (auto record : batch) {
                auto json = record[0].as<std::string>();
                if (format == ExportFormat::json) {
                    // The jsonb column already holds a valid JSON object.
                    writeBytes(file.get(), first ? "\n" : ",\n");
                    writeBytes(file.get(), json);
                } else {
                    writeBytes(file.get(), exportRow(format, decode(json), columns, first));
                }
                first = false;
            }
        }
        writeBytes(file.get(), exportSuffix(format));
        tx.commit();
    }
    if (std::fflush(file.get()) != 0) throw HttpError(507, "Cannot finish storing the complete export");
    const auto length = ::ftello(file.get());
    if (length < 0 || ::fseeko(file.get(), 0, SEEK_SET) != 0)
        throw HttpError(500, "Cannot read the finished export file");
    const char* mime = format == ExportFormat::json ? "application/json; charset=utf-8"
                       : format == ExportFormat::xml ? "application/xml; charset=utf-8"
                                                    : "text/csv; charset=utf-8";
    const auto filename = "run-" + run + "-" + artifact + "." + formatName;
    auto response = drogon::HttpResponse::newStreamResponse(
        [file](char* buffer, std::size_t capacity) -> std::size_t {
            return buffer ? std::fread(buffer, 1, capacity, file.get()) : 0;
        }, filename, drogon::CT_CUSTOM, mime);
    response->addHeader("Content-Length", std::to_string(length));
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("X-Content-Type-Options", "nosniff");
    return response;
}

} // namespace lens