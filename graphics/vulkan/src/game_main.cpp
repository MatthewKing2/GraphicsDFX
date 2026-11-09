#include "game.hpp"
#include "gpu_prediction.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

namespace {
using Clock=std::chrono::steady_clock;
class CpuPrediction {
    prediction::Course course;
    std::future<prediction::Result> future;
    Clock::time_point started;
public:
    explicit CpuPrediction(const prediction::Course& c):course(c) {}
    bool busy() const {return future.valid();}
    void submit(prediction::Request r) {
        started=Clock::now();
        future=std::async(std::launch::async,[data=course,r] {return prediction::evaluate(data,r);});
    }
    bool poll(prediction::Result& r) {
        if(!future.valid() || future.wait_for(std::chrono::seconds(0))!=std::future_status::ready) return false;
        r=future.get();
        r.latencyMs=std::chrono::duration<double,std::milli>(Clock::now()-started).count();
        return true;
    }
};
void validateGpu(GpuPrediction& gpu,const prediction::Course& course) {
    for(uint32_t count:{1u,63u,64u,65u,512u,4096u}) {
        for(uint32_t stage=0;stage<3;++stage) {
            prediction::Request r;r.rollouts=count;r.junction=stage;r.progressMm=21000;r.worldTick=stage*177;
            r.budgetTicks=stage==1?24:660;r.seed=12345+stage;r.horizon=stage==2?480:240;
            const auto cpu=prediction::evaluate(course,r);
            if(!gpu.submit(r) || gpu.submit(r)) throw std::runtime_error("prediction busy contract failed");
            prediction::Result device;
            while(!gpu.poll(device)) std::this_thread::yield();
            if(!prediction::sameResults(cpu,device)) throw std::runtime_error("CPU/GPU prediction mismatch");
        }
    }
    std::cout<<"PASS exact CPU/GPU aggregate agreement: 18 requests, partial workgroups, bounded early exits\n";
}
void benchmark(GpuPrediction& gpu,const prediction::Course& course,const std::string& path) {
    std::ofstream file(path);
    if(!file) throw std::runtime_error("cannot open benchmark output");
    const std::string header="backend,state,rollouts_per_route,horizon_ticks,latency_ms,kernel_ms,rollouts_per_s,steps_per_s,left_survival,right_survival,course_bytes,request_bytes,result_bytes,agreement\n";
    file<<header;std::cout<<header;
    for(uint32_t count:{256u,1024u,4096u}) for(uint32_t state=0;state<3;++state) {
        prediction::Request r;r.rollouts=count;r.progressMm=state==0?-10000:21000;
        r.worldTick=state*53;r.budgetTicks=state==2?90:500;r.seed=12345;
        const auto reference=prediction::evaluate(course,r);
        for(int backend=0;backend<2;++backend) {
            double latency=0,kernel=0;prediction::Result last;
            for(int sample=0;sample<13;++sample) {
                if(backend==0) last=prediction::evaluate(course,r);
                else {
                    gpu.submit(r);
                    while(!gpu.poll(last)) std::this_thread::yield();
                }
                if(!prediction::sameResults(reference,last)) throw std::runtime_error("benchmark agreement failed");
                if(sample>=3) {latency+=last.latencyMs;kernel+=last.kernelMs;}
            }
            latency/=10;kernel/=10;
            const uint64_t steps=uint64_t(last.routes[0].steps)+last.routes[1].steps;
            std::ostringstream row;
            row<<std::fixed<<std::setprecision(6)<<(backend?"gpu":"cpu")<<','<<state<<','<<count<<','<<r.horizon<<','<<latency<<','<<kernel
                <<','<<count*2*1000.0/latency<<','<<steps*1000.0/latency<<','<<prediction::survival(last,0)<<','<<prediction::survival(last,1)
                <<",288,48,32,true\n";
            file<<row.str();std::cout<<row.str();
        }
    }
    // Recommendation stability, including an intentionally close comparison.
    auto closeCourse=course;
    closeCourse.routes[0].jumpFailure=900; closeCourse.routes[1].jumpFailure=950;
    closeCourse.routes[0].hurdleMm=closeCourse.routes[1].hurdleMm=0;
    closeCourse.routes[0].gateMm=closeCourse.routes[1].gateMm=0;
    closeCourse.routes[1].speedMmPerTick=closeCourse.routes[0].speedMmPerTick;
    closeCourse.routes[1].gapStart=closeCourse.routes[0].gapStart;
    closeCourse.routes[1].gapEnd=closeCourse.routes[0].gapEnd;
    std::cout<<"stability_count,left_wins_of_32,mean_left_minus_right\n";
    for(uint32_t count:{256u,1024u,4096u}) {
        int left=0;double difference=0;
        for(uint32_t seed=1;seed<=32;++seed) {
            prediction::Request r;r.rollouts=count;r.seed=seed;r.progressMm=20000;
            auto value=prediction::evaluate(closeCourse,r);
            double delta=prediction::survival(value,0)-prediction::survival(value,1);
            left+=delta>=0;difference+=delta;
        }
        std::cout<<count<<','<<left<<','<<difference/32<<'\n';
    }
    std::cout<<"Saved "<<path<<". Timing is not measured energy. Payload excludes PCIe protocol/DMA overhead.\n";
}
}

