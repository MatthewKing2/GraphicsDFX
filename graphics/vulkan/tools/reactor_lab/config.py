"""Commented TOML settings, strict validation, and partial preset overrides."""
from copy import deepcopy
from pathlib import Path
from types import SimpleNamespace
import ast
import math
import re
import tomllib

HERE = Path(__file__).resolve().parent
DEFAULT_PATH = HERE / "settings.toml"
_STARTUP_DEFAULTS = tomllib.loads(DEFAULT_PATH.read_text(encoding="utf-8"))
# Field TYPES are fixed in code; all tuning VALUES live in settings.toml.
# This also catches typos/types in the main file, not just in preset overrides.
_INTEGER_FIELDS = {
    "layout": "reactors rods_per_reactor cells_per_rod",
    "power": "banks plugs_per_bank seed base_heat heat_at_nominal_load",
    "initial": "seed heat_min heat_max fault_chance_256 isolated_heat cluster_target_heat cluster_heat_min cluster_heat_max",
    "thermal": "hot_heat surge_heat critical_heat safe_heat heat_step cool_step ignition_loaded_256 ignition_quiet_256 ignition_per_hot_neighbor_256 ignition_if_hot_256 fault_recovery_loaded_256 fault_recovery_surge_256 fault_recovery_quiet_256 heating_256 heating_per_hot_neighbor_256 surge_heating_256 background_reheating_256 cooling_loaded_256 cooling_quiet_256 extra_quiet_cooling_256 runaway_heating_256 diffusion_256 diffusion_passes hotspot_loaded_256 hotspot_quiet_256 hotspot_heat_step",
    "connections": "neighbors_per_cell seed",
    "cooling": "group_size group_seed direct_heat_per_pulse linked_heat_per_pulse axial_radius_cells",
    "forecast": "trials seed trial_batch_size",
    "neutron": "walkers_per_rod max_steps generations rod_pitch fuel_width fuel_absorb_256 moderator_absorb_256 fission_base_256 fission_load_256 hot_feedback_256 hot_source_256 heat_per_fission seed",
    "lattice": "film_rows film_columns steps_per_tick gravity_256 splash_radius max_drawn_drips seed",
    "foam": "steps_per_second grid_width grid_height gravity_every_steps diagonal_every_steps flow_every_steps debris_every_steps seed",
    "voxel": "width height depth steps_per_second rod_radius rod_columns nozzle_radius jet_radius jet_push_passes drain_every_steps debris_every_steps seed render_bands max_drawn_particles frame_milliseconds jet_particles_per_step jet_max_particles jet_splash_radius runoff_gravity_passes max_drawn_jet_particles max_simulation_steps_per_frame",
    "ui": "frame_milliseconds panel_columns panel_rod_columns room_rod_columns cell_width_pixels cell_height_pixels max_drawn_particles rod_height_pixels bank_columns bank_width_pixels bank_height_pixels cable_gap_pixels cable_width_pixels socket_radius_pixels spark_count",
}
_FLOAT_FIELDS = {
    "tuning": "fault_scale heating_scale recovery_scale",
    "power": "demand_period_seconds phase_stride_seconds blackout_seconds rod_capacity_gw demand_min demand_max nominal_rod_load",
    "thermal": "hotspot_width_fraction",
    "initial": "hotspot_width_fraction",
    "cooling": "pulse_seconds",
    "timing": "rod_step_seconds travel_seconds treatment_seconds repair_deadline_seconds return_settle_seconds",
    "forecast": "horizon_seconds refresh_seconds",
    "neutron": "source_width_fraction",
    "lattice": "drip_lifetime_seconds drip_speed",
    "foam": "spray_radius_cells",
    "voxel": "rod_top_fraction rod_bottom_fraction demo_deadline_seconds jet_speed_voxels_per_second jet_gravity_voxels_per_second2 jet_spread_degrees jet_max_lifetime_seconds nozzle_aim_offset_pixels water_render_scale water_render_opacity jet_render_scale jet_render_opacity",
    "ui": "gradient_smoothing_cells flow_speed_pixels_per_second flow_arrow_spacing_pixels rod_color_saturation rod_color_value heat_trend_deadband flow_min_speed_pixels_per_second flow_speed_exponent cable_brightness alarm_flash_hz spark_length_pixels",
}
_BOOL_FIELDS = {"initial": "showcase", "cooling": "stack_linked_cooling require_active_spray", "voxel": "cool_residual_water",
                "neutron": "enabled", "lattice": "enabled"}
