// Shared Q10 motion/collision primitives. Coordinates are millimetres.
uint randomNext(uint state) {state^=state<<13;state^=state>>17;state^=state<<5;return state;}
uint mixSeed(uint value) {
    value^=value>>16;value*=0x7feb352du;value^=value>>15;
    value*=0x846ca68bu;value^=value>>16;return value|1u;
}
int laneCenter(uint count,uint choice) {return (int(choice)*2-int(count)+1)*2000;}
int sineQ10(uint phase) {
    uint index=((phase+2u)>>2u)&31u;
    bool negative=index>16u;
    if(negative) index-=16u;
    if(index>8u) index=16u-index;
    int value=0;
    switch(index) {
        case 0u:value=0;break;case 1u:value=200;break;case 2u:value=392;break;
        case 3u:value=569;break;case 4u:value=724;break;case 5u:value=851;break;
        case 6u:value=946;break;case 7u:value=1004;break;default:value=1024;break;
    }
    return negative?-value:value;
}
Pose pose(Obstacle obstacle,uint tick) {
    uint phase=(((tick+obstacle.phase)<<7u)>>obstacle.period)&127u;
    int wave=(phase<64u?int(phase):128-int(phase))*32-1024;
    Pose result;
    result.center=obstacle.offset;
    result.cosine=1024;result.sine=0;result.aperture=obstacle.opening;
    if(obstacle.kind==1u) result.center+=(obstacle.amplitude*wave)/1024;
    if(obstacle.kind==2u) {
        if((obstacle.flags&2u)!=0u) phase=uint((wave+1024)/16)&127u;
        if((obstacle.flags&1u)!=0u) phase=(128u-phase)&127u;
        result.cosine=sineQ10(phase+32u);result.sine=sineQ10(phase);
    }
    if(obstacle.kind==3u) result.aperture=obstacle.opening+
        ((obstacle.extent-obstacle.opening)*(wave+1024))/2048;
    return result;
}
bool blocked(Obstacle obstacle,int x,uint tick) {
    Pose p=pose(obstacle,tick);
    int dx=x-p.center;
    if(obstacle.kind==0u) return abs(dx)<(obstacle.extent*400)/3000+180;
    if(obstacle.kind==1u) return abs(dx)<obstacle.extent+180;
    if(obstacle.kind==2u) {
        // Conservative circle-vs-oriented-bar test; same boundary in real play.
        return abs(dx*p.cosine)<=obstacle.extent*1024+260640 &&
               abs(dx*p.sine)<=210*1024+260640;
    }
    return abs(dx)+180>=p.aperture;
}
int steeringTarget(Route route,int x,uint tick,uint eventTick,uint event) {
    int best=0,bestCost=2147483647;
    for(int candidateIndex=-4;candidateIndex<=4;++candidateIndex) {
        int candidate=candidateIndex*350;
        if(!blocked(route.obstacles[event],candidate,eventTick) &&
           !blocked(route.obstacles[event],candidate,eventTick-3u) &&
           !blocked(route.obstacles[event],candidate,eventTick+3u)) {
            // Prefer a nearby clear lane, with a mild centering bias.
            int cost=abs(candidate-x)+abs(candidate)/4;
            if(cost<bestCost) {bestCost=cost;best=candidate;}
        }
    }
    return best;
}
