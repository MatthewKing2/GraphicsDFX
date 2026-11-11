"""Configurable fictional reactor/foam rules, shared by live and sampled futures."""
from dataclasses import dataclass
import colorsys
import math
import numpy as np
from config import load_config


def random_next(x):
    x = np.asarray(x, dtype=np.uint32).copy()
    x ^= x << np.uint32(13)
    x ^= x >> np.uint32(17)
    x ^= x << np.uint32(5)
    return x


def seed_streams(seed, shape, start=0):
    # Absolute stream IDs make micro-batches bit-identical to one large batch.
    x = np.arange(start, start + np.prod(shape), dtype=np.uint32).reshape(shape) + np.uint32(1)
    x = x * np.uint32(0x9e3779b9) ^ np.uint32(seed & 0xffffffff)
    x ^= x >> np.uint32(16)
    x *= np.uint32(0x7feb352d)
    x ^= x >> np.uint32(15)
    x *= np.uint32(0x846ca68b)
    return (x ^ (x >> np.uint32(16))) | np.uint32(1)


@dataclass
class Forecast:
    probability: np.ndarray
    trials: int
    seconds: float
    rod_probability: np.ndarray
    rod_hits: np.ndarray
    batch_trials: int
    persistent_batch_bytes: int

    def result_packet(self):
        """Proposed device -> host ABI: uint32 trial count, uint16 hits per rod.

        The panel needs rod-level unions, not every trial's full thermal world.
        Framing/version IDs would be supplied by the eventual transport shell.
        """
        return np.asarray([self.trials], dtype="<u4").tobytes() + self.rod_hits.astype("<u2").tobytes()