_STRING_FIELDS = {
    "connections": "mode", "cooling": "group_mode",
    "ui": "foam_color debris_color background_color panel_color text_color muted_color linked_color selected_color accent_color flow_color alarm_color alarm_dim_color spark_color",
}
_LIST_FIELDS = {"ui": "risk_bands heat_colors rod_colors", "power": "bank_names bank_colors"}
_SCHEMA = {}
for kind, sections in ((int, _INTEGER_FIELDS), (float, _FLOAT_FIELDS), (bool, _BOOL_FIELDS),
                       (str, _STRING_FIELDS), (list, _LIST_FIELDS)):
    for section, fields in sections.items():
        _SCHEMA.setdefault(section, {}).update({field: kind for field in fields.split()})


def merge(base, changes, prefix=""):
    result = deepcopy(base)
    for key, value in changes.items():
        path = f"{prefix}{key}"
        if key not in base:
            raise ValueError(f"Unknown setting: {path}")
        if isinstance(base[key], dict):
            if not isinstance(value, dict):
                raise ValueError(f"{path} must be a section")
            result[key] = merge(base[key], value, path + ".")
        else:
            section = prefix.rstrip(".")
            expected = _SCHEMA.get(section, {}).get(key, type(base[key]))
            formula = expected in (int, float) and isinstance(value,str) and value.startswith("=")
            valid = formula or (type(value) in (int, float) if expected is float else type(value) is expected)
            if not valid:
                raise ValueError(f"{path} has the wrong type (expected {expected.__name__})")
            result[key] = value
    return result


def resolve_formulas(source):
    """Small arithmetic language, NOT Python eval. Resolve AFTER preset merging.

    Numeric settings can be '= section.key * 0.8'. Only numeric references,
    arithmetic, min/max/round/ceil/floor are accepted; cycles fail clearly.
    """
    data, resolving, done = deepcopy(source), set(), set()
    functions = {"min": min, "max": max, "round": round, "ceil": math.ceil, "floor": math.floor}
    def field(section, key):
        path = f"{section}.{key}"
        expected = _SCHEMA.get(section, {}).get(key)
        if expected not in (int,float):
            raise ValueError(f"Formula reference must be a numeric setting: {path}")
        if path in resolving:
            raise ValueError(f"Circular formula dependency: {path}")
        if path in done:
            return data[section][key]
        value = data[section][key]
        if isinstance(value,str) and value.startswith("="):
            resolving.add(path)
            try:
                if len(value) > 512:
                    raise ValueError("Formula is too long")
                tree = ast.parse(value[1:].strip(), mode="eval")
                if sum(1 for _ in ast.walk(tree)) > 100:
                    raise ValueError("Formula is too complex")
                value = number(tree.body)
                if expected is int:
                    if value != round(value):
                        raise ValueError("Integer setting needs an integral result; use round/ceil/floor")
                    value = int(value)
                data[section][key] = value
            except (SyntaxError, ArithmeticError, TypeError, ValueError) as e:
                raise ValueError(f"Invalid formula for {path}: {e}") from e
            finally:
                resolving.remove(path)
        done.add(path)
        return value
    def number(node):
        if isinstance(node,ast.Constant) and type(node.value) in (int,float):
            value = node.value
        elif isinstance(node,ast.Attribute) and isinstance(node.value,ast.Name):
            value = field(node.value.id,node.attr)
        elif isinstance(node,ast.UnaryOp) and isinstance(node.op,(ast.UAdd,ast.USub)):
            value = number(node.operand) * (-1 if isinstance(node.op,ast.USub) else 1)
        elif isinstance(node,ast.BinOp):
            a,b = number(node.left),number(node.right)
            if isinstance(node.op,ast.Add): value = a+b
            elif isinstance(node.op,ast.Sub): value = a-b
            elif isinstance(node.op,ast.Mult): value = a*b
            elif isinstance(node.op,ast.Div): value = a/b
            elif isinstance(node.op,ast.FloorDiv): value = a//b
            elif isinstance(node.op,ast.Mod): value = a%b
            else: raise ValueError("Unsupported arithmetic operator")
        elif isinstance(node,ast.Call) and isinstance(node.func,ast.Name) and node.func.id in functions:
            if node.keywords or not 1 <= len(node.args) <= 8:
                raise ValueError("Use 1-8 positional numeric arguments")
            value = functions[node.func.id](*(number(arg) for arg in node.args))
        else:
            raise ValueError("Only arithmetic, section.key, min/max/round/ceil/floor are allowed")
        if type(value) not in (int,float) or not math.isfinite(value) or abs(value)>1e12:
            raise ValueError("Formula result must be a finite number of magnitude <= 1e12")
        return value
    for section, fields in _SCHEMA.items():
        for key, expected in fields.items():
            if expected in (int,float) and section in data and key in data[section]:
                field(section,key)
    return data


