"""Small integer neutron-transport reference, NOT engineering reactor physics.

Each reactor has a separate axial 2D map: fuel strips separated by moderator.
Walks move one cell, leak, absorb, or fission. Fission deposits heat exactly
where it happened and starts two children in the next bounded generation.
There is no exponentially growing unbounded queue or cross-reactor transport.
No sampled path is added solely to manufacture an accelerator workload.
"""
import numpy as np


def xorshift(value):
    value = value.copy()
    value ^= value << np.uint32(13)
    value ^= value >> np.uint32(17)
    value ^= value << np.uint32(5)
    return value


def sample(rules, heat, rng, operating_rods, source_rods=None):
    """Pure OLD state -> per-cell fission counters and accounting diagnostics.

    Supports one world or a batch. Seeds depend on each world's cell RNGs,
    never its position within a batch, so forecast batching stays exact.
    Walkers are temporary within one thermal step; h/f/RNG remain its snapshot.
    """
    n = rules.cfg.neutron
    batch = heat.reshape(-1,rules.count)
    streams = rng.reshape(batch.shape)
    size = len(batch)
    per_rod = n.walkers_per_rod
    selected = np.arange(rules.total_rods) if source_rods is None else np.asarray(source_rods,dtype=int)
    if not len(selected) or np.any((selected<0)|(selected>=rules.total_rods)):
        raise ValueError("Neutron sources need valid rod indices")
    rods = np.repeat(selected,per_rod)
    chamber = rods//rules.rods
    local_rod = rods%rules.rods
    j = np.arange(len(rods),dtype=np.uint32)
    nodes = rods*rules.segments + j%rules.segments
    state = xorshift(streams[:,nodes] ^ (j+1)*np.uint32(0x9e3779b9) ^ np.uint32(n.seed)) | np.uint32(1)
    x = np.broadcast_to(local_rod*n.rod_pitch,batch[:,nodes].shape).copy().astype(np.int32)
    x += (state % n.fuel_width).astype(np.int32)
    y = ((state >> 8) % rules.segments).astype(np.int32)
    hottest = batch.reshape(size,rules.total_rods,rules.segments).argmax(axis=-1)[:,rods]
    radius = max(1,round(rules.segments*n.source_width_fraction))
    patch = np.clip(hottest+((state>>16)%(2*radius+1)).astype(np.int32)-radius,0,rules.segments-1)
    y = np.where((state>>24)<n.hot_source_256,patch,y)
    alive = np.ones_like(x,bool)
    counts = np.zeros(batch.shape,np.uint32)
    stats = dict(sources=int(alive.size),children=0,steps=0,fissions=0,absorbed=0,leaked=0,capped=0)
    offsets = np.arange(size)[:,None]*rules.count
    base = rules.cfg.power.base_heat
    scale = max(1,rules.cfg.power.heat_at_nominal_load-base)
    load_rate = n.fission_base_256 + n.fission_load_256*np.maximum(0,operating_rods.astype(np.int32)-base)//scale
    for generation in range(n.generations):
        fissioned = np.zeros_like(alive)
        site_x,site_y = x.copy(),y.copy()
        for _ in range(n.max_steps):
            if not alive.any():
                break
            stats["steps"] += int(alive.sum())
            # Completed histories must stop consuming RNG. Otherwise a child
            # seed would depend on the longest OTHER history in its batch.
            state = np.where(alive,xorshift(state),state)
            direction = state & 3
            x += ((direction==0).astype(np.int32)-(direction==1))*alive
            y += ((direction==2).astype(np.int32)-(direction==3))*alive
            inside = (x>=0)&(x<rules.rods*n.rod_pitch)&(y>=0)&(y<rules.segments)
            stats["leaked"] += int((alive&~inside).sum())
            alive &= inside
            rod = np.clip(x//n.rod_pitch,0,rules.rods-1)+chamber*rules.rods
            node = rod*rules.segments+np.clip(y,0,rules.segments-1)
            fuel = (x%n.rod_pitch)<n.fuel_width
            absorption = np.where(fuel,n.fuel_absorb_256,n.moderator_absorb_256)
            threshold = np.minimum(256-absorption,load_rate[rod] + n.hot_feedback_256*
                                  np.maximum(0,batch[np.arange(size)[:,None],node].astype(np.int32)-rules.cfg.thermal.hot_heat))
            draw = (state>>8)&255
            absorbed = alive & (draw<absorption)
            hit = alive & fuel & (draw>=absorption) & (draw<absorption+threshold)
            if hit.any():
                ids = (offsets+node)[hit]
                counts += np.bincount(ids,minlength=size*rules.count).reshape(counts.shape).astype(np.uint32)
                site_x[hit],site_y[hit] = x[hit],y[hit]
            stats["fissions"] += int(hit.sum())
            stats["absorbed"] += int(absorbed.sum())
            fissioned |= hit
            alive &= ~(hit|absorbed)
        stats["capped"] += int(alive.sum())
        if generation+1==n.generations or not fissioned.any():
            break
        x,y = np.repeat(site_x,2,axis=1),np.repeat(site_y,2,axis=1)
        alive = np.repeat(fissioned,2,axis=1)
        stats["children"] += int(alive.sum())
        state = xorshift(np.repeat(state,2,axis=1) ^ np.tile(np.array([0x85ebca6b,0xc2b2ae35],np.uint32),state.shape[1])) | np.uint32(1)
        chamber = np.repeat(chamber,2)
    return counts.reshape(heat.shape),stats