int main(int argc,char** argv) {
    try {
        bool gpuBackend=true,selfTest=false,validate=false,bench=false,smoke=false,autoplay=false;
        int frameLimit=0;uint32_t rollouts=512;
        std::string output="build/game-benchmark.csv";
        for(int i=1;i<argc;++i) {
            std::string flag=argv[i];
            auto value=[&]() -> std::string {if(++i>=argc) throw std::invalid_argument("missing value for "+flag);return argv[i];};
            if(flag=="--backend") {auto name=value();if(name!="cpu"&&name!="gpu") throw std::invalid_argument("backend must be cpu or gpu");gpuBackend=name=="gpu";}
            else if(flag=="--rollouts") rollouts=uint32_t(std::stoul(value()));
            else if(flag=="--frames") frameLimit=std::stoi(value());
            else if(flag=="--output") output=value();
            else if(flag=="--self-test") selfTest=true;
            else if(flag=="--validate") validate=true;
            else if(flag=="--benchmark") bench=true;
            else if(flag=="--smoke") smoke=true;
            else if(flag=="--autoplay") autoplay=true;
            else if(flag=="--help") {
                std::cout<<"DEADLINE / RUNNER\nWASD move, mouse look, Space jump, R restart, Esc pause.\n"
                    <<"F1 CPU/GPU predictor, F2 guidance off/on, F3 statistics.\n"
                    <<"--backend cpu|gpu --rollouts 512 --frames N --autoplay\n"
                    <<"--self-test (headless) --validate (GPU parity) --benchmark [--output path]\n"
                    <<"--smoke (accelerated rendered autopilot test)\n";return 0;
            } else throw std::invalid_argument("unknown option: "+flag);
        }
        prediction::Course course=prediction::makeCourse();
        prediction::Request checked;checked.rollouts=rollouts;prediction::validate(course,checked);
        if(frameLimit<0) throw std::invalid_argument("frame count must be nonnegative");
        if(selfTest||validate) survival::selfTest();
        if(selfTest&&!validate&&!bench) return 0;

        Renderer renderer(1280,720,"DEADLINE / RUNNER",survival::cubeMesh(),24000);
        GpuPrediction gpu(renderer.computeContext(),course);
        if(validate) {validateGpu(gpu,course);if(!bench) return 0;}
        if(bench) {benchmark(gpu,course,output);return 0;}

        GameInput input(renderer);
        survival::Game game(course);survival::Autopilot bot;
        CpuPrediction cpu(course);
        survival::Hud hud;hud.gpu=gpuBackend;hud.rollouts=rollouts;
        std::vector<SceneInstance> scene;scene.reserve(24000);
        std::vector<double> frames;frames.reserve(240);
        double accumulator=0,nextPrediction=0,lastAccepted=-100,wallTime=0;
        Clock::time_point previous=Clock::now();
        uint32_t requestId=0,validRun=0,validStage=0;
        int frameNumber=0;
        while(!renderer.shouldClose()) {
            const auto frameStart=Clock::now();
            const double dt=std::clamp(std::chrono::duration<double>(frameStart-previous).count(),0.0,.1);
            previous=frameStart;wallTime+=dt;
            renderer.pollEvents();
            auto controls=input.sample();
            if(controls.pause) hud.paused=!hud.paused;
            if(!controls.focused && !smoke && !autoplay) hud.paused=true;
            input.capture(!hud.paused && !smoke && !autoplay);
            if(controls.backend) {hud.gpu=!hud.gpu;hud.valid=false;nextPrediction=0;}
            if(controls.guidance) {hud.guidance=!hud.guidance;hud.valid=false;nextPrediction=0;}
            if(controls.stats) hud.stats=!hud.stats;
            if(!hud.paused) {
                game.look(controls.mouseX,controls.mouseY);
                accumulator+=smoke?8.0/120:dt;
                // Preserve a jump edge until at least one simulation tick consumes it.
                static bool bufferedJump=false,bufferedRestart=false;
                bufferedJump|=controls.jump;bufferedRestart|=controls.restart;
                int catchup=0;
                while(accumulator>=1.0/120 && catchup++<12) {
                    auto stepControls=autoplay||smoke?bot.sample(game):controls;
                    if(!autoplay&&!smoke) {stepControls.jump=bufferedJump;stepControls.restart=bufferedRestart;}
                    game.step(stepControls,1.0f/120);
                    bufferedJump=bufferedRestart=false;accumulator-=1.0/120;
                }
                if(catchup>=12) accumulator=0;
            } else accumulator=0;

            prediction::Result result;
            auto accept=[&](const prediction::Result& r,bool fromGpu) {
                const auto& state=game.state();
                if(!hud.guidance || fromGpu!=hud.gpu || r.request.run!=state.run || r.request.junction!=state.stage ||
                    r.request.courseVersion!=prediction::COURSE_VERSION || !game.approachingJunction() ||
                    uint32_t(state.worldTime*30)-r.request.worldTick>30) return;
                hud.percentages[0]=prediction::survival(r,0);hud.percentages[1]=prediction::survival(r,1);
                const int best=hud.percentages[1]>hud.percentages[0]?1:0;
                if(!hud.valid || hud.percentages[best]>hud.percentages[hud.recommended]+.05f) hud.recommended=best;
                hud.valid=true;validRun=state.run;validStage=state.stage;lastAccepted=wallTime;
                hud.predictionMs=r.latencyMs;hud.kernelMs=fromGpu?r.kernelMs:0;
            };
            if(cpu.poll(result)) accept(result,false);
            if(gpu.poll(result)) accept(result,true);
            if(validRun!=game.state().run || validStage!=game.state().stage || !game.approachingJunction() || wallTime-lastAccepted>.75) hud.valid=false;
            if(!hud.paused && hud.guidance && game.approachingJunction() && wallTime>=nextPrediction && !cpu.busy()&&!gpu.busy()) {
                auto request=game.request(++requestId,rollouts);
                if(hud.gpu) gpu.submit(request);else cpu.submit(request);
                nextPrediction=wallTime+.2;
            }
            hud.pending=cpu.busy()||gpu.busy();
            game.scene(scene,hud);
            renderer.drawScene(game.transforms(renderer.aspectRatio()),scene);
            const double frameMs=std::chrono::duration<double,std::milli>(Clock::now()-frameStart).count();
            frames.push_back(frameMs);
            if(frames.size()==240) {
                double total=0;for(double t:frames) total+=t;
                std::sort(frames.begin(),frames.end());hud.frameMs=total/frames.size();hud.p95Ms=frames[227];
                std::cout<<"[game] frame_avg_ms="<<hud.frameMs<<" p95_ms="<<hud.p95Ms<<" backend="<<(hud.gpu?"gpu":"cpu")
                    <<" prediction_observed_ms="<<hud.predictionMs<<" kernel_ms="<<hud.kernelMs<<" objects="<<scene.size()<<'\n';
                frames.clear();
            }
            ++frameNumber;
            if(smoke && game.state().won) {
                if(game.state().deaths) throw std::runtime_error("rendered autopilot died");
                std::cout<<"PASS rendered course completion, score="<<game.score()<<" frames="<<frameNumber<<'\n';break;
            }
            if(frameLimit && frameNumber>=frameLimit) break;
            if(smoke && frameNumber>1500) throw std::runtime_error("rendered autopilot did not finish");
        }
        return 0;
    } catch(const std::exception& e) {std::cerr<<"Game error: "<<e.what()<<'\n';return 1;}
}