def namespace(data):
    return SimpleNamespace(**{k: namespace(v) if isinstance(v, dict) else v for k, v in data.items()})


class Config:
    def __init__(self, data):
        self.source = deepcopy(data)  # Keep formulas for further overrides/editor saves.
        data = resolve_formulas(data)
        if set(data) != set(_SCHEMA):
            raise ValueError(f"Unexpected or missing sections: {set(data) ^ set(_SCHEMA)}")
        for section, fields in _SCHEMA.items():
            if not isinstance(data[section], dict) or set(data[section]) != set(fields):
                raise ValueError(f"Unexpected or missing fields in [{section}]")
            for key, expected in fields.items():
                value = data[section][key]
                valid = type(value) in (int, float) if expected is float else type(value) is expected
                if not valid:
                    raise ValueError(f"{section}.{key} must be {expected.__name__}")
        self.data = deepcopy(data)
        for k, v in data.items():
            setattr(self, k, namespace(v))
        self.validate()

    def validate(self):
        def bounded(value, lo, hi, name):
            if not isinstance(value, (int, float)) or not math.isfinite(value) or not lo <= value <= hi:
                raise ValueError(f"{name} must be between {lo} and {hi}")
        for section, values in self.data.items():
            for key, value in values.items():
                if key.endswith("_256"):
                    bounded(value, 0, 256, f"{section}.{key}")
        for key, hi in (("reactors", 16), ("rods_per_reactor", 128), ("cells_per_rod", 512)):
            bounded(getattr(self.layout, key), 1, hi, "layout." + key)
        count = self.layout.reactors * self.layout.rods_per_reactor * self.layout.cells_per_rod
        bounded(count, 1, 8192, "total cells")
        for key, lo, hi in (("walkers_per_rod",1,64),("max_steps",1,256),("generations",1,3),
                            ("rod_pitch",2,16),("fuel_width",1,self.neutron.rod_pitch-1),
                            ("heat_per_fission",0,16),("seed",0,0xffffffff)):
            bounded(getattr(self.neutron,key),lo,hi,"neutron."+key)
        bounded(self.neutron.source_width_fraction,.01,1,"neutron.source_width_fraction")
        for key, lo, hi in (("film_rows",8,256),("film_columns",8,256),("steps_per_tick",1,8),
                            ("splash_radius",0,8),("max_drawn_drips",0,1000),("seed",0,0xffffffff)):
            bounded(getattr(self.lattice,key),lo,hi,"lattice."+key)
        bounded(self.lattice.drip_lifetime_seconds,.05,5,"lattice.drip_lifetime_seconds")
        bounded(self.lattice.drip_speed,0,100,"lattice.drip_speed")
        bounded(self.layout.rods_per_reactor*self.lattice.film_rows*self.lattice.film_columns,
                1,1048576,"lattice sites per chamber")
        if self.neutron.enabled and (self.forecast.trials*self.layout.reactors*self.layout.rods_per_reactor*
                                     self.neutron.walkers_per_rod*2**(self.neutron.generations-1)>2_000_000):
            raise ValueError("Too many forecast neutron walkers; reduce trials, sources, generations or rods")
        p = self.power
        bounded(p.banks, 1, 64, "power.banks")
        bounded(p.plugs_per_bank, 1, 128, "power.plugs_per_bank")
        if p.banks * p.plugs_per_bank < self.layout.reactors * self.layout.rods_per_reactor:
            raise ValueError("Not enough bank plugs for all rods: increase power.banks or power.plugs_per_bank")
        if p.banks > self.layout.reactors * self.layout.rods_per_reactor and p.demand_max > 0:
            raise ValueError("Each powered bank needs at least one rod; reduce power.banks or add rods")
        bounded(p.rod_capacity_gw, .01, 1e6, "power.rod_capacity_gw")
        bounded(p.demand_min, 0, 1e9, "power.demand_min")
        bounded(p.demand_max, p.demand_min, 1e9, "power.demand_max")
        bounded(p.nominal_rod_load, .01, 1e6, "power.nominal_rod_load")
        bounded(p.base_heat, 0, self.thermal.critical_heat-1, "power.base_heat")
        bounded(p.heat_at_nominal_load, p.base_heat, self.thermal.critical_heat-1, "power.heat_at_nominal_load")
        bounded(p.demand_period_seconds, self.timing.rod_step_seconds, 36000, "power.demand_period_seconds")
        bounded(p.phase_stride_seconds, 0, 36000, "power.phase_stride_seconds")
        bounded(p.blackout_seconds, .1, 3600, "power.blackout_seconds")
        if not all(isinstance(s, str) and s for s in p.bank_names):
            raise ValueError("power.bank_names must contain nonempty strings")
        if not p.bank_colors:
            raise ValueError("power.bank_colors needs at least one color")
        bounded(self.initial.hotspot_width_fraction, .01, 1, "initial.hotspot_width_fraction")
        bounded(self.thermal.hotspot_width_fraction, .01, 1, "thermal.hotspot_width_fraction")
        bounded(self.thermal.hotspot_heat_step, 0, 16, "thermal.hotspot_heat_step")
        bounded(self.ui.gradient_smoothing_cells, 0, 64, "ui.gradient_smoothing_cells")
        bounded(self.ui.flow_speed_pixels_per_second, 0, 2000, "ui.flow_speed_pixels_per_second")
        bounded(self.ui.flow_min_speed_pixels_per_second, 0, self.ui.flow_speed_pixels_per_second, "ui.flow_min_speed_pixels_per_second")
        bounded(self.ui.flow_speed_exponent, .1, 8, "ui.flow_speed_exponent")
        bounded(self.ui.cable_brightness, .05, 1, "ui.cable_brightness")
        bounded(self.ui.alarm_flash_hz, .1, 2, "ui.alarm_flash_hz")
        bounded(self.ui.spark_count, 0, 32, "ui.spark_count")
        bounded(self.ui.spark_length_pixels, 1, 60, "ui.spark_length_pixels")
        bounded(self.ui.flow_arrow_spacing_pixels, 20, 500, "ui.flow_arrow_spacing_pixels")
        bounded(self.ui.rod_color_saturation, 0, 1, "ui.rod_color_saturation")
        bounded(self.ui.rod_color_value, 0, 1, "ui.rod_color_value")
        bounded(self.ui.heat_trend_deadband, 0, 1, "ui.heat_trend_deadband")
        v = self.voxel
        for key in ("width","height","depth"):
            bounded(getattr(v,key),16,128,"voxel."+key)
            if getattr(v,key)%2:
                raise ValueError("Voxel dimensions must be even for disjoint 2x2 updates")
        bounded(v.width*v.height*v.depth,1,1048576,"voxel volume cells")
        for key,lo,hi in (("steps_per_second",1,240),("rod_radius",1,12),("rod_columns",1,32),
                          ("nozzle_radius",1,5),("jet_radius",1,8),("jet_push_passes",0,12),
                          ("drain_every_steps",0,10000),("debris_every_steps",0,10000),
                          ("seed",0,0xffffffff),("render_bands",4,128),("max_drawn_particles",100,10000),
                          ("frame_milliseconds",20,250)):
            bounded(getattr(v,key),lo,hi,"voxel."+key)
        if not .05 <= v.rod_top_fraction < v.rod_bottom_fraction <= .95:
            raise ValueError("Need .05 <= voxel rod_top_fraction < rod_bottom_fraction <= .95")
        bounded(v.demo_deadline_seconds,self.timing.treatment_seconds+1,3600,"voxel.demo_deadline_seconds")
        for key,lo,hi in (("jet_particles_per_step",1,512),("jet_max_particles",64,16384),
                          ("runoff_gravity_passes",0,12),
                          ("jet_splash_radius",1,8),("jet_speed_voxels_per_second",1,500),
                          ("jet_gravity_voxels_per_second2",0,500),("jet_spread_degrees",0,30),
                          ("jet_max_lifetime_seconds",.1,5),("nozzle_aim_offset_pixels",0,300),
                          ("water_render_scale",.5,4)):
            bounded(getattr(v,key),lo,hi,"voxel."+key)
        for key,lo,hi in (("water_render_opacity",0,1),("jet_render_opacity",0,1),
                          ("jet_render_scale",.1,4),("max_drawn_jet_particles",16,10000),
                          ("max_simulation_steps_per_frame",1,32)):
            bounded(getattr(v,key),lo,hi,"voxel."+key)
        bounded(self.thermal.diffusion_256, 0, 128, "thermal.diffusion_256 (stable pairwise diffusion)")
        bounded(self.thermal.diffusion_passes, 0, 16, "thermal.diffusion_passes")
        bounded(self.cooling.axial_radius_cells, 0, 512, "cooling.axial_radius_cells")
        t = self.thermal
        if not 0 <= t.safe_heat < t.hot_heat <= t.surge_heat < t.critical_heat <= 255:
            raise ValueError("Need 0 <= safe_heat < hot_heat <= surge_heat < critical_heat <= 255")
        for key in ("heat_step", "cool_step"):
            bounded(getattr(t, key), 1, 16, "thermal." + key)
        for key in ("heat_min", "heat_max", "isolated_heat", "cluster_target_heat", "cluster_heat_min", "cluster_heat_max"):
            bounded(getattr(self.initial, key), 0, t.critical_heat - 1, "initial." + key)
        if self.initial.heat_min > self.initial.heat_max or self.initial.cluster_heat_min > self.initial.cluster_heat_max:
            raise ValueError("Initial heat minima must not exceed maxima")
        for key in ("fault_scale", "heating_scale", "recovery_scale"):
            bounded(getattr(self.tuning, key), 0, 8, "tuning." + key)
        for key in ("seed",):
            for section in ("initial", "connections", "forecast", "foam", "power"):
                bounded(getattr(getattr(self, section), key), 0, 0xffffffff, section + ".seed")
        bounded(self.cooling.group_seed, 0, 0xffffffff, "cooling.group_seed")
        bounded(self.connections.neighbors_per_cell, 0, 32, "connections.neighbors_per_cell")
        if self.connections.mode not in ("mixed", "local", "circuit", "random"):
            raise ValueError("connections.mode: use mixed, local, circuit, or random")
        if self.cooling.group_mode not in ("cross_reactor", "random", "local"):
            raise ValueError("cooling.group_mode: use cross_reactor, random, or local")
        bounded(self.cooling.group_size, 1, 8192, "cooling.group_size")
        for key in ("direct_heat_per_pulse", "linked_heat_per_pulse"):
            bounded(getattr(self.cooling, key), 0, 255, "cooling." + key)
        for key in ("rod_step_seconds", "repair_deadline_seconds", "treatment_seconds"):
            bounded(getattr(self.timing, key), .05, 3600, "timing." + key)
        for key in ("travel_seconds", "return_settle_seconds"):
            bounded(getattr(self.timing, key), 0, 3600, "timing." + key)
        if self.timing.repair_deadline_seconds <= self.timing.travel_seconds + self.timing.treatment_seconds:
            raise ValueError("Repair deadline must exceed travel + treatment time")
        bounded(self.forecast.horizon_seconds, .05, 3600, "forecast.horizon_seconds")
        bounded(self.forecast.trials, 1, 8192, "forecast.trials")
        bounded(self.forecast.trial_batch_size, 1, 8192, "forecast.trial_batch_size")
        if self.forecast.trials * count > 4_000_000:
            raise ValueError("Too many Python forecast states; reduce trials or total cells (limit 4,000,000)")
        bounded(self.forecast.refresh_seconds, .1, 120, "forecast.refresh_seconds")
        if math.ceil(self.forecast.horizon_seconds / self.timing.rod_step_seconds) > 4096:
            raise ValueError("Forecast horizon is too many steps; increase rod_step_seconds")
        bounded(self.cooling.pulse_seconds, .01, 120, "cooling.pulse_seconds")
        for key, hi in (("steps_per_second", 120), ("grid_width", 512), ("grid_height", 512)):
            bounded(getattr(self.foam, key), 1 if key == "steps_per_second" else 16, hi, "foam." + key)
        bounded(self.foam.spray_radius_cells, .5, 50, "foam.spray_radius_cells")
        for key in ("gravity_every_steps", "diagonal_every_steps", "flow_every_steps", "debris_every_steps"):
            bounded(getattr(self.foam, key), 0, 1000, "foam." + key)
        for key, lo, hi in (("rod_height_pixels", 60, 800), ("bank_columns", 1, 64),
                            ("bank_width_pixels", 100, 1000), ("bank_height_pixels", 70, 600),
                            ("cable_gap_pixels", 50, 1000), ("cable_width_pixels", 2, 20),
                            ("socket_radius_pixels", 4, 30)):
            bounded(getattr(self.ui, key), lo, hi, "ui." + key)
        for key, lo, hi in (("frame_milliseconds", 10, 250), ("panel_columns", 1, 16),
                            ("panel_rod_columns", 1, 128), ("room_rod_columns", 1, 128),
                            ("cell_width_pixels", 24, 120), ("cell_height_pixels", 14, 80),
                            ("max_drawn_particles", 50, 10000)):
            bounded(getattr(self.ui, key), lo, hi, "ui." + key)
        def color(value):
            if not isinstance(value, str) or not re.fullmatch(r"#[0-9a-fA-F]{6}", value):
                raise ValueError(f"Colors must use #RRGGBB, not {value!r}")
        previous = -1
        for band in self.ui.risk_bands:
            if not isinstance(band, list) or len(band) != 2:
                raise ValueError("Each risk band must be [lower_probability, color]")
            bounded(band[0], 0, 1, "risk band")
            if band[0] <= previous:
                raise ValueError("Risk bands must have increasing thresholds")
            previous = band[0]
            color(band[1])
        if not self.ui.risk_bands or self.ui.risk_bands[0][0] != 0:
            raise ValueError("First risk band must start at 0.0")
        if not self.ui.heat_colors:
            raise ValueError("heat_colors cannot be empty")
        for value in self.ui.heat_colors:
            color(value)
        for value in self.ui.rod_colors:
            color(value)
        for value in p.bank_colors:
            color(value)
        for key, value in self.data["ui"].items():
            if key.endswith("_color"):
                color(value)


def load_config(path=DEFAULT_PATH, text=None, overrides=None):
    path = Path(path)
    # Allow the editor to repair a malformed on-disk main file using its valid
    # in-memory defaults; an ordinary reload still reports that file's error.
    base = (deepcopy(_STARTUP_DEFAULTS) if text is not None and path.resolve() == DEFAULT_PATH.resolve()
            else tomllib.loads(DEFAULT_PATH.read_text(encoding="utf-8")))
    changes = tomllib.loads(text if text is not None else path.read_text(encoding="utf-8"))
    merged = merge(base, changes)
    if overrides:
        merged = merge(merged, overrides)
    return Config(merged)
