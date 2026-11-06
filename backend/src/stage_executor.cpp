#include "stage_executor.hpp"
#include <openssl/rand.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace lens {
namespace fs = std::filesystem;
static std::string encode(const Json::Value& value) {
    thread_local const std::unique_ptr<Json::StreamWriter> writer = [] {
        Json::StreamWriterBuilder builder; builder["indentation"] = "";
        return std::unique_ptr<Json::StreamWriter>(builder.newStreamWriter());
    }();
    std::ostringstream stream; writer->write(value, &stream); return stream.str();
}
static Json::Value decode(const std::string& text) {
    Json::CharReaderBuilder reader; reader["rejectDupKeys"] = true;
    std::istringstream stream(text); Json::Value out; std::string errors;
    if (!Json::parseFromStream(reader, stream, &out, &errors)) throw std::runtime_error("Worker returned invalid JSON: " + errors);
    return out;
}
Json::Value executePython(const std::string& kind, const std::string& code,
        const Json::Value& input, const Json::Value& config,
        const std::function<bool()>& cancelled,
        const std::function<void(const std::string&)>& log) {
    const char* configured = std::getenv("JOBS_ROOT");
    const fs::path root = configured ? configured : "/jobs";
    unsigned char random[16];
    if (RAND_bytes(random, sizeof(random)) != 1) throw std::runtime_error("Cannot generate worker job id");
    std::ostringstream identifier;
    for (unsigned char c : random) identifier << std::hex << std::setw(2) << std::setfill('0') << int(c);
    const fs::path job = root / identifier.str();
    const auto timeout = std::chrono::seconds(config.get("worker_timeout_seconds", 180).asInt());
    if (timeout.count() < 1 || timeout.count() > 900) throw std::runtime_error("worker_timeout_seconds must be 1..900");
    fs::create_directories(job);
    struct Cleanup { fs::path path; ~Cleanup(){std::error_code error;if(fs::exists(path / "done.json",error)||!fs::exists(path / "ready",error))fs::remove_all(path,error);} } cleanup{job};
    {
        std::ofstream script(job / "procedure.py", std::ios::binary); script << code;
        if (!script) throw std::runtime_error("Cannot write worker procedure");
        std::ofstream stream(job / "input.ndjson", std::ios::binary);
        Json::Value meta; meta["type"] = "meta"; meta["kind"] = kind; meta["config"] = config;
        stream << encode(meta) << '\n';
        for (auto it = input.begin(); it != input.end(); ++it) {
            const auto type = encode(Json::Value(it.name()));
            auto writeRow = [&](const Json::Value& row) {
                // Serialize the existing row directly: constructing a wrapper
                // Json::Value would copy every dictionary in the full mart.
                stream << "{\"type\":" << type << ",\"data\":" << encode(row) << "}\n";
            };
            if (it->isArray()) { for (const auto& row : *it) writeRow(row); }
            else writeRow(*it);
        }
        if (!stream) throw std::runtime_error("Cannot write worker input");
    }
    if (fs::file_size(job / "input.ndjson") > 512ULL * 1024 * 1024) throw std::runtime_error("Worker input exceeds 512 MiB");
    { std::ofstream ready(job / "ready.tmp"); ready << timeout.count(); if(!ready)throw std::runtime_error("Cannot enqueue Python job"); }
    fs::rename(job / "ready.tmp",job / "ready");
    const auto deadline = std::chrono::steady_clock::now() + timeout + std::chrono::seconds(10);
    auto readLog = [&]() { std::ifstream diagnostics(job / "diagnostics.log"); std::string line; size_t total = 0;
        while (std::getline(diagnostics, line) && total < 65536) { log(line); total += line.size(); } };
    while (!fs::exists(job / "done.json")) {
        if (cancelled() || std::chrono::steady_clock::now() > deadline) {
            { std::ofstream cancel(job / "cancel"); cancel << "stop"; }
            for(int attempt=0;attempt<30&&!fs::exists(job / "done.json");++attempt)std::this_thread::sleep_for(std::chrono::milliseconds(100));
            readLog();
            throw std::runtime_error(cancelled() ? "CANCELLED" : "Python worker timeout (or worker is unavailable)");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    readLog();
    if(fs::file_size(job / "done.json")>65536)throw std::runtime_error("Worker status is too large");
    std::ifstream doneFile(job / "done.json"); std::ostringstream doneText; doneText << doneFile.rdbuf();
    auto done = decode(doneText.str());
    if (!done.get("ok", false).asBool()) throw std::runtime_error("Python worker: " + done.get("error", "execution failed").asString());
    if (fs::file_size(job / "output.ndjson") > 512ULL * 1024 * 1024) throw std::runtime_error("Worker output exceeds 512 MiB");
    Json::Value rows(Json::arrayValue), segments(Json::arrayValue);
    std::ifstream output(job / "output.ndjson"); std::string line;
    while (std::getline(output, line)) {
        if (line.size() > 16 * 1024 * 1024) throw std::runtime_error("Worker output row is too large");
        auto value = decode(line);
        if (!value.isObject()) throw std::runtime_error("Worker must emit JSON objects");
        if (kind == "compare" && value.get("record_type", "").asString() == "segment") { value.removeMember("record_type"); segments.append(value); }
        else rows.append(value);
        if ((rows.size() % 10000 == 0) && cancelled()) throw std::runtime_error("CANCELLED");
    }
    if (kind == "compare") { Json::Value result; result["rows"] = rows; result["segments"] = segments; return result; }
    return rows;
}
}
