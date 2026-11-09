// This scalar kernel and tunnel_math.inc compile as both C++ and GLSL.
// The modeled player reacts to geometry with bounded latency/noise. Real play
// has no random deaths. Each choice is evaluated through the SAME section exit.
#include "tunnel_math.inc"
struct RolloutResult { uint survived; uint progress; uint steps; };
RolloutResult rollout(COURSE_PARAM Request request,uint choice,uint number) {
    RolloutResult result;
    result.survived=0u;result.progress=0u;result.steps=0u;
    uint rng=mixSeed(request.seed ^ (number*0x9e3779b9u) ^ (choice*0x85ebca6bu));
    Route route=course.routes[(request.junction & 31u)*4u+choice];
    uint start=request.junction*TUNNEL_SECTION_TICKS;
    uint tick=max(start,request.worldTick);
    int x=request.xMm,velocity=request.velocity;
    int center=laneCenter(route.choiceCount,choice);
    bool inside=tick>=start+TUNNEL_ENTRY_TICK;
    if(inside) x-=center;
    rng=randomNext(rng);
    uint reaction=route.reactionTicks+(rng&7u);
    int desired=0;
    uint event=0u;
    for(uint step=0u;step<request.horizon;++step) {
        if(tick>=start+TUNNEL_SECTION_TICKS) {result.survived=1u;return result;}
        ++result.steps;
        ++tick;
        if(reaction>0u) --reaction;
        else {
            rng=randomNext(rng);
            int target=center;
            if(inside) {
                uint arrival=start+TUNNEL_FIRST_HAZARD+event*TUNNEL_HAZARD_SPACING;
                int uncertainty=int((rng>>16u)&7u)-3;
                uint predicted=uint(max(0,int(arrival)+uncertainty));
                target=event<TUNNEL_OBSTACLES?steeringTarget(route,x,tick,predicted,event):0;
            }
            int noise=(int(rng&255u)-128)*int(route.lateralNoise)/128;
            target+=noise;
            desired=clamp((target-x)/4,-200,200);
            reaction=max(1u,route.reactionTicks/2u+(rng&3u));
            // A rare delayed update represents a missed reaction, not a random collision.
            if((rng&31u)==0u) reaction+=6u;
        }
        velocity+=clamp(desired-velocity,-22,22);
        x+=velocity;
        ++result.progress;
        if(!inside && tick>=start+TUNNEL_ENTRY_TICK) {
            x-=center;inside=true;reaction=0u;
        }
        if(inside && tick<start+TUNNEL_MERGE_TICK && abs(x)>TUNNEL_LOCAL_LIMIT) return result;
        if(abs(inside?x+center:x)>TUNNEL_SHAFT_LIMIT) return result;
        uint arrival=start+TUNNEL_FIRST_HAZARD+event*TUNNEL_HAZARD_SPACING;
        if(event<TUNNEL_OBSTACLES && tick>=arrival-3u) {
            if(blocked(route.obstacles[event],x,tick)) return result;
            if(tick>=arrival+3u) {++event;reaction=0u;}
        }
    }
    result.survived=tick>=start+TUNNEL_SECTION_TICKS?1u:0u;
    return result;
}
