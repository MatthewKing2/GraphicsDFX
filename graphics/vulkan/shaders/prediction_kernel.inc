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
    uint start=sectionStartTick(request.junction),end=start+sectionTicks(request.junction);
    uint events=obstacleCount(request.junction);
    uint tick=max(start,request.worldTick);
    int x=request.xMm,z=request.zMm,velocity=request.velocity,vz=request.verticalVelocity;
    int center=laneCenter(route.choiceCount,choice);
    bool inside=tick>=start+TUNNEL_ENTRY_TICK;
    if(inside) x-=center;
    rng=randomNext(rng);
    uint reaction=route.reactionTicks+(rng&7u);
    int desired=0,desiredZ=0;
    uint event=0u;
    for(uint step=0u;step<request.horizon;++step) {
        if(tick>=end) {result.survived=1u;return result;}
        ++result.steps;
        ++tick;
        if(reaction>0u) --reaction;
        else {
            rng=randomNext(rng);
            Target target;target.x=center;target.z=0;
            if(inside) {
                uint arrival=start+hazardTick(request.junction,event);
                int uncertainty=int((rng>>16u)&7u)-3;
                uint predicted=uint(max(0,int(arrival)+uncertainty));
                if(event<events) target=steeringTarget(route,x,z,predicted,event);
                else {target.x=0;target.z=0;}
            }
            int noise=(int(rng&255u)-128)*int(route.lateralNoise)/128;
            target.x+=noise;
            rng=randomNext(rng);
            target.z+=(int(rng&255u)-128)*int(route.lateralNoise)/128;
            desired=clamp((target.x-x)/4,-TUNNEL_MAX_VELOCITY,TUNNEL_MAX_VELOCITY);
            desiredZ=clamp((target.z-z)/4,-TUNNEL_MAX_VELOCITY,TUNNEL_MAX_VELOCITY);
            reaction=max(1u,route.reactionTicks/2u+(rng&3u));
            // A rare delayed update represents a missed reaction, not a random collision.
            if((rng&31u)==0u) reaction+=6u;
        }
        velocity+=clamp(desired-velocity,-TUNNEL_ACCELERATION,TUNNEL_ACCELERATION);
        vz+=clamp(desiredZ-vz,-TUNNEL_ACCELERATION,TUNNEL_ACCELERATION);
        x+=velocity;
        z+=vz;
        ++result.progress;
        if(!inside && tick>=start+TUNNEL_ENTRY_TICK) {
            x-=center;inside=true;reaction=0u;
        }
        if(inside && tick<start+mergeTick(request.junction) &&
           !withinTube(x,z,TUNNEL_LOCAL_LIMIT)) return result;
        if(!withinTube(inside?x+center:x,z,TUNNEL_SHAFT_LIMIT)) return result;
        uint arrival=start+hazardTick(request.junction,event);
        if(event<events && tick>=arrival-3u) {
            if(blocked(route.obstacles[event],x,z,tick)) return result;
            if(tick>=arrival+3u) {++event;reaction=0u;}
        }
    }
    result.survived=tick>=end?1u:0u;
    return result;
}
