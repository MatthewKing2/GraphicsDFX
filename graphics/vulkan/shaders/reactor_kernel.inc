// Shared C++/GLSL transition model. One state is 128 faults + 128 three-bit ages.
// Double-buffered state: no relay reads a neighbor updated during this same tick.
uint reactorBit(ReactorState state,uint node,uint plane) {
    return (state.words[plane*4u+(node>>5u)]>>(node&31u))&1u;
}
uint relayLevel(ReactorState state,uint node) {
    return reactorBit(state,node,1u)|(reactorBit(state,node,2u)<<1u)|
           (reactorBit(state,node,3u)<<2u);
}
uint hazardLevel(ReactorState state,Obstacle obstacle) {
    return relayLevel(state,(obstacle.flags>>8u)&127u);
}
ReactorState reactorStep(COURSE_PARAM ReactorState old,uint tick,uint routePower,INOUT_UINT rng) {
    ReactorState next;
    for(uint w=0u;w<16u;++w) next.words[w]=0u;
    for(uint node=0u;node<128u;++node) {
        uint rule=course.reactorRules[node];
        uint a=rule&127u,b=(rule>>7u)&127u,c=(rule>>14u)&127u,bank=(rule>>21u)&3u;
        uint rate=((rule>>23u)&255u)+min(20u,tick/180u);
        // Boosting a bank reduces its faults but steals capacity from the next bank.
        if((routePower&(1u<<bank))!=0u) rate=max(1u,rate/4u);
        if((routePower&(1u<<((bank+3u)&3u)))!=0u) rate+=8u;
        rng=randomNext(rng);
        bool cascade=reactorBit(old,a,0u)!=0u && reactorBit(old,b,0u)!=0u;
        bool fault=(rng&255u)<rate ||
            (cascade && ((rng>>24u)&255u)<96u) ||
            (reactorBit(old,c,0u)!=0u && ((rng>>16u)&255u)<12u) ||
            (reactorBit(old,node,0u)!=0u && ((rng>>8u)&255u)>=64u);
        uint age=relayLevel(old,node);
        age=fault?min(7u,age+1u):(age>1u?age-2u:0u);
        uint word=node>>5u,bit=1u<<(node&31u);
        if(fault) next.words[word]|=bit;
        for(uint plane=0u;plane<3u;++plane)
            if((age&(1u<<plane))!=0u) next.words[(plane+1u)*4u+word]|=bit;
    }
    return next;
}
Obstacle reactorObstacle(Obstacle obstacle,ReactorState state,uint tick,uint pulseUntil) {
    if(obstacle.kind==3u) {
        uint age=hazardLevel(state,obstacle);
        int gap=obstacle.extent;
        // Three warning states, then four progressively closing states.
        if(tick>=pulseUntil && age>=4u)
            gap=max(60,obstacle.extent-int(age-3u)*(obstacle.extent-60)/4);
        obstacle.opening=gap;obstacle.extent=gap;
    }
    return obstacle;
}
bool reactorBlocked(Obstacle obstacle,ReactorState state,int x,int z,uint tick,uint pulseUntil) {
    if(obstacle.kind==2u && (tick<pulseUntil || hazardLevel(state,obstacle)<4u)) return false;
    return blocked(reactorObstacle(obstacle,state,tick,pulseUntil),x,z,tick);
}
Target reactorTarget(Route route,ReactorState state,int x,int z,uint tick,uint event,uint pulseUntil) {
    Target best;best.x=0;best.z=0;
    int cost=2147483647;
    for(int ix=-1;ix<=1;++ix) for(int iz=-1;iz<=1;++iz) {
        int cx=ix*6500,cz=iz*6500;
        if(withinTube(cx,cz,TUNNEL_LOCAL_LIMIT) &&
           !reactorBlocked(route.obstacles[event],state,cx,cz,tick,pulseUntil)) {
            int candidate=abs(cx-x)+abs(cz-z)+(abs(cx)+abs(cz))/4;
            if(candidate<cost) {cost=candidate;best.x=cx;best.z=cz;}
        }
    }
    return best;
}
