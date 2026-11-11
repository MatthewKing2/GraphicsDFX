"""Six-bit FHP-style lattice gas, plus thin cylindrical coolant films.

The directions are axial hex-grid coordinates (q,r), not square-grid velocity
vectors. Collisions conserve particle count and hex-vector momentum. Downward
forcing, injection and open bottom drains are explicit external interactions.
All of this is a CPU reference; the surface geometry/source/drain extensions
are not a measured or synthesized LiteFury implementation.
"""
import math
import numpy as np

VELOCITY = ((1,0),(0,1),(-1,1),(-1,0),(0,-1),(1,-1))
POPCOUNT = np.array([i.bit_count() for i in range(64)],np.uint8)


def collision_table():
    table = np.tile(np.arange(64,dtype=np.uint8),(2,1))
    for noise in range(2):
        for pair in (0b001001,0b010010,0b100100):
            shift = 1 if noise else 5
            table[noise,pair] = ((pair<<shift)|(pair>>(6-shift)))&63
        table[noise,0b010101] = 0b101010
        table[noise,0b101010] = 0b010101
    return table


COLLISION = collision_table()


class LatticeGas:
    """One byte/site: low six bits are moving particles; geometry is separate."""
    def __init__(self, shape, solids=None, wrap_x=False, drain_bottom=True, seed=1, gravity=0):
        self.channels = np.zeros(shape,np.uint8)
        self.solids = np.zeros(shape,bool) if solids is None else np.broadcast_to(solids,shape).copy()
        self.wrap_x,self.drain_bottom,self.gravity = wrap_x,drain_bottom,gravity
        self.rng = (np.arange(math.prod(shape),dtype=np.uint32).reshape(shape)*np.uint32(0x9e3779b9)
                    +np.uint32(seed))|np.uint32(1)
        self.injected = self.drained = self.ticks = 0
        self.last_drain_sites = np.zeros(shape[:-2]+(shape[-1],),np.uint8)

    @property
    def density(self):
        return POPCOUNT[self.channels]

    @property
    def particle_count(self):
        return int(self.density.sum())

    def inject_mask(self, mask):
        """At most one new particle at each marked free site, directed down."""
        waiting = np.asarray(mask,bool)&~self.solids
        for direction in (1,2,0,3,4,5):
            chosen = waiting & ((self.channels & (1<<direction))==0)
            self.channels[chosen] |= np.uint8(1<<direction)
            waiting &= ~chosen
        added = int((np.asarray(mask,bool)&~self.solids&~waiting).sum())
        self.injected += added
        return added

    def step(self):
        self.rng ^= self.rng<<np.uint32(13)
        self.rng ^= self.rng>>np.uint32(17)
        self.rng ^= self.rng<<np.uint32(5)
        state = COLLISION[self.rng&1,self.channels].copy()
        # External body force: redirect only when the destination channel is
        # free. It can change momentum, but never creates/destroys particles.
        for shift,(source,target) in enumerate(((0,1),(3,2),(4,2),(5,1))):
            turn = ((state&(1<<source))!=0)&((state&(1<<target))==0)&(((self.rng>>(8*shift))&255)<self.gravity)
            state[turn] ^= np.uint8((1<<source)|(1<<target))
        output = np.zeros_like(state)
        self.last_drain_sites.fill(0)
        for direction,(dx,dy) in enumerate(VELOCITY):
            moving = (state&(1<<direction))!=0
            blocked = np.roll(self.solids,(-dy,-dx),axis=(-2,-1)).copy()
            if dy<0: blocked[...,0,:] = True
            if dy>0: blocked[...,-1,:] = not self.drain_bottom
            if not self.wrap_x:
                if dx<0: blocked[...,:,0] = True
                if dx>0: blocked[...,:,-1] = True
            bounce = moving&blocked
            allowed = moving&~blocked
            if dy>0 and self.drain_bottom:
                self.drained += int(allowed[...,-1,:].sum())
                self.last_drain_sites += allowed[...,-1,:]
            streamed = np.roll(allowed,(dy,dx),axis=(-2,-1))
            if dy>0: streamed[...,0,:] = False
            if dy<0: streamed[...,-1,:] = False
            if not self.wrap_x:
                if dx>0: streamed[...,:,0] = False
                if dx<0: streamed[...,:,-1] = False
            output |= streamed.astype(np.uint8)<<direction
            output |= bounce.astype(np.uint8)<<((direction+3)%6)
        output[self.solids] = 0
        self.channels = output
        self.ticks += 1


