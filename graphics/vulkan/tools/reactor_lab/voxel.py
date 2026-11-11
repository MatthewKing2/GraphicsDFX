"""True 3D, 2-bit occupancy prototype using the upstream sand rule verbatim.

Coordinates are [z, y, x], with +y down. Each step applies the original 2x2
rule to either XY or ZY planes; successive steps exchange material in BOTH
horizontal directions. This is NOT independent stacked 2D simulations.

The current demo adds bounded fixed-point flight packets before impact;
after collision they transfer their mass into the voxel world. Legacy local
pressure helpers remain for reference tests. There is no global neighbor
search or enormous 2x2x2 lookup table. This is NOT realistic fluid dynamics.

All code here runs on the laptop CPU. FPGA fit, timing, power, and GPU
comparisons are future work; packed payload size is not a resource report.
"""
from pathlib import Path
import math
import time
import numpy as np
from model import Reactor, Repair, Rules

EMPTY, DEBRIS, WATER, WALL = 0, 1, 2, 3  # Exactly the upstream encoding.
ROOT = Path(__file__).resolve().parents[4]


def upstream_table():
    root = next((ROOT/name/"fpga_graphics_dfx" for name in ("FPGA","fpga")
                 if (ROOT/name/"fpga_graphics_dfx").exists()), ROOT/"FPGA"/"fpga_graphics_dfx")
    path = root/"sim"/"sand_vectors"/"table.hex"
    if not path.exists():
        raise ValueError("Missing particle submodule: git submodule update --init --recursive")
    raw = np.array([int(v,16) for v in path.read_text(encoding="ascii").split()],dtype=np.uint16)
    if raw.shape != (512,) or np.any(raw>255):
        raise ValueError("Expected upstream 512-entry 8-bit block table")
    # Audited committed table: its random-bit halves are IDENTICAL. Do not
    # pretend unused RNG does useful particle work, or silently change rules.
    if not np.array_equal(raw[:256],raw[256:]):
        raise ValueError("Upstream random-bit rules changed: re-audit the 3D extension")
    return raw[:256].astype(np.uint8)


def plane_pass(cells, table, axes, offset):
    """In-place batch of the exact upstream nonoverlapping 2x2 rule.

    A wall shell prevents physical wraparound on shifted passes. Axes specify
    vertical then horizontal; the remaining volume axis is the batch axis.
    """
    remaining = next(axis for axis in range(3) if axis not in axes)
    view = cells.transpose(remaining,*axes)
    grid = np.roll(view,(-offset,-offset),axis=(1,2)) if offset else view
    index = (grid[:,0::2,0::2].astype(np.uint16) |
             grid[:,0::2,1::2].astype(np.uint16)<<2 |
             grid[:,1::2,0::2].astype(np.uint16)<<4 |
             grid[:,1::2,1::2].astype(np.uint16)<<6)
    output = table[index]
    grid[:,0::2,0::2] = output & 3
    grid[:,0::2,1::2] = (output>>2) & 3
    grid[:,1::2,0::2] = (output>>4) & 3
    grid[:,1::2,1::2] = output>>6
    if offset:
        view[:] = np.roll(grid,(offset,offset),axis=(1,2))


def pack_cells(cells):
    """Four 2-bit cells per byte, least-significant cell first; C-order z/y/x."""
    values = np.asarray(cells,dtype=np.uint8).ravel()
    if values.size%4 or np.any(values>WALL):
        raise ValueError("Packed volume needs a multiple of four 2-bit cells")
    q = values.reshape(-1,4)
    return (q[:,0] | q[:,1]<<2 | q[:,2]<<4 | q[:,3]<<6).tobytes()


def unpack_cells(payload, shape):
    raw = np.frombuffer(payload,dtype=np.uint8)
    if raw.size*4 != math.prod(shape):
        raise ValueError("Packed volume size does not match shape")
    return ((raw[:,None] >> np.array([0,2,4,6],np.uint8)) & 3).reshape(shape).copy()


