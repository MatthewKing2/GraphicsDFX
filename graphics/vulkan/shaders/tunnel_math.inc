// Shared Q10 motion/collision primitives. Coordinates are millimetres.
#include "tunnel_config.inc"
uint randomNext(uint state) {state^=state<<13;state^=state>>17;state^=state<<5;return state;}
uint mixSeed(uint value) {
    value^=value>>16;value*=0x7feb352du;value^=value>>15;
    value*=0x846ca68bu;value^=value>>16;return value|1u;
}
uint sectionTicks(uint section) {
    return max(TUNNEL_MIN_SECTION_TICKS,
               TUNNEL_SECTION_BASE-min(section,TUNNEL_PACE_RAMP)*TUNNEL_SECTION_SHRINK);
}
uint sectionStartTick(uint section) {
    uint ramp=min(section,TUNNEL_PACE_RAMP);
    uint previous=ramp==0u?0u:ramp-1u;
    return ramp*TUNNEL_SECTION_BASE-TUNNEL_SECTION_SHRINK*ramp*previous/2u+
           (section-ramp)*TUNNEL_MIN_SECTION_TICKS;
}
uint entryTick(uint section) {
    uint scaled=sectionTicks(section)*TUNNEL_ENTRY_TICK/TUNNEL_SECTION_BASE;
    // Final squeeze: the opening window reaches 0.3 s at the one-minute section.
    uint squeeze=section>TUNNEL_PACE_RAMP?min(section-TUNNEL_PACE_RAMP,5u)*11u:0u;
    return max(18u,scaled-squeeze);
}
uint mergeTick(uint section) {
    return sectionTicks(section)-max(2u,sectionTicks(section)*TUNNEL_EXIT_TICKS/TUNNEL_SECTION_BASE);
}
uint contactTicks(uint section) {return max(1u,sectionTicks(section)*3u/TUNNEL_SECTION_BASE);}
uint obstacleCount(uint section) {return min(TUNNEL_OBSTACLES,4u+section/2u);}
uint hazardTick(uint section,uint event) {
    uint lead=max(4u,sectionTicks(section)*30u/TUNNEL_SECTION_BASE);
    return entryTick(section)+lead+
        event*(mergeTick(section)-entryTick(section)-2u*lead)/(obstacleCount(section)-1u);
}
bool withinTube(int x,int z,int limit) {
    // Eight wall half-planes, not a bounding box: diagonal flight must hit the wall too.
    int ax=abs(x),az=abs(z);
    return ax*1024+az*424<=limit*1024 && az*1024+ax*424<=limit*1024;
}
int laneCenter(uint count,uint choice) {return (int(choice)*2-int(count)+1)*TUNNEL_HALF_PITCH;}
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
bool blocked(Obstacle obstacle,int x,int z,uint tick) {
    Pose p=pose(obstacle,tick);
    int dx=x-p.center;
    if(obstacle.kind==0u) {
        int orientedZ=(obstacle.flags&1u)!=0u?-z:z;
        int top=TUNNEL_SPIKE_HEIGHT/2-TUNNEL_SPIKE_CENTER;
        int bottom=top-TUNNEL_SPIKE_HEIGHT;
        // Conservative circle-vs-triangle half-planes, including the base.
        return orientedZ>=bottom-TUNNEL_PLAYER_RADIUS &&
            orientedZ<=top+TUNNEL_PLAYER_RADIUS &&
            abs(dx)*TUNNEL_SPIKE_HEIGHT+obstacle.extent*(orientedZ-top)<=
                TUNNEL_PLAYER_RADIUS*(TUNNEL_SPIKE_HEIGHT+obstacle.extent);
    }
    if(obstacle.kind==1u) return abs(dx)<obstacle.extent+TUNNEL_PLAYER_RADIUS &&
        abs(z)<TUNNEL_SLIDER_HALF_HEIGHT+TUNNEL_PLAYER_RADIUS;
    if(obstacle.kind==2u) {
        // Rotate BOTH coordinates into blade space. Vertical flight can escape it.
        int u=dx*p.cosine+z*p.sine,v=-dx*p.sine+z*p.cosine;
        int margin=TUNNEL_PLAYER_RADIUS*1026;
        return abs(u)<=obstacle.extent*1024+margin &&
               abs(v)<=TUNNEL_BLADE_HALF_THICKNESS*1024+margin;
    }
    // Four moving edges form a square opening, clipped by the octagonal tunnel.
    return abs(dx)+TUNNEL_PLAYER_RADIUS>=p.aperture ||
           abs(z)+TUNNEL_PLAYER_RADIUS>=p.aperture;
}
Target steeringTarget(Route route,int x,int z,uint eventTick,uint event,uint halfWindow) {
    Target best;best.x=0;best.z=0;
    int bestCost=2147483647;
    // Nine 2D targets, not nine horizontal lanes; keep this first controller simple.
    for(int ix=-1;ix<=1;++ix) for(int iz=-1;iz<=1;++iz) {
        int cx=ix*6500,cz=iz*6500;
        if(withinTube(cx,cz,TUNNEL_LOCAL_LIMIT) &&
           !blocked(route.obstacles[event],cx,cz,eventTick) &&
           !blocked(route.obstacles[event],cx,cz,eventTick-halfWindow) &&
           !blocked(route.obstacles[event],cx,cz,eventTick+halfWindow)) {
            int cost=abs(cx-x)+abs(cz-z)+(abs(cx)+abs(cz))/4;
            if(cost<bestCost) {bestCost=cost;best.x=cx;best.z=cz;}
        }
    }
    return best;
}
