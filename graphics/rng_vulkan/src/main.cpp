#include "gpu.hpp"
#include "reference.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>

namespace {
std::uint32_t number(const std::string& text) {
    if(text.empty() || text.find_first_not_of("0123456789")!=std::string::npos)
        throw std::invalid_argument("Expected unsigned integer: "+text);
    auto value=std::stoull(text);
    if(value>UINT32_MAX) throw std::invalid_argument("Integer exceeds uint32: "+text);
    return static_cast<std::uint32_t>(value);
}
Options parse(int argc,char** argv) {
    Options o;
    for(int i=1;i<argc;++i) {
        std::string arg=argv[i];
        if(arg=="--help") {
            std::cout << "Vulkan RNG workload laboratory\n"
                "  --workload pi|walk     Monte Carlo pi or 2D uniform-step diffusion\n"
                "  --mode split|fused     Materialized RNG pass or inline RNG\n"
                "  --isolate rng|consumer Isolate a split pass in the GPU envelope;\n"
                "                         requires --headless --no-viz and finite frames\n"
                "  --rng-stream PATH      Read framed RNG batches from file/FIFO/driver;\n"
                "                         requires --mode split (see JETSON_FPGA.md)\n"
                "  --count N              Parallel samples/walkers (default 65536)\n"
                "  --blocks N             Philox blocks per entity (default 16)\n"
                "  --frames N             Measured frames after warmup; default 0=interactive\n"
                "  --warmup N             Warmup frames (default 20)\n"
                "  --seed N --step-size X --visual-samples N --device N\n"
                "  --headless --no-viz --validation --verify\n"
                "  --csv path.csv --image path.ppm\n"
                "Each block supplies 4 uniform values: 2 pi samples or 2 walk steps.\n";
            std::exit(0);
        }
        if(arg=="--headless") {o.headless=true;continue;}
        if(arg=="--no-viz") {o.visualize=false;continue;}
        if(arg=="--validation") {o.validation=true;continue;}
        if(arg=="--verify") {o.verify=true;continue;}
        if(i+1>=argc) throw std::invalid_argument("Missing value for "+arg);
        std::string value=argv[++i];
        if(arg=="--count") o.count=number(value);
        else if(arg=="--blocks") o.blocks=number(value);
        else if(arg=="--frames") o.frames=number(value);
        else if(arg=="--warmup") o.warmup=number(value);
        else if(arg=="--seed") o.seed=number(value);
        else if(arg=="--visual-samples") o.visualSamples=number(value);
        else if(arg=="--device") o.deviceIndex=number(value);
        else if(arg=="--csv") o.csv=value;
        else if(arg=="--image") o.image=value;
        else if(arg=="--rng-stream") {
            if(value.empty()) throw std::invalid_argument("rng-stream path must not be empty");
            o.rngStream=value;
        }
        else if(arg=="--workload" && (value=="pi"||value=="walk")) o.workload=value=="walk";
        else if(arg=="--mode" && (value=="split"||value=="fused")) o.mode=value=="fused";
        else if(arg=="--isolate" && (value=="rng"||value=="consumer")) o.isolate=value=="rng"?1:2;
        else if(arg=="--step-size") {
            std::size_t used=0; o.stepSize=std::stof(value,&used);
            if(used!=value.size()||!std::isfinite(o.stepSize)||o.stepSize<=0||o.stepSize>1)
                throw std::invalid_argument("step-size must be in (0,1]");
        } else throw std::invalid_argument("Unknown argument or value: "+arg+" "+value);
    }
    if(!o.count || !o.blocks || !o.visualSamples) throw std::invalid_argument("count, blocks and visual-samples must be positive");
    if(o.blocks>1000000 || o.count>16777216 || std::uint64_t(o.count)*o.blocks>UINT32_MAX/4)
        throw std::invalid_argument("Workload exceeds safe shader indexing/count limits");
    if(std::uint64_t(o.frames)+o.warmup>UINT32_MAX) throw std::invalid_argument("epoch would overflow");
    if(o.isolate && (o.mode || !o.headless || o.visualize || o.verify))
        throw std::invalid_argument("--isolate requires --mode split --headless --no-viz and cannot use --verify");
    if(!o.rngStream.empty() && (o.mode || o.isolate))
        throw std::invalid_argument("--rng-stream requires split mode without --isolate");
    if(o.headless && !o.frames && !o.verify) throw std::invalid_argument("Headless runs require finite --frames");
    if(!o.visualize && !o.image.empty()) throw std::invalid_argument("--image requires visualization");
    return o;
}
std::string csvString(const std::string& value) {
    std::string out="\"";
    for(char c:value) {out+=c;if(c=='\"')out+='\"';}
    return out+'\"';
}
const char* isolationName(std::uint32_t isolate) {
    return isolate==1?"rng":(isolate==2?"consumer":"none");
}
float asFloat(std::uint32_t value) {float result;std::memcpy(&result,&value,4);return result;}
void verify(Options o) {
    o.count=259; o.blocks=7; o.headless=true; o.visualize=false;
    Gpu gpu(o);
    std::vector<State> expected(o.count);
    for(std::uint32_t epoch=0;epoch<3;++epoch) {
        gpu.frame(epoch);
        auto actual=gpu.readStates();
        for(std::uint32_t id=0;id<o.count;++id) {
            expected[id]=rnglab::simulateState(expected[id],id,o.blocks,epoch,o.seed,o.workload,o.stepSize);
            if(o.workload==0) {
                if(actual[id]!=expected[id]) throw std::runtime_error("GPU/CPU pi mismatch at entity "+std::to_string(id));
            } else for(unsigned k=0;k<2;++k) {
                if(!std::isfinite(asFloat(actual[id][k])) || std::abs(asFloat(actual[id][k])-asFloat(expected[id][k]))>2e-6f)
                    throw std::runtime_error("GPU/CPU walk mismatch at entity "+std::to_string(id));
            }
        }
        if(o.mode==0) {
            auto random=gpu.readRandoms();
            for(std::uint32_t block=0;block<o.blocks;++block)
                for(std::uint32_t id=0;id<o.count;++id)
                    if(random[std::size_t(block)*o.count+id]!=rnglab::randomBlock(id,block,epoch,o.workload,o.seed))
                        throw std::runtime_error("GPU Philox mismatch");
        }
    }
    if(gpu.validationErrors()) throw std::runtime_error("Vulkan validation errors during verification");
    std::cout << "PASS GPU/CPU verification: 259 entities, 7 blocks, 3 epochs; "
              << (o.workload?"walk":"pi") << ' ' << (o.mode?"fused":"split") << '\n';
}
double median(std::vector<double> values) {
    std::sort(values.begin(),values.end());
    return values.size()%2?values[values.size()/2]:(values[values.size()/2-1]+values[values.size()/2])/2;
}
}
int main(int argc,char** argv) {
    try {
        const Options o=parse(argc,argv);
        if(o.verify) {verify(o);return 0;}
        std::ofstream csv;
        if(!o.csv.empty()) {
            csv.open(o.csv); if(!csv) throw std::runtime_error("Cannot open CSV: "+o.csv);
            csv << "device,driver_version,timestamp_period_ns,timestamp_method,rng_source,visualization_enabled,workload,mode,isolation,count,blocks,seed,frame,epoch,warmup,step_size,visual_samples,rng_ms,rng_input_ms,rng_staging_ms,upload_ms,simulation_ms,compute_ms,visual_prepare_ms,render_ms,gpu_ms,cpu_ms,random_values,operations\n";
            csv << std::setprecision(10);
        }
        Gpu gpu(o);
        std::cout << "Device: " << gpu.deviceName() << " | driver " << gpu.driverVersion()
            << " | timestamp period " << gpu.timestampPeriod() << " ns | " << gpu.timestampBits() << " valid bits | " << gpu.timestampMethod() << '\n'
            << (o.workload?"Uniform-step random walk":"Monte Carlo pi") << " | " << (o.mode?"fused":"split")
            << " | " << o.count << " entities x " << o.blocks << " blocks | visualization " << (o.visualize?"on":"off")
            << " | isolation " << isolationName(o.isolate) << " | RNG source " << (o.rngStream.empty()?"GPU":"external stream") << '\n';
        if(o.isolate==2) std::cout << "Consumer inputs are generated for each current epoch in a separate untimed GPU submission; CPU time includes that preparation.\n";
        std::vector<double> rng,sim,compute,visual,render,total,rngFractions,input,staging,upload;
        std::uint32_t epoch=0, measured=0;
        while(gpu.poll() && (o.frames==0 || measured<o.frames)) {
            auto t=gpu.frame(epoch);
            if(epoch>=o.warmup) {
                if(csv) csv << csvString(gpu.deviceName()) << ',' << gpu.driverVersion() << ',' << gpu.timestampPeriod() << ',' << gpu.timestampMethod() << ',' << (o.rngStream.empty()?"gpu":"external") << ',' << o.visualize << ','
                    << (o.workload?"walk":"pi") << ',' << (o.mode?"fused":"split") << ',' << isolationName(o.isolate) << ',' << o.count << ',' << o.blocks << ',' << o.seed << ',' << measured << ',' << epoch << ',' << o.warmup << ',' << o.stepSize << ',' << o.visualSamples << ','
                    << t.rng << ',' << t.rngInput << ',' << t.rngStaging << ',' << t.upload << ',' << t.simulation << ',' << t.compute << ',' << t.visualPrepare << ',' << t.render << ',' << t.gpu << ',' << t.cpu << ','
                    << std::uint64_t(o.count)*o.blocks*4 << ',' << (o.isolate==1?0:std::uint64_t(o.count)*o.blocks*2) << '\n';
                // Bounded memory for indefinitely interactive runs; full data stays in CSV.
                if(total.size()==10000) {rng.clear();sim.clear();compute.clear();visual.clear();render.clear();total.clear();rngFractions.clear();input.clear();staging.clear();upload.clear();}
                rng.push_back(t.rng);sim.push_back(t.simulation);compute.push_back(t.compute);
                visual.push_back(t.visualPrepare);render.push_back(t.render);total.push_back(t.gpu);
                input.push_back(t.rngInput);staging.push_back(t.rngStaging);upload.push_back(t.upload);
                if(!o.mode && !o.isolate && o.rngStream.empty() && t.compute>0) rngFractions.push_back(t.rng/t.compute);
                ++measured;
            }
            if(epoch==UINT32_MAX) throw std::runtime_error("RNG epoch exhausted");
            ++epoch;
        }
        if(csv.is_open()) {
            csv.flush();
            if(!csv) throw std::runtime_error("CSV write failed: "+o.csv);
        }
        if(!total.empty()) {
            std::cout << std::fixed << std::setprecision(4) << "Median GPU ms: RNG=" << median(rng)
                << " simulation=" << median(sim) << " compute=" << median(compute)
                << " visual_prepare=" << median(visual) << " render=" << median(render) << " total=" << median(total) << '\n';
            if(!o.rngStream.empty()) std::cout << "External RNG median ms: driver/read wait=" << median(input)
                << " host staging copy=" << median(staging) << " GPU upload=" << median(upload) << '\n';
            if(!rngFractions.empty()) std::cout << "Median per-frame RNG/full-compute fraction (includes buffer stores/probes): "
                << 100*median(rngFractions) << "%\n";
            if(o.isolate) std::cout << "Isolated " << isolationName(o.isolate)
                << " GPU envelope including instrumentation: " << median(total) << " ms\n";
        }
        if(epoch && o.isolate!=1) {
            auto state=gpu.readStates();
            if(!o.workload) {
                std::uint64_t hits=0;for(auto s:state) hits+=s[0];
                auto samples=std::uint64_t(o.count)*o.blocks*2;
                double p=static_cast<double>(hits)/samples;
                std::cout << std::setprecision(8) << "Final batch pi=" << 4*p << " (" << hits << '/' << samples
                    << "); approximate 95% sampling half-width=" << 1.96*4*std::sqrt(p*(1-p)/samples) << '\n';
            } else {
                double r2=0,mx=0,my=0;
                for(auto s:state) {double x=asFloat(s[0]),y=asFloat(s[1]);mx+=x;my+=y;r2+=x*x+y*y;}
                std::cout << "Walk mean=(" << mx/o.count << ',' << my/o.count << "), wrapped mean r^2=" << r2/o.count
                    << "; unwrapped expectation=" << double(epoch)*o.blocks*2*(2.0/3.0)*o.stepSize*o.stepSize << '\n';
            }
            if(!o.image.empty()) gpu.saveImage(o.image);
        }
        if(gpu.validationErrors()) throw std::runtime_error("Vulkan validation reported errors");
    } catch(const std::exception& e) {std::cerr << "Error: " << e.what() << '\n';return 1;}
    return 0;
}
