// Shared scalar C++/GLSL forecast: same circuit, hazards, pulse and control rings.
#include "tunnel_math.inc"
#include "reactor_kernel.inc"
struct RolloutResult { uint survived; uint progress; uint steps; };
RolloutResult rollout(COURSE_PARAM Request request,uint choice,uint number) {
    RolloutResult result;
    result.survived=0u;result.progress=0u;result.steps=0u;
    uint rng=mixSeed(request.seed ^ (number*0x9e3779b9u) ^ (choice*0x85ebca6bu));
    // Circuit and controller have independent streams. No actual future RNG is sent.
    uint circuitRng=mixSeed(rng^0x706f7765u);
    ReactorState reactor=request.reactor;
    Route route=course.routes[(request.junction&31u)*4u+choice];
    uint start=sectionStartTick(request.junction),end=start+sectionTicks(request.junction);
    uint entry=start+entryTick(request.junction),halfWindow=contactTicks(request.junction);
    uint events=obstacleCount(request.junction),event=0u,tick=request.worldTick;
    uint routed=request.routePower&15u,pulseUntil=request.pulseUntil,charges=request.pulseCharges;
    bool collected=false,inside=false;
    bool frozen=(request.routePower&256u)!=0u; // Diagnostic ablation, not a valid predictor.
    int x=request.xMm,z=request.zMm,velocity=request.velocity,vz=request.verticalVelocity;
    int center=laneCenter(route.choiceCount,choice),desired=0,desiredZ=0;
    rng=randomNext(rng);
    uint reaction=route.reactionTicks+(rng&7u);
    for(uint step=0u;step<request.horizon;++step) {
        if(tick>=end) {result.survived=1u;return result;}
        ++tick;++result.steps;
        if(tick%6u==0u && !frozen)
            reactor=reactorStep(COURSE_ARG reactor,tick,routed,circuitRng);
        // Previewing the next fork conditions on arriving with the supplied x/z.
        // Evolve its reactor from NOW, but do not invent movement through the current route.
        if(tick<=start) continue;
        if(reaction>0u) --reaction;
        else {
            rng=randomNext(rng);
            Target target;target.x=center;target.z=0;
            if(inside) {
                uint arrival=start+hazardTick(request.junction,event);
                if(event<events) target=reactorTarget(route,reactor,x,z,arrival,event,pulseUntil);
                else {target.x=0;target.z=0;}
                // The forecast uses the same simple emergency-pulse policy documented in UI.
                if(event<events && charges>0u && tick>=pulseUntil &&
                   arrival<=tick+18u && route.obstacles[event].kind==3u &&
                   hazardLevel(reactor,route.obstacles[event])>=5u) {
                    pulseUntil=tick+72u;--charges;
                }
            }
            target.x+=(int(rng&255u)-128)*int(route.lateralNoise)/128;
            rng=randomNext(rng);
            target.z+=(int(rng&255u)-128)*int(route.lateralNoise)/128;
            desired=clamp((target.x-x)/4,-TUNNEL_MAX_VELOCITY,TUNNEL_MAX_VELOCITY);
            desiredZ=clamp((target.z-z)/4,-TUNNEL_MAX_VELOCITY,TUNNEL_MAX_VELOCITY);
            reaction=max(1u,route.reactionTicks/2u+(rng&3u));
            if((rng&31u)==0u) reaction+=6u;
        }
        velocity+=clamp(desired-velocity,-TUNNEL_ACCELERATION,TUNNEL_ACCELERATION);
        vz+=clamp(desiredZ-vz,-TUNNEL_ACCELERATION,TUNNEL_ACCELERATION);
        x+=velocity;z+=vz;++result.progress;
        if(!inside && tick>=entry) {x-=center;inside=true;reaction=0u;}
        if(inside && tick<start+mergeTick(request.junction) &&
           !withinTube(x,z,TUNNEL_LOCAL_LIMIT)) return result;
        if(!withinTube(inside?x+center:x,z,TUNNEL_SHAFT_LIMIT)) return result;
        uint control=start+entryTick(request.junction)+
                     (mergeTick(request.junction)-entryTick(request.junction))/3u;
        if(inside && !collected && tick+3u>=control && tick<=control+3u &&
           (x+4000)*(x+4000)+(z-4000)*(z-4000)<4000000) {
            collected=true;routed^=1u<<(choice&3u);
            pulseUntil=max(pulseUntil,tick+36u);charges=min(2u,charges+1u);
        }
        uint arrival=start+hazardTick(request.junction,event);
        if(event<events && tick>=arrival-halfWindow) {
            if(reactorBlocked(route.obstacles[event],reactor,x,z,tick,pulseUntil)) return result;
            if(tick>=arrival+halfWindow) {++event;reaction=0u;}
        }
    }
    result.survived=tick>=end?1u:0u;
    return result;
}
