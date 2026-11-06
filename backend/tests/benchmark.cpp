#include "core.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>

namespace {
using Clock = std::chrono::steady_clock;
double seconds(Clock::time_point start) { return std::chrono::duration<double>(Clock::now()-start).count(); }
std::string read(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);
    if(!input)throw std::runtime_error("Cannot open "+path.string());
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
}

// Usage: core_benchmark /path/to/RUN-20260827-0001
// Measures native processing only. PostgreSQL/API end-to-end measurements must
// be collected separately; this executable deliberately does not imply them.
int main(int argc,char** argv) {
    try {
        if(argc!=2){std::cerr<<"Usage: core_benchmark DATASET_DIRECTORY\n";return 2;}
        auto started=Clock::now();auto root=std::filesystem::path(argv[1]);
        if(std::filesystem::exists(root/"RESULTS"))root/="RESULTS";
        std::vector<std::filesystem::path> files;
        for(const auto& entry:std::filesystem::directory_iterator(root/"BEFORE_MASTER"))
            if(entry.is_regular_file()&&entry.path().extension()==".json")files.push_back(entry.path());
        std::sort(files.begin(),files.end());
        if(files.empty())throw std::runtime_error("Dataset is empty");
        size_t afterFiles=0;for(const auto& entry:std::filesystem::directory_iterator(root/"AFTER_BETA"))
            afterFiles+=entry.is_regular_file()&&entry.path().extension()==".json";
        if(afterFiles!=files.size())throw std::runtime_error("Unpaired dataset");
        double readSeconds=0,normalizeSeconds=0,compareSeconds=0,segmentSeconds=0;
        size_t beforeCount=0,afterCount=0,changedApps=0,changedRows=0;
        std::map<std::string,size_t> changeCounts;
        Json::Value comparisons(Json::arrayValue),segments(Json::arrayValue);
        auto cfg=lens::defaultConfig();
        for(const auto& file:files){
            auto tick=Clock::now();auto b=read(file),a=read(root/"AFTER_BETA"/file.filename());readSeconds+=seconds(tick);
            auto id=file.stem().string();tick=Clock::now();
            auto nb=lens::normalize(b,id,"BEFORE",cfg),na=lens::normalize(a,id,"AFTER",cfg);
            normalizeSeconds+=seconds(tick);beforeCount+=nb.size();afterCount+=na.size();
            tick=Clock::now();auto compared=lens::compare(nb,na,cfg);compareSeconds+=seconds(tick);
            bool changed=false;
            for(auto& row:compared){++changeCounts[row["CHANGE_TYPE"].asString()];if(row["DIFF_FLAG"].asInt()){changed=true;++changedRows;}comparisons.append(std::move(row));}
            changedApps+=changed;
            tick=Clock::now();segments.append(lens::segments(b,a,id,cfg));segmentSeconds+=seconds(tick);
        }
        auto tick=Clock::now();auto facts=lens::aggregate(comparisons,segments,cfg);auto aggregateSeconds=seconds(tick);
        lens::validateFacts(facts);
        Json::Value report;report["scope"]="Native core only; excludes PostgreSQL persistence and HTTP API";
        report["compiler"]=__VERSION__;report["dataset"]=root.string();
        report["apps"]=Json::UInt64(files.size());report["normalized_before"]=Json::UInt64(beforeCount);
        report["normalized_after"]=Json::UInt64(afterCount);report["union_rows"]=Json::UInt64(beforeCount+afterCount);
        report["comparison_rows"]=comparisons.size();report["changed_apps"]=Json::UInt64(changedApps);
        report["changed_rows"]=Json::UInt64(changedRows);report["aggregate_facts"]=facts.size();
        for(const auto& [type,count]:changeCounts)report["change_counts"][type]=Json::UInt64(count);
        report["seconds"]["file_read"]=readSeconds;report["seconds"]["normalization"]=normalizeSeconds;
        report["seconds"]["comparison"]=compareSeconds;report["seconds"]["segments"]=segmentSeconds;
        report["seconds"]["aggregation"]=aggregateSeconds;report["seconds"]["total"]=seconds(started);
        Json::StreamWriterBuilder writer;writer["indentation"]="  ";std::cout<<Json::writeString(writer,report)<<'\n';
        return 0;
    }catch(const std::exception& e){std::cerr<<"Benchmark failed: "<<e.what()<<'\n';return 1;}
}