class Rules:
    """Per-run wiring/rates: reloading cannot change an in-flight old forecast.

    Rates are compiled to integers once. The hot loop uses small counters,
    shifts, XORs, comparisons and static connections, not prediction feedback.
    This CPU/NumPy prototype does not establish FPGA fit or power savings.
    """
    def __init__(self, cfg=None):
        self.cfg = cfg or load_config()
        self.chambers = self.cfg.layout.reactors
        self.rods = self.cfg.layout.rods_per_reactor
        self.segments = self.cfg.layout.cells_per_rod
        self.per_chamber = self.rods * self.segments
        self.count = self.chambers * self.per_chamber
        self.total_rods = self.chambers * self.rods
        self.steps = math.ceil(self.cfg.forecast.horizon_seconds / self.cfg.timing.rod_step_seconds)
        self.horizon = self.steps * self.cfg.timing.rod_step_seconds
        self.groups, self.group_id = self.make_groups()
        self.neighbors, self.neighbor_valid = self.make_neighbors()
        self.power_period = max(2, self.ticks(self.cfg.power.demand_period_seconds))
        self.power_phase = np.arange(self.cfg.power.banks) * self.ticks(self.cfg.power.phase_stride_seconds, 0)
        self.power_phase += self.cfg.power.seed % self.power_period
        u = self.cfg.ui
        self.rod_colors = []
        for rod in range(self.total_rods):
            rgb = colorsys.hsv_to_rgb((rod*.61803398875+.04)%1, u.rod_color_saturation, u.rod_color_value)
            generated = "#"+"".join(f"{round(channel*255):02x}" for channel in rgb)
            self.rod_colors.append(u.rod_colors[rod] if rod < len(u.rod_colors) else generated)
        self.rates = {}
        for key, value in vars(self.cfg.thermal).items():
            if key.endswith("_256"):
                scale = (self.cfg.tuning.fault_scale if key.startswith("ignition") else
                         self.cfg.tuning.recovery_scale if "cooling" in key or "recovery" in key else
                         self.cfg.tuning.heating_scale)
                self.rates[key] = min(256, max(0, round(value * scale)))

    def ticks(self, seconds, minimum=1):
        return max(minimum, round(seconds / self.cfg.timing.rod_step_seconds))

    def node(self, c, r, s):
        return (c * self.rods + r) * self.segments + s

    def decode(self, i):
        return i // self.per_chamber, i // self.segments % self.rods, i % self.segments

    def linked(self, i):
        return self.groups[self.group_id[i]]

    def initial_sockets(self):
        # Interleave banks: each gets approximately the same rod count.
        rods = np.arange(self.total_rods)
        return (rods % self.cfg.power.banks) * self.cfg.power.plugs_per_bank + rods // self.cfg.power.banks

    def bank_name(self, bank):
        names = self.cfg.power.bank_names
        return names[bank] if bank < len(names) else f"SYSTEM {bank+1}"

    def bank_color(self, bank):
        colors = self.cfg.power.bank_colors
        return colors[bank % len(colors)]

    def rod_color(self, rod):
        return self.rod_colors[rod]

    def cable_color(self, rod):
        color = self.rod_color(rod)
        return "#"+"".join(f"{round(int(color[k:k+2],16)*self.cfg.ui.cable_brightness):02x}" for k in (1,3,5))

    def flow_speed(self, output_gw):
        """Exaggerated DISPLAY speed, not electrical current or thermal physics."""
        u = self.cfg.ui
        fraction = np.clip(np.asarray(output_gw)/self.cfg.power.rod_capacity_gw,0,1)
        speed = u.flow_min_speed_pixels_per_second + (u.flow_speed_pixels_per_second-u.flow_min_speed_pixels_per_second)*fraction**u.flow_speed_exponent
        return np.where(fraction>0,speed,0.)

    def bank_demand(self, tick):
        # Slow triangular load cycle: no jump at the wrap, integer arithmetic.
        phase = (tick + self.power_phase) % self.power_period
        ramp = np.minimum(phase, self.power_period-phase)
        p = self.cfg.power
        return p.demand_min + (p.demand_max-p.demand_min) * ramp / (self.power_period/2)

    def rod_load(self, tick, sockets=None):
        sockets = self.initial_sockets() if sockets is None else sockets
        banks = np.asarray(sockets) // self.cfg.power.plugs_per_bank
        counts = np.bincount(banks, minlength=self.cfg.power.banks)
        demand = self.bank_demand(tick)
        return demand[banks] / counts[banks]

    def rod_output(self, tick, sockets=None):
        """Delivered GW, capped at the configured maximum; rod_load is requested GW."""
        return np.minimum(self.rod_load(tick, sockets), self.cfg.power.rod_capacity_gw)

    def bank_deficit(self, tick, sockets=None):
        sockets = self.initial_sockets() if sockets is None else sockets
        counts = np.bincount(np.asarray(sockets)//self.cfg.power.plugs_per_bank,minlength=self.cfg.power.banks)
        return np.maximum(0., self.bank_demand(tick)-counts*self.cfg.power.rod_capacity_gw)

    def operating_heat(self, tick, sockets=None):
        p = self.cfg.power
        load = self.rod_output(tick, sockets)
        return np.minimum(self.cfg.thermal.critical_heat,
                          p.base_heat + np.floor(load*(p.heat_at_nominal_load-p.base_heat)/p.nominal_rod_load)).astype(np.int16)

    def diffuse(self, heat, rng, tick):
        """Conservative pair exchanges; no wraparound or heat transfer between rods.

        Alternating disjoint pairs avoid read/write races. Stochastic rounding
        lets even a one-unit heat difference slowly spread without float state.
        """
        h = heat.astype(np.int16).reshape(*heat.shape[:-1], self.total_rods, self.segments).copy()
        random = rng.reshape(h.shape)
        for k in range(self.cfg.thermal.diffusion_passes):
            start = (tick+k) & 1
            n = (self.segments-start)//2
            a = slice(start, start+2*n, 2)
            b = slice(start+1, start+2*n+1, 2)
            difference = h[..., a]-h[..., b]
            rounding = (random[..., a] >> ((k % 4)*8)) & 255
            magnitude = (np.abs(difference)*self.cfg.thermal.diffusion_256+rounding)//256
            flux = np.sign(difference)*magnitude
            h[..., a] -= flux
            h[..., b] += flux
        return h.reshape(heat.shape).astype(np.uint8)

    def make_groups(self):
        rng = np.random.default_rng(self.cfg.cooling.group_seed)
        mode = self.cfg.cooling.group_mode
        if mode == "cross_reactor":
            order = np.stack([rng.permutation(self.per_chamber) + c * self.per_chamber
                              for c in range(self.chambers)], axis=1).ravel()
        elif mode == "random":
            order = rng.permutation(self.count)
        else:
            groups = [np.arange(c*self.per_chamber+i, c*self.per_chamber+min(i+self.cfg.cooling.group_size, self.per_chamber))
                      for c in range(self.chambers) for i in range(0, self.per_chamber, self.cfg.cooling.group_size)]
            group_id = np.empty(self.count, dtype=np.intp)
            for g, members in enumerate(groups):
                group_id[members] = g
            return groups, group_id
        size = self.cfg.cooling.group_size
        groups = [order[i:i + size] for i in range(0, self.count, size)]
        group_id = np.empty(self.count, dtype=np.intp)
        for g, members in enumerate(groups):
            group_id[members] = g
        return groups, group_id

    def make_neighbors(self):
        n = self.cfg.connections.neighbors_per_cell
        neighbors = np.zeros((self.count, n), dtype=np.intp)
        valid = np.zeros_like(neighbors, dtype=bool)
        rng = np.random.default_rng(self.cfg.connections.seed)
        mode = self.cfg.connections.mode
        for i in range(self.count):
            c, r, s = self.decode(i)
            group = self.linked(i)
            p = int(np.flatnonzero(group == i)[0])
            circuit = [int(group[(p + d) % len(group)]) for d in range(1, len(group))]
            local = [self.node(c, r, (s + d) % self.segments) for d in range(1, self.segments)]
            local += [self.node(c, (r + d) % self.rods, s) for d in range(1, self.rods)]
            rng.shuffle(circuit)
            rng.shuffle(local)
            if mode == "mixed":
                candidates = []
                for k in range(max(len(circuit), len(local))):
                    candidates += circuit[k:k + 1] + local[k:k + 1]
            elif mode == "circuit":
                candidates = circuit
            elif mode == "local":
                candidates = local
            else:
                candidates = rng.permutation(self.count).tolist()
            chosen = list(dict.fromkeys(j for j in candidates if j != i))[:n]
            neighbors[i, :len(chosen)] = chosen
            valid[i, :len(chosen)] = True
        return neighbors, valid

    def transition(self, heat, fault, rng, tick=0, sockets=None, operating_rods=None):
        """Synchronous OLD -> NEXT, one world or [trials, cells]; no mutation."""
        if self.cfg.neutron.enabled:
            return self.neutron_transition(heat,fault,rng,tick,sockets,operating_rods)
        return self.legacy_transition(heat,fault,rng,tick,sockets,operating_rods)

    def neutron_transition(self, heat, fault, rng, tick=0, sockets=None, operating_rods=None):
        """Fission heat + axial diffusion + cooling + the existing runaway fuse.

        This same bounded transport kernel runs live and in every forecast.
        Source/load feedback is a fictional game control law, not nuclear data.
        No forecast percentage feeds back into the physical simulation.
        """
        from neutron import sample
        cfg,q = self.cfg,self.rates
        rng = random_next(rng)
        heat = self.diffuse(heat,rng,tick)
        if operating_rods is None:
            operating_rods = self.operating_heat(tick,sockets)
        operating = np.repeat(operating_rods,self.segments)
        counts,_ = sample(self,heat,rng,operating_rods)
        loaded = operating>=cfg.thermal.hot_heat
        runaway = ((fault&2)!=0)|(heat>=cfg.thermal.surge_heat)
        background = (heat<operating)&(((rng>>16)&255)<q["background_reheating_256"])
        rising = runaway&(((rng>>8)&255)<q["runaway_heating_256"])
        falling = (heat>operating)&~runaway&(((rng>>24)&255)<
                    np.where(loaded,q["cooling_loaded_256"],q["cooling_quiet_256"]))
        energy = round(cfg.neutron.heat_per_fission*cfg.tuning.heating_scale)
        h = np.clip(heat.astype(np.int32)+counts.astype(np.int32)*energy+
                    (background.astype(np.int32)+rising)*cfg.thermal.heat_step-
                    falling*cfg.thermal.cool_step,0,cfg.thermal.critical_heat).astype(np.uint8)
        runaway |= h>=cfg.thermal.surge_heat
        return h,(counts>0).astype(np.uint8)|(runaway.astype(np.uint8)<<1),rng

    def legacy_transition(self, heat, fault, rng, tick=0, sockets=None, operating_rods=None):
        """Original random-event model, retained for comparison/regressions."""
        cfg, q = self.cfg, self.rates
        rng = random_next(rng)
        # bit 0 = stochastic fault; bit 1 = latched nuclear runaway.
        heat = self.diffuse(heat, rng, tick)
        hot = heat >= cfg.thermal.hot_heat
        influence = np.zeros_like(heat, dtype=np.int32)
        for k in range(self.neighbors.shape[1]):
            influence += hot[..., self.neighbors[:, k]] & self.neighbor_valid[:, k]
        if operating_rods is None:
            operating_rods = self.operating_heat(tick, sockets)
        operating = np.repeat(operating_rods, self.segments)
        loaded = operating >= cfg.thermal.hot_heat
        ignition = (np.where(loaded, q["ignition_loaded_256"], q["ignition_quiet_256"]) +
                    q["ignition_per_hot_neighbor_256"] * influence + q["ignition_if_hot_256"] * hot)
        surge = heat >= cfg.thermal.surge_heat
        recovery = np.where(loaded & surge, q["fault_recovery_surge_256"],
                            np.where(loaded, q["fault_recovery_loaded_256"], q["fault_recovery_quiet_256"]))
        f = (((fault & 1) != 0) & ((rng & 255) >= recovery)) | (((rng >> 8) & 255) < ignition)
        runaway = ((fault & 2) != 0) | (heat >= cfg.thermal.surge_heat)
        rate = np.where(surge, q["surge_heating_256"], q["heating_256"] + q["heating_per_hot_neighbor_256"] * influence)
        rise = (f & (((rng >> 16) & 255) < rate)) | ((heat < operating) &
                (((rng >> 16) & 255) < q["background_reheating_256"]))
        rise |= runaway & (((rng >> 16) & 255) < q["runaway_heating_256"])
        fall = (~f & (((rng >> 24) & 255) < np.where(loaded, q["cooling_loaded_256"], q["cooling_quiet_256"]))) | (
            ~loaded & (heat > operating) & (((rng >> 24) & 255) < q["extra_quiet_cooling_256"]))
        fall &= ~rise & ~runaway & (heat > operating)
        # One rare event per rod heats a contiguous patch, not 128 unrelated
        # independent triggers. Event location/occurrence vary between futures.
        rod_random = rng.reshape(*rng.shape[:-1], self.total_rods, self.segments)[...,0]
        chance = np.where(operating_rods >= cfg.thermal.hot_heat,q["hotspot_loaded_256"],q["hotspot_quiet_256"])
        event = (rod_random & 255) < chance
        center = ((rod_random >> 8) % self.segments)[...,None]
        width = max(1, round(self.segments*cfg.thermal.hotspot_width_fraction))
        patch = event[...,None] & (np.abs(np.arange(self.segments).astype(np.int32)-center.astype(np.int32)) <= width)
        pulse = patch.reshape(heat.shape)*cfg.thermal.hotspot_heat_step
        h = np.clip(heat.astype(np.int16) + rise * cfg.thermal.heat_step - fall * cfg.thermal.cool_step + pulse,
                    0, cfg.thermal.critical_heat).astype(np.uint8)
        runaway |= h >= cfg.thermal.surge_heat
        return h, f.astype(np.uint8) | (runaway.astype(np.uint8) << 1), rng

    def forecast(self, heat, fault, tick=0, trials=None, cancel=None, sockets=None):
        # Routing is a snapshot, fixed throughout each hypothetical future.
        sockets = self.initial_sockets() if sockets is None else np.asarray(sockets).copy()
        trials = self.cfg.forecast.trials if trials is None else trials
        if not 1 <= trials <= 8192:
            raise ValueError("Forecast trials must be in [1,8192]")
        # The shared load trajectory depends on routing/time, not trial RNG.
        # Compile it ONCE on the host; the device kernel needs no GW divisions.
        schedule = np.empty((self.steps,self.total_rods), dtype=np.uint8)
        for offset in range(self.steps):
            if cancel is not None and cancel.is_set():
                return None
            schedule[offset] = self.operating_heat(tick + offset, sockets)
        cell_hits = np.zeros(self.count, np.uint32)
        rod_hits = np.zeros(self.total_rods, np.uint16)
        batch_trials = min(trials, self.cfg.forecast.trial_batch_size)
        for first in range(0, trials, batch_trials):
            size = min(batch_trials, trials-first)
            h = np.broadcast_to(heat, (size, self.count)).copy()
            f = np.broadcast_to(fault, h.shape).copy()
            rng = seed_streams(self.cfg.forecast.seed, h.shape, first*self.count)
            failed = h >= self.cfg.thermal.critical_heat
            for offset, operating in enumerate(schedule):
                if cancel is not None and cancel.is_set():
                    return None
                h, f, rng = self.transition(h, f, rng, tick + offset, sockets, operating)
                failed |= h >= self.cfg.thermal.critical_heat
            cell_hits += failed.sum(axis=0, dtype=np.uint32)
            rod_hits += failed.reshape(size, self.total_rods, self.segments).any(axis=-1).sum(axis=0, dtype=np.uint16)
        # Persistent h/f/RNG/failure state ONLY; excludes scratch, routing,
        # schedules, reduction counters, shell, and any extra hardware buffers.
        persistent = batch_trials*self.count*(1+1+4+1)
        return Forecast(cell_hits/trials, trials, self.horizon, rod_hits/trials,
                        rod_hits, batch_trials, persistent)

    def risk_color(self, probability):
        return next(color for lower, color in reversed(self.cfg.ui.risk_bands) if probability >= lower)

    def heat_color(self, heat):
        colors = self.cfg.ui.heat_colors
        position = min(len(colors)-1, max(0, float(heat))*(len(colors)-1)/self.cfg.thermal.critical_heat)
        i, fraction = int(position), position % 1
        a, b = colors[i], colors[min(i+1, len(colors)-1)]
        rgb = [round(int(a[k:k+2],16)*(1-fraction)+int(b[k:k+2],16)*fraction) for k in (1,3,5)]
        return "#"+"".join(f"{v:02x}" for v in rgb)


class Reactor:
    def __init__(self, rules=None):
        self.rules = rules or Rules()
        cfg = self.rules.cfg
        self.ticks = 0
        self.sockets = self.rules.initial_sockets()
        self.blackout = np.zeros(cfg.power.banks, dtype=float)
        self.heat_trend = np.zeros(self.rules.total_rods, dtype=float)
        self.rng = seed_streams(cfg.initial.seed, (self.rules.count,))
        span = cfg.initial.heat_max - cfg.initial.heat_min + 1
        # One base temperature per rod, with smooth random axial hot spots.
        bases = cfg.initial.heat_min + (self.rng.reshape(self.rules.total_rods, -1)[:,0] >> 8) % span
        self.heat = np.repeat(bases, self.rules.segments).astype(np.uint8)
        self.fault = (((self.rng >> 16) & 255) < cfg.initial.fault_chance_256).astype(np.uint8)
        self.opening_isolated = self.rules.segments//2
        self.opening_cluster = min(1, self.rules.total_rods-1)*self.rules.segments+self.rules.segments//2
        if cfg.initial.showcase:
            for rod in range(self.rules.total_rods):
                center = (.3 + .4*((int(self.rng[rod*self.rules.segments]) >> 8) & 255)/255)*max(1,self.rules.segments-1)
                peak = (cfg.initial.isolated_heat if rod == 0 else cfg.initial.cluster_target_heat if rod == 1 else
                        cfg.initial.cluster_heat_min + rod % (cfg.initial.cluster_heat_max-cfg.initial.cluster_heat_min+1))
                start = rod*self.rules.segments
                x = np.arange(self.rules.segments)
                width = max(1, self.rules.segments*cfg.initial.hotspot_width_fraction)
                profile = bases[rod] + (peak-int(bases[rod]))*np.exp(-.5*((x-center)/width)**2)
                self.heat[start:start+self.rules.segments] = np.clip(np.rint(profile), 0, cfg.thermal.critical_heat)

    @property
    def runaway_rods(self):
        return ((self.fault.reshape(self.rules.total_rods, self.rules.segments) & 2) != 0).any(axis=-1)

    def reroute(self, rod, socket):
        """Move to a free socket or atomically swap an occupied socket. Never unplug."""
        if not 0 <= rod < self.rules.total_rods or not 0 <= socket < self.rules.cfg.power.banks*self.rules.cfg.power.plugs_per_bank:
            raise ValueError("Rod or socket outside configured layout")
        occupied = np.flatnonzero(self.sockets == socket)
        old = int(self.sockets[rod])
        if len(occupied):
            self.sockets[int(occupied[0])] = old
        self.sockets[rod] = socket

    def step(self):
        before = self.heat.reshape(self.rules.total_rods,self.rules.segments).mean(axis=-1)
        self.heat, self.fault, self.rng = self.rules.transition(self.heat, self.fault, self.rng, self.ticks, self.sockets)
        after = self.heat.reshape(self.rules.total_rods,self.rules.segments).mean(axis=-1)
        self.heat_trend = (after-before)/self.rules.cfg.timing.rod_step_seconds
        underpowered = self.rules.bank_deficit(self.ticks,self.sockets) > 1e-6
        self.blackout = np.where(underpowered, self.blackout+self.rules.cfg.timing.rod_step_seconds, 0.)
        self.ticks += 1

    def settle_after_repair(self):
        for _ in range(self.rules.ticks(self.rules.cfg.timing.return_settle_seconds, 0)):
            self.step()
            if np.any(self.heat >= self.rules.cfg.thermal.critical_heat) or np.any(self.blackout >= self.rules.cfg.power.blackout_seconds):
                return False
        return True


class Repair:
    """Every foam-contacted cell cools. Each earns its own contact/pulse credit.

    Linked cooling broadcasts once per circuit per step unless stacking is
    enabled. Multiple wet cells cannot accidentally multiply the circuit bonus.
    """
    def __init__(self, reactor, target, travel=None, deadline=None, steps_per_second=None):
        self.reactor, self.rules, self.target = reactor, reactor.rules, target
        cfg = self.rules.cfg
        self.steps_per_second = cfg.foam.steps_per_second if steps_per_second is None else steps_per_second
        self.chamber, self.rod, self.segment = self.rules.decode(target)
        self.group = self.rules.linked(target).copy()
        self.travel = cfg.timing.travel_seconds if travel is None else travel
        self.remaining = cfg.timing.repair_deadline_seconds if deadline is None else deadline
        self.contacts = np.zeros(self.rules.count, dtype=np.uint32)
        self.rod_contact_frames = 0
        self.credits = np.zeros(self.rules.count, dtype=np.uint32)
        self.pulse_frames = max(1, round(cfg.cooling.pulse_seconds * self.steps_per_second))
        self.won = self.lost = False

    @property
    def contact_seconds(self):
        return self.rod_contact_frames / self.steps_per_second

    def advance(self, dt):
        if not (self.won or self.lost):
            self.remaining = max(0., self.remaining - dt)
            self.travel = max(0., self.travel - dt)
            self.lost = self.remaining <= 0

    def cool(self, contacts):
        if self.travel > 0 or self.won or self.lost:
            return
        cfg, rules = self.rules.cfg, self.rules
        touched = np.flatnonzero(contacts)
        touched = touched[touched // rules.per_chamber == self.chamber]
        if np.any(touched//rules.segments == self.target//rules.segments):
            self.rod_contact_frames += 1
        # Local foam cooling spreads along the same rod, never across its ends.
        if len(touched) and cfg.cooling.axial_radius_cells:
            expanded = []
            for i in touched:
                rod_start = int(i)//rules.segments*rules.segments
                expanded.extend(range(max(rod_start,int(i)-cfg.cooling.axial_radius_cells),
                                      min(rod_start+rules.segments,int(i)+cfg.cooling.axial_radius_cells+1)))
            touched = np.unique(expanded)
        self.contacts[touched] += 1
        self.credits[touched] += 1
        pulses = touched[self.credits[touched] >= self.pulse_frames]
        self.credits[pulses] -= self.pulse_frames
        if len(pulses):
            delta = np.zeros(rules.count, dtype=np.int32)
            delta[pulses] = cfg.cooling.direct_heat_per_pulse
            gids, counts = np.unique(rules.group_id[pulses], return_counts=True)
            for g, count in zip(gids, counts):
                amount = cfg.cooling.linked_heat_per_pulse * (int(count) if cfg.cooling.stack_linked_cooling else 1)
                members = rules.groups[g]
                delta[members] = np.maximum(delta[members], amount)
            self.reactor.heat = np.maximum(self.reactor.heat.astype(np.int32) - delta, 0).astype(np.uint8)
            cooled = (delta > 0) & (self.reactor.heat <= cfg.thermal.safe_heat)
            self.reactor.fault[cooled] &= np.uint8(2)
            for rod in range(rules.total_rods):
                section = slice(rod*rules.segments, (rod+1)*rules.segments)
                if self.reactor.heat[section].max() <= cfg.thermal.safe_heat:
                    self.reactor.fault[section] &= np.uint8(1)
        self.won = bool(self.contact_seconds >= cfg.timing.treatment_seconds and
                        self.reactor.heat.reshape(rules.total_rods,rules.segments)[self.target//rules.segments].max() <= cfg.thermal.safe_heat)


class Foam:
    """2D contact sandbox drawn as a room, NOT true 3D fluid simulation."""
    def __init__(self, rules, chamber, seed=None):
        self.rules, self.chamber = rules, chamber
        cfg = rules.cfg
        columns = min(cfg.ui.room_rod_columns, rules.rods)
        rows = math.ceil(rules.rods / columns)
        self.width = max(cfg.foam.grid_width, columns * 8)
        self.height = max(cfg.foam.grid_height, rows * math.ceil((rules.segments + 8)/.7))
        self.cells = np.zeros((self.height, self.width), dtype=np.uint8)
        self.rod_map = np.full(self.cells.shape, -1, dtype=np.int32)
        self.bounds = []
        self.rng = np.random.default_rng(cfg.foam.seed if seed is None else seed)
        self.ticks = 0
        for r in range(rules.rods):
            row, col = divmod(r, columns)
            pitch_x, pitch_y = self.width / columns, self.height / rows
            cx = (col + .5) * pitch_x
            half = max(1, int(pitch_x * .13))
            y0 = int((row + .12) * pitch_y)
            y1 = max(y0 + rules.segments, int((row + .86) * pitch_y))
            box = (max(0, int(cx) - half), y0, min(self.width, int(cx) + half), min(self.height - 1, y1))
            self.bounds.append(box)
            x0, y0, x1, y1 = box
            self.cells[y0:y1, x0:x1] = 3
            for s in range(rules.segments):
                a = y0 + s * (y1 - y0) // rules.segments
                b = y0 + (s + 1) * (y1 - y0) // rules.segments
                self.rod_map[a:b, x0:x1] = rules.node(chamber, r, s)
        self.yy, self.xx = np.ogrid[:self.height, :self.width]
        self.gas = None
        if rules.cfg.lattice.enabled:
            from lattice import LatticeGas
            self.gas = LatticeGas(self.cells.shape,self.cells==3,seed=cfg.foam.seed,
                                  gravity=cfg.lattice.gravity_256)

    def inject(self, x, y):
        # Aiming into a solid rod deposits on its nearest surface, rather than
        # losing the spray inside a wide rod when the layout/grid changes.
        ix, iy = int(np.clip(x, 0, self.width-1)), int(np.clip(y, 0, self.height-1))
        hit = int(self.rod_map[iy, ix])
        if hit >= 0:
            rod = self.rules.decode(hit)[1]
            x0, _, x1, _ = self.bounds[rod]
            x = x0-1 if x-x0 < x1-x else x1
        radius = self.rules.cfg.foam.spray_radius_cells
        spray = ((self.xx - x) ** 2 + (self.yy - y) ** 2 <= radius ** 2) & (self.cells == 0)
        if self.gas is not None:
            self.gas.inject_mask(spray)
        self.cells[spray] = 1

    def _move(self, dx, dy, eligible):
        mask = eligible & (np.roll(self.cells, (-dy, -dx), axis=(0, 1)) == 0)
        if dy:
            mask[-dy:] = False
        if dx > 0:
            mask[:, -dx:] = False
        elif dx < 0:
            mask[:, :-dx] = False
        values = self.cells[mask].copy()
        self.cells[mask] = 0
        self.cells[np.roll(mask, (dy, dx), axis=(0, 1))] = values

    def step(self):
        self.ticks += 1
        q = self.rules.cfg.foam
        if self.gas is not None:
            debris = self.cells==2
            for _ in range(self.rules.cfg.lattice.steps_per_tick):
                self.gas.step()
            self.cells = np.where(self.gas.solids,3,np.where(self.gas.channels!=0,1,0)).astype(np.uint8)
            self.cells[debris&~self.gas.solids&(self.cells==0)] = 2
        foam = self.cells == 1
        adjacent = np.zeros_like(foam)
        adjacent[1:] |= foam[:-1]
        adjacent[:-1] |= foam[1:]
        adjacent[:, 1:] |= foam[:, :-1]
        adjacent[:, :-1] |= foam[:, 1:]
        contacts = np.bincount(self.rod_map[adjacent & (self.rod_map >= 0)], minlength=self.rules.count)
        if self.gas is not None:
            # Debris is cosmetic in this 2D preview; the FHP channels own water.
            self._move(0,1,self.cells==2)
            if q.debris_every_steps and self.ticks%q.debris_every_steps==0:
                x=int(self.rng.integers(self.width))
                if self.cells[0,x]==0:
                    self.cells[0,x]=2
            return contacts
        if q.gravity_every_steps and self.ticks % q.gravity_every_steps == 0:
            self._move(0, 1, (self.cells == 1) | (self.cells == 2))
        direction = 1 if self.rng.integers(2) else -1
        if q.diagonal_every_steps and self.ticks % q.diagonal_every_steps == 0:
            self._move(direction, 1, (self.cells == 1) | (self.cells == 2))
        if q.flow_every_steps and self.ticks % q.flow_every_steps == 0:
            self._move(direction, 0, (self.cells == 1) & ((self.xx & 1) == (self.ticks & 1)))
        self.cells[-1] = 0
        if q.debris_every_steps and self.ticks % q.debris_every_steps == 0:
            x = int(self.rng.integers(self.width))
            if self.cells[0, x] == 0:
                self.cells[0, x] = 2
        return contacts
