#pragma once
#include <chrono>
#include <cstdint>
#include <json/json.h>

namespace lens {
// Jobs execute sequentially on each worker thread. Timers are enabled only
// during the profiled job; concurrent runs never share counters.
struct PipelineProfile {
    double inputRead = 0, inputMapping = 0, idValidation = 0;
    double keySort = 0, renderRows = 0, martFetch = 0, martDecode = 0;
    std::uint64_t inputBytes = 0, mappedBytes = 0, fetchedBytes = 0;
    inline static thread_local PipelineProfile* active = nullptr;

    explicit PipelineProfile(bool enabled) : previous(active) { active = enabled ? this : nullptr; }
    ~PipelineProfile() { active = previous; }
    PipelineProfile(const PipelineProfile&) = delete;
    Json::Value report() const {
        Json::Value v;
        v["source_read_s"] = inputRead;
        v["mapping_s"] = inputMapping;
        v["document_validation_s"] = idValidation;
        v["native_row_sort_s"] = keySort;
        v["native_row_render_s"] = renderRows;
        v["aggregation_cursor_fetch_s"] = martFetch;
        v["aggregation_cursor_decode_s"] = martDecode;
        v["source_bytes"] = Json::UInt64(inputBytes);
        v["mapped_bytes"] = Json::UInt64(mappedBytes);
        v["aggregation_cursor_bytes"] = Json::UInt64(fetchedBytes);
        return v;
    }
private:
    PipelineProfile* previous;
};

class OperationTimer {
    using Clock = std::chrono::steady_clock;
    double* seconds;
    Clock::time_point start;
public:
    explicit OperationTimer(double PipelineProfile::*field) :
        seconds(PipelineProfile::active ? &(PipelineProfile::active->*field) : nullptr) {
        if (seconds) start = Clock::now();
    }
    ~OperationTimer() {
        if (seconds) *seconds += std::chrono::duration<double>(Clock::now()-start).count();
    }
};
}