class VoxelWorld:
    def __init__(self, rules, chamber=0):
        self.rules,self.cfg,self.chamber = rules,rules.cfg.voxel,chamber
        v = self.cfg
        self.cells = np.zeros((v.depth,v.height,v.width),np.uint8)
        self.node_map = np.full(self.cells.shape,-1,np.int32)
        self.table = upstream_table()
        self.ticks = self.injected = self.drained = self.debris_added = 0
        self.rng = np.random.default_rng(v.seed)
        self.cells[[0,-1],:,:] = WALL
        self.cells[:,[0,-1],:] = WALL
        self.cells[:,:,[0,-1]] = WALL
        columns = min(v.rod_columns,rules.rods)
        rows = math.ceil(rules.rods/columns)
        if min((v.width-4)/columns,(v.depth-4)/rows) < 2*v.rod_radius+3:
            raise ValueError("3D rods overlap: enlarge voxel width/depth, reduce radius, or change rod_columns/count")
        self.y0,self.y1 = round(v.height*v.rod_top_fraction),round(v.height*v.rod_bottom_fraction)
        if self.y1-self.y0 < 4:
            raise ValueError("3D rods need at least four voxels of height")
        self.centers = []
        zz,xx = np.ogrid[:v.depth,:v.width]
        for rod in range(rules.rods):
            row,col = divmod(rod,columns)
            x = round(2+(col+.5)*(v.width-4)/columns)
            z = round(v.depth-2-(row+.5)*(v.depth-4)/rows)
            self.centers.append((x,z))
            footprint = (xx-x)**2+(zz-z)**2 <= v.rod_radius**2
            for y in range(self.y0,self.y1):
                segment = min(rules.segments-1,(y-self.y0)*rules.segments//(self.y1-self.y0))
                self.cells[:,y,:][footprint] = WALL
                self.node_map[:,y,:][footprint] = rules.node(chamber,rod,segment)
        self.solids = self.cells == WALL
        self.beam = np.zeros_like(self.cells,dtype=bool)
        self.nozzle = np.array([v.width//2,round(v.height*.75),v.depth-3],dtype=int)
        self.aim = np.array([*self.nozzle],dtype=int)
        self.delta = np.zeros(3,dtype=int)
        self._aim_key = None
        self.last_step_ms = 0.

    @property
    def packed_bytes(self):
        return self.cells.size//4

    def aim_at(self, xyz):
        v = self.cfg
        aim = np.rint(np.clip(xyz,[1,1,1],[v.width-2,v.height-2,v.depth-2])).astype(int)
        key = tuple(aim)
        if key == self._aim_key:
            return
        self._aim_key,self.aim = key,aim
        self.delta = aim-self.nozzle
        length = max(1,int(np.abs(self.delta).max()))
        self.beam.fill(False)
        # Integer line command -> a narrow pressure corridor; only the SOURCE
        # injects water. Cells behind a wall are never spawned or pushed into.
        for step in range(length+1):
            point = (self.nozzle*length+self.delta*step+length//2)//length
            x,y,z = map(int,point)
            if self.solids[z,y,x] and step>0:
                break
            radius = v.jet_radius
            x0,x1=max(1,x-radius),min(v.width-1,x+radius+1)
            y0,y1=max(1,y-radius),min(v.height-1,y+radius+1)
            z0,z1=max(1,z-radius),min(v.depth-1,z+radius+1)
            zz,yy,xx=np.ogrid[z0:z1,y0:y1,x0:x1]
            self.beam[z0:z1,y0:y1,x0:x1] |= (xx-x)**2+(yy-y)**2+(zz-z)**2 <= radius**2
        self.beam &= ~self.solids

    def emit(self):
        x,y,z = self.nozzle
        radius = self.cfg.nozzle_radius
        z0,z1=max(1,z-radius),min(self.cfg.depth-1,z+radius+1)
        y0,y1=max(1,y-radius),min(self.cfg.height-1,y+radius+1)
        x0,x1=max(1,x-radius),min(self.cfg.width-1,x+radius+1)
        view = self.cells[z0:z1,y0:y1,x0:x1]
        zz,yy,xx = np.ogrid[z0:z1,y0:y1,x0:x1]
        mask = ((xx-x)**2+(yy-y)**2+(zz-z)**2 <= radius**2) & (view==EMPTY)
        count = int(np.count_nonzero(mask))
        view[mask] = WATER
        self.injected += count
        return count

    def _push_axis(self, axis, direction, parity):
        # Disjoint neighbor pairs: no destination conflicts or atomics.
        size = self.cells.shape[axis]
        a,b = [slice(None)]*3,[slice(None)]*3
        a[axis],b[axis]=slice(parity,size-1,2),slice(parity+1,size,2)
        if direction < 0:
            a,b = b,a
        a,b=tuple(a),tuple(b)
        source,target = self.cells[a],self.cells[b]
        move = (source==WATER)&(target==EMPTY)&self.beam[a]&self.beam[b]
        source[move],target[move] = EMPTY,WATER

    def contacts(self):
        wet = self.cells==WATER
        neighbor = np.zeros_like(wet)
        for axis in range(3):
            a,b = [slice(None)]*3,[slice(None)]*3
            a[axis],b[axis]=slice(1,None),slice(None,-1)
            neighbor[tuple(a)] |= wet[tuple(b)]
            neighbor[tuple(b)] |= wet[tuple(a)]
        nodes = self.node_map[neighbor & (self.node_map>=0)]
        return np.bincount(nodes,minlength=self.rules.count)

    def step(self, spraying=False):
        start = time.perf_counter()
        v = self.cfg
        if spraying:
            self.emit()
            length = max(1,int(np.abs(self.delta).max()))
            for repeat in range(v.jet_push_passes):
                phase = self.ticks*v.jet_push_passes+repeat
                for xyz_axis,axis in ((0,2),(1,1),(2,0)):
                    amount = abs(int(self.delta[xyz_axis]))
                    if (phase+1)*amount//length != phase*amount//length:
                        self._push_axis(axis,int(np.sign(self.delta[xyz_axis])),phase&1)
        # Alternating vertical planes + block phases couples the whole volume.
        plane_pass(self.cells,self.table,(1,2) if self.ticks%2==0 else (1,0),(self.ticks//2)&1)
        # Stronger downward transport after impact; water should run down a
        # rod, not spend most of its screen time hovering/spreading sideways.
        for repeat in range(v.runoff_gravity_passes):
            parity=(self.ticks+repeat)&1
            a=self.cells[:,parity:v.height-1:2,:]
            b=self.cells[:,parity+1:v.height:2,:]
            sink=((a==WATER)&(b==EMPTY))|((a==DEBRIS)&((b==WATER)|(b==EMPTY)))
            before=a[sink].copy()
            a[sink],b[sink]=b[sink],before
        self.ticks += 1
        if v.drain_every_steps and self.ticks%v.drain_every_steps==0:
            sump = self.cells[:,-2,:]
            remove = sump==WATER
            self.drained += int(np.count_nonzero(remove))
            sump[remove] = EMPTY
        if v.debris_every_steps and self.ticks%v.debris_every_steps==0:
            x,z = int(self.rng.integers(1,v.width-1)),int(self.rng.integers(1,v.depth-1))
            if self.cells[z,2,x]==EMPTY:
                self.cells[z,2,x] = DEBRIS
                self.debris_added += 1
        contacts = self.contacts()
        self.last_step_ms = (time.perf_counter()-start)*1000
        return contacts


class SpraySession:
    """Particle phase only: NEVER step or forecast reactor thermal dynamics."""
    def __init__(self, rules=None, chamber=0, rod=1, reactor=None):
        self.rules = rules or Rules()
        if not 0 <= chamber < self.rules.chambers or not 0 <= rod < self.rules.rods:
            raise ValueError("Chamber/rod outside configured layout (CLI uses 1-based numbers)")
        self.reactor = reactor or Reactor(self.rules)
        self.world = VoxelWorld(self.rules,chamber)
        self.jet = BallisticJet(self.world)
        self.select(rod)
        self.simulation_ms = 0.

    def select(self, rod):
        target = self.rules.node(self.world.chamber,rod,self.rules.segments//2)
        self.repair = Repair(self.reactor,target,travel=0,deadline=self.rules.cfg.voxel.demo_deadline_seconds,
                             steps_per_second=self.world.cfg.steps_per_second)
        x,z = self.world.centers[rod]
        profile = self.reactor.heat[target-self.rules.segments//2:target-self.rules.segments//2+self.rules.segments]
        y = self.world.y0+(int(profile.argmax())+.5)*(self.world.y1-self.world.y0)/self.rules.segments
        self.world.aim_at([x,y,z+self.world.cfg.rod_radius])
        self.jet.set_stream(self.world.nozzle,self.world.aim)

    def step(self, spraying=False):
        start = time.perf_counter()
        self.repair.advance(1/self.world.cfg.steps_per_second)
        self.jet.step(spraying)
        # Before impact: directed fixed-point flight. AFTER impact: local
        # upstream voxel rules only; no pressure corridor tugging runoff around.
        contacts = self.world.step(False)
        if spraying or self.world.cfg.cool_residual_water:
            self.repair.cool(contacts)
        self.simulation_ms = (time.perf_counter()-start)*1000
        return contacts


class BallisticJet:
    """Bounded Q10 integer flight packets; one packet becomes one water voxel.

    This ADDS explicit velocity/flight state to the old 2-bit occupancy model.
    It is not the original FPGA workload, nor a measured hardware design.
    Successful impacts transfer mass to the voxel world exactly once. Blocked
    impacts rebound; missed/expired packets are counted as escaped, never hidden.
    """
    Q = 1024

    def __init__(self, world):
        self.world,self.cfg=world,world.cfg
        self.positions=np.empty((0,3),np.int32)
        self.previous=self.positions.copy()
        self.velocities=self.positions.copy()
        self.age=np.empty(0,np.int32)
        self.entered=np.empty(0,bool)
        self.origin=world.nozzle.astype(float)
        self.direction=np.array([0.,0.,-1.])
        self.right=np.array([1.,0.,0.])
        self.up=np.array([0.,1.,0.])
        self.rng=np.random.default_rng(self.cfg.seed ^ 0x6174)
        self.emitted=self.deposited=self.escaped=0
        self.last_hits=np.empty((0,3),float)
        radius=self.cfg.jet_splash_radius
        self.offsets=sorted(((x,y,z) for x in range(-radius,radius+1)
                             for y in range(-radius,radius+1) for z in range(-radius,radius+1)
                             if x*x+y*y+z*z<=radius*radius),key=lambda p:sum(n*n for n in p))

    def set_stream(self, origin, target):
        self.origin=np.asarray(origin,float).copy()
        delta=np.asarray(target,float)-self.origin
        length=np.linalg.norm(delta)
        if length<.01: return
        self.direction=delta/length
        self.right=np.cross(self.direction,[0.,1.,0.])
        if np.linalg.norm(self.right)<.01: self.right=np.cross(self.direction,[1.,0.,0.])
        self.right/=np.linalg.norm(self.right)
        self.up=np.cross(self.direction,self.right)

    def emit(self):
        v=self.cfg
        count=min(v.jet_particles_per_step,v.jet_max_particles-len(self.positions))
        if count<=0: return
        angle=self.rng.uniform(0,math.tau,count)
        radius=np.sqrt(self.rng.random(count))
        a,b=(radius*np.cos(angle))[:,None],(radius*np.sin(angle))[:,None]
        sideways=a*self.right+b*self.up
        start=self.origin+sideways*v.nozzle_radius*.85
        direction=self.direction+sideways*math.tan(math.radians(v.jet_spread_degrees))
        velocity=direction*v.jet_speed_voxels_per_second/v.steps_per_second
        points=np.rint(start*self.Q).astype(np.int32)
        self.positions=np.concatenate((self.positions,points))
        self.previous=np.concatenate((self.previous,points))
        self.velocities=np.concatenate((self.velocities,np.rint(velocity*self.Q).astype(np.int32)))
        self.age=np.concatenate((self.age,np.zeros(count,np.int32)))
        self.entered=np.concatenate((self.entered,np.zeros(count,bool)))
        self.emitted+=count

    def _deposit(self, packet_ids, free_positions, alive):
        w=self.world
        base=np.rint(free_positions/self.Q).astype(int)
        flat=w.cells.ravel()
        points=base[:,None,:]+np.asarray(self.offsets)[None,:,:]
        bounds=np.array([w.cfg.width-1,w.cfg.height-1,w.cfg.depth-1])
        valid=np.all((points>=1)&(points<bounds),axis=2)
        # A splash may spread around a surface, but cannot deposit through a
        # thin wall merely because an empty cell is within the search radius.
        for part in range(1,self.cfg.jet_splash_radius+1):
            between=base[:,None,:]+np.rint(np.asarray(self.offsets)[None,:,:]*part/self.cfg.jet_splash_radius).astype(int)
            between=np.clip(between,[0,0,0],bounds)
            valid &= ~w.solids[between[:,:,2],between[:,:,1],between[:,:,0]]
        safe=np.clip(points,[0,0,0],bounds)
        indices=(safe[:,:,2]*w.cfg.height+safe[:,:,1])*w.cfg.width+safe[:,:,0]
        searching=np.any(valid & (flat[indices]==EMPTY),axis=1)
        deposited=np.zeros(len(packet_ids),bool)
        for column in range(len(self.offsets)):
            if not np.any(searching): break
            candidates=np.flatnonzero(searching & valid[:,column] & (flat[indices[:,column]]==EMPTY))
            targets=indices[candidates,column]
            _,first=np.unique(targets,return_index=True)
            placed=candidates[first]
            flat[targets[first]]=WATER
            alive[packet_ids[placed]]=False
            self.deposited+=len(placed)
            w.injected+=len(placed)
            deposited[placed]=True
            searching[placed]=False
            if column%8==7 and column+1<len(self.offsets):
                ids=np.flatnonzero(searching)
                searching[ids]=np.any(valid[ids,column+1:] & (flat[indices[ids,column+1:]]==EMPTY),axis=1)
        remaining=np.flatnonzero(~deposited)
        if len(remaining):
            ids=packet_ids[remaining]
            alive[ids]=True
            self.positions[ids]=free_positions[remaining]
            self.velocities[ids]=(-self.velocities[ids]//3)
            self.velocities[ids,1]+=self.Q//3

    def step(self, spraying):
        if spraying: self.emit()
        if not len(self.positions): return
        w,v=self.world,self.cfg
        self.previous=self.positions.copy()
        self.velocities[:,1]+=round(v.jet_gravity_voxels_per_second2*self.Q/v.steps_per_second**2)
        next_positions=self.positions+self.velocities
        alive=np.ones(len(self.positions),bool)
        free=self.positions.copy()
        subdivisions=max(1,int(np.ceil(np.max(np.abs(self.velocities))/self.Q)))
        impacts=[]
        # Swept samples prevent a fast stream tunneling through thin rods.
        for part in range(1,subdivisions+1):
            ids=np.flatnonzero(alive)
            if not len(ids): break
            positions=self.previous[ids]+self.velocities[ids]*part//subdivisions
            xyz=np.rint(positions/self.Q).astype(int)
            inside=np.all((xyz>=1)&(xyz<np.array([v.width-1,v.height-1,v.depth-1])),axis=1)
            leaving=self.entered[ids]&~inside
            alive[ids[leaving]]=False
            self.escaped+=int(leaving.sum())
            self.entered[ids[inside]]=True
            interior=ids[inside]
            points=xyz[inside]
            hit=w.solids[points[:,2],points[:,1],points[:,0]]
            hit_ids=interior[hit]
            if len(hit_ids):
                impacts.extend(hit_ids.tolist())
                alive[hit_ids]=False
            safe=ids[~leaving & ~np.isin(ids,hit_ids)]
            free[safe]=positions[~leaving & ~np.isin(ids,hit_ids)]
        self.positions=next_positions
        if impacts:
            hits=np.asarray(impacts,int)
            self.last_hits=free[hits]/self.Q
            self._deposit(hits,free[hits],alive)
            # Deposited packets stay dead; unplaced packets rebound and survive.
            # _deposit explicitly restores only the unplaced packets below.
        else:
            self.last_hits=np.empty((0,3),float)
        self.age+=1
        expired=alive & (self.age>math.ceil(v.jet_max_lifetime_seconds*v.steps_per_second))
        self.escaped+=int(expired.sum())
        alive[expired]=False
        self.positions=self.positions[alive]
        self.previous=self.previous[alive]
        self.velocities=self.velocities[alive]
        self.age=self.age[alive]
        self.entered=self.entered[alive]