class SurfaceFilm:
    """Independent circumferential/axial films, mounted on real 3D rods.

    No fluid may jump directly between rods or behind an occluding rod. The
    host performs XYZ -> surface-address mapping before the compact command.
    Rejected impacts remain flying/rebounding packets in the host jet.
    """
    INVALID = np.uint32(0xffffffff)

    def __init__(self, world):
        self.world = world
        self.cfg = world.rules.cfg.lattice
        q = self.cfg
        self.gas = LatticeGas((world.rules.rods,q.film_rows,q.film_columns),wrap_x=True,
                              seed=q.seed,gravity=q.gravity_256)
        self.offsets = sorted(((dy,dx) for dy in range(-q.splash_radius,q.splash_radius+1)
                              for dx in range(-q.splash_radius,q.splash_radius+1)
                              if dx*dx+dy*dy<=q.splash_radius**2),key=lambda p:p[0]**2+p[1]**2)
        nodes,sites = [],[]
        for rod in range(world.rules.rods):
            for row in range(q.film_rows):
                lo = row*world.rules.segments//q.film_rows
                hi = ((row+1)*world.rules.segments+q.film_rows-1)//q.film_rows
                for segment in range(lo,hi):
                    nodes.append(world.rules.node(world.chamber,rod,segment))
                    sites.append(rod*q.film_rows+row)
        self.contact_nodes,self.contact_rows = np.asarray(nodes),np.asarray(sites)
        self.drain_sites = np.zeros((world.rules.rods,q.film_columns),np.uint16)

    @property
    def particle_count(self):
        return self.gas.particle_count

    def addresses(self, xyz):
        """Host/UI geometry operation, deliberately outside the device kernel."""
        w,q = self.world,self.cfg
        xyz = np.asarray(xyz)
        centers = np.asarray(w.centers)
        delta = xyz[:,None,(0,2)]-centers[None,:,:]
        distance = (delta*delta).sum(axis=-1)
        rod = distance.argmin(axis=1)
        radial = np.sqrt(distance[np.arange(len(xyz)),rod])
        valid = ((xyz[:,1]>=w.y0-1)&(xyz[:,1]<w.y1+1)&
                 (radial<=w.cfg.rod_radius+w.cfg.jet_splash_radius+1))
        chosen = delta[np.arange(len(xyz)),rod]
        angle = np.mod(np.arctan2(chosen[:,1],chosen[:,0]),math.tau)
        column = np.floor(angle*q.film_columns/math.tau).astype(int)%q.film_columns
        row = np.clip(((xyz[:,1]-w.y0)*q.film_rows/(w.y1-w.y0)).astype(int),0,q.film_rows-1)
        return np.where(valid,(rod*q.film_rows+row)*q.film_columns+column,self.INVALID).astype("<u4")

    def deposit(self, payload):
        if len(payload)%4:
            raise ValueError("Film impacts must contain uint32 surface addresses")
        ids = np.frombuffer(payload,dtype="<u4")
        shape = self.gas.channels.shape
        valid = ids!=self.INVALID
        if np.any(ids[valid]>=self.gas.channels.size):
            raise ValueError("Film impact address outside surface")
        safe = np.where(valid,ids,0)
        rods,rows,columns = np.unravel_index(safe,shape)
        accepted = np.zeros(len(ids),bool)
        flat = self.gas.channels.ravel()
        for dy,dx in self.offsets:
            target_row = rows+dy
            targets = (rods*shape[1]+np.clip(target_row,0,shape[1]-1))*shape[2]+(columns+dx)%shape[2]
            eligible = valid&~accepted&(target_row>=0)&(target_row<shape[1])
            for direction in (1,2,0,3,4,5):
                waiting = np.flatnonzero(eligible&~accepted&((flat[targets]&(1<<direction))==0))
                _,first = np.unique(targets[waiting],return_index=True)
                placed = waiting[first]
                flat[targets[placed]] |= np.uint8(1<<direction)
                accepted[placed] = True
            if np.all(accepted|~valid):
                break
        self.gas.injected += int(accepted.sum())
        return np.packbits(accepted,bitorder="little").tobytes()

    def contacts(self):
        w = self.world
        wet = self.gas.density.sum(axis=-1)
        return np.bincount(self.contact_nodes,weights=wet.ravel()[self.contact_rows],minlength=w.rules.count).astype(np.uint32)

    def step(self):
        self.drain_sites.fill(0)
        for _ in range(self.cfg.steps_per_tick):
            self.gas.step()
            self.drain_sites += self.gas.last_drain_sites
        return self.contacts()

    def samples(self, limit):
        w,q = self.world,self.cfg
        sites = np.argwhere(self.gas.channels!=0)
        if len(sites)>limit:
            sites = sites[np.linspace(0,len(sites)-1,limit,dtype=int)]
        if not len(sites):
            return np.empty((0,3))
        rod,row,column = sites.T
        angle = (column+.5)*math.tau/q.film_columns
        centers = np.asarray(w.centers)[rod]
        return np.column_stack((centers[:,0]+(w.cfg.rod_radius+.18)*np.cos(angle),
                                w.y0+(row+.5)*(w.y1-w.y0)/q.film_rows,
                                centers[:,1]+(w.cfg.rod_radius+.18)*np.sin(angle)))
