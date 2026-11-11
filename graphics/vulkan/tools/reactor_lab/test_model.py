"""Headless model/config checks and a real Tk drag/foam/settings smoke test."""
import math
from pathlib import Path
import tempfile
import time
import unittest
import threading
from types import SimpleNamespace
import numpy as np
from config import HERE, DEFAULT_PATH, load_config, merge
from model import Rules, Reactor, Repair, Foam, seed_streams


def rules_for(overrides=None):
    base = {
        "layout": {"reactors": 2, "rods_per_reactor": 3, "cells_per_rod": 16},
        "power": {"banks": 3, "plugs_per_bank": 3},
        "initial": {"showcase": False},
        "forecast": {"trials": 8, "horizon_seconds": 4.0},
    }
    return Rules(load_config(overrides=merge(load_config(overrides=base).source, overrides or {})))


class ConfigTests(unittest.TestCase):
    def test_default_and_presets(self):
        for path in [DEFAULT_PATH, *sorted((HERE/"presets").glob("*.toml"))]:
            with self.subTest(path=path.name):
                r = Rules(load_config(path))
                world = Reactor(r)
                p = r.forecast(world.heat, world.fault, trials=2, sockets=world.sockets)
                self.assertEqual(p.probability.shape, (r.count,))
                self.assertEqual(p.rod_probability.shape, (r.total_rods,))

    def test_example_counts_are_defaults_only(self):
        default = load_config()
        # The main file is deliberately user-tunable, not a fixed test fixture.
        self.assertEqual(Rules(default).total_rods,default.layout.reactors*default.layout.rods_per_reactor)
        for chambers, rods, segments, banks, plugs in ((1, 1, 1, 1, 1), (3, 5, 32, 5, 4),
                                                       (2, 8, 128, 7, 3), (4, 2, 64, 2, 5)):
            r = rules_for({"layout": {"reactors": chambers, "rods_per_reactor": rods, "cells_per_rod": segments},
                           "power": {"banks": banks, "plugs_per_bank": plugs}})
            self.assertEqual(r.total_rods, chambers*rods)
            sockets = r.initial_sockets()
            self.assertEqual(len(set(sockets)), r.total_rods)
            self.assertTrue(np.all(sockets < banks*plugs))
            foam = Foam(r, chambers-1)
            ids = np.unique(foam.rod_map[foam.rod_map >= 0])
            np.testing.assert_array_equal(ids, np.arange((chambers-1)*r.per_chamber, r.count))

    def test_invalid_settings_keep_clear_contract(self):
        cases = [
            {"layout": {"reactors": 0}}, {"layout": {"cells_per_rod": 2.5}},
            {"forecast": {"trials": 0}}, {"thermal": {"diffusion_256": 129}},
            {"power": {"banks": 1, "plugs_per_bank": 1}},
            {"power": {"banks": 60}}, {"power": {"plugs_per_bank": 0}},
            {"power": {"bank_colors": []}}, {"power": {"demand_min": 300, "demand_max": 100}},
            {"ui": {"risk_bands": [[0.0, "#ffffff"], [.7, "#bad"]]}},
            {"timing": {"repair_deadline_seconds": 1.0}},
        ]
        for change in cases:
            with self.subTest(change=change):
                with self.assertRaises(ValueError):
                    load_config(overrides=change)
        for text in ('[power]\nplugs_per_bank = 2.5\n', '[power]\nplugs_per_bnak = 4\n'):
            with self.assertRaises(ValueError):
                load_config(text=text)

    def test_partial_overrides_and_no_mutation(self):
        original = load_config()
        changed = load_config(text='[power]\nbanks = 3\nplugs_per_bank = 5\n')
        self.assertEqual(changed.power.banks, 3)
        self.assertEqual(load_config().data, original.data)
        self.assertEqual(Rules(original).risk_color(.7), "#df963f")
        self.assertEqual(Rules(original).risk_color(.99), "#c52f49")

    def test_formulas_recompute_after_overrides_and_support_dependencies(self):
        cfg=load_config(overrides={"layout":{"reactors":3,"rods_per_reactor":4},
                                  "power":{"banks":3,"rod_capacity_gw":150.0}})
        self.assertAlmostEqual(cfg.power.demand_max,12*150/3*.95)
        self.assertAlmostEqual(cfg.power.demand_min,12*150/3*.25)
        self.assertAlmostEqual(cfg.power.nominal_rod_load,90.)
        self.assertTrue(cfg.source["power"]["demand_max"].startswith("="))
        cfg=load_config(text='[power]\nbanks = "= ceil(layout.rods_per_reactor / 2)"\n'
            'plugs_per_bank = "= max(4, layout.reactors * 2)"\n'
            'demand_max = "= power.demand_min * 2"\n')
        self.assertEqual(cfg.power.banks,3)
        self.assertEqual(cfg.power.demand_max,cfg.power.demand_min*2)

    def test_formulas_reject_cycles_bad_math_and_python_execution(self):
        for formula in ("= power.unknown", "= __import__('os').getcwd()", "= (1).__class__",
                        "= 1 / 0", "= 2 ** 100000", "= power.bank_names", "= 1e999", "= sum([1,2])"):
            with self.subTest(formula=formula):
                with self.assertRaises(ValueError):
                    load_config(overrides={"power":{"demand_max":formula}})
        with self.assertRaisesRegex(ValueError,"Circular"):
            load_config(overrides={"power":{"demand_min":"= power.demand_max", "demand_max":"= power.demand_min"}})
        with self.assertRaises(ValueError):
            load_config(overrides={"power":{"banks":"= 1.5"}})

    def test_unique_rod_colors_stay_stable_on_reroute(self):
        r=Rules()
        colors=[r.rod_color(i) for i in range(r.total_rods)]
        self.assertEqual(len(set(colors)),r.total_rods)
        w=Reactor(r)
        w.reroute(0,7)
        self.assertEqual(colors,[r.rod_color(i) for i in range(r.total_rods)])
        self.assertEqual(colors,Rules().rod_colors)

    def test_dark_wires_and_exaggerated_visual_speeds(self):
        r=Rules()
        for rod in range(r.total_rods):
            dark,bright=r.cable_color(rod),r.rod_color(rod)
            self.assertTrue(all(int(dark[k:k+2],16)<int(bright[k:k+2],16) for k in (1,3,5)))
        speeds=r.flow_speed(np.array([0.,25.,50.,75.,100.]))
        self.assertEqual(speeds[0],0)
        self.assertTrue(np.all(np.diff(speeds)>0))
        self.assertGreater(speeds[-1]/speeds[1],40)
        self.assertEqual(speeds[-1],r.cfg.ui.flow_speed_pixels_per_second)
        self.assertEqual(r.cfg.forecast.horizon_seconds,60.)


class RoutingTests(unittest.TestCase):
    def test_exact_gw_split_and_hard_output_capacity(self):
        r=rules_for({"power":{"demand_min":50.,"demand_max":50.,"rod_capacity_gw":100.}})
        w=Reactor(r)
        np.testing.assert_allclose(r.rod_output(0,w.sockets),25.)
        w.reroute(0,5)
        self.assertEqual(r.rod_output(0,w.sockets)[3],50.)
        # Output doubles; display velocity deliberately exaggerates load differences.
        self.assertAlmostEqual(r.rod_output(0,w.sockets)[3]/25,2.)
        r=rules_for({"power":{"demand_min":250.,"demand_max":250.,"rod_capacity_gw":100.}})
        w=Reactor(r)
        np.testing.assert_array_equal(r.rod_output(0,w.sockets),100.)
        np.testing.assert_array_equal(r.bank_deficit(0,w.sockets),50.)
        w.step()
        self.assertTrue(np.all(w.blackout>0))

    def test_output_conservation_for_fractional_gw(self):
        r=rules_for({"power":{"demand_min":50.,"demand_max":50.}})
        w=Reactor(r)
        w.reroute(0,5)  # one, three and two rods per bank
        output=r.rod_output(0,w.sockets)
        delivered=np.bincount(w.sockets//r.cfg.power.plugs_per_bank,weights=output,minlength=r.cfg.power.banks)
        np.testing.assert_allclose(delivered,r.bank_demand(0))

    def test_socket_moves_and_atomic_swaps(self):
        r = rules_for()
        world = Reactor(r)
        old = world.sockets.copy()
        empty = next(i for i in range(r.cfg.power.banks*r.cfg.power.plugs_per_bank) if i not in old)
        world.reroute(0, empty)
        self.assertEqual(world.sockets[0], empty)
        world.reroute(0, int(world.sockets[1]))
        self.assertEqual(world.sockets[1], empty)
        self.assertEqual(len(set(world.sockets)), r.total_rods)
        unchanged = world.sockets.copy()
        world.reroute(0, int(world.sockets[0]))
        np.testing.assert_array_equal(world.sockets, unchanged)
        for rod, socket in ((-1, 0), (0, -1), (r.total_rods, 0), (0, 1000)):
            with self.assertRaises(ValueError):
                world.reroute(rod, socket)

    def test_bank_demand_shared_not_duplicated(self):
        r = rules_for({"power": {"demand_min": 120, "demand_max": 120}})
        world = Reactor(r)
        np.testing.assert_array_equal(r.rod_load(0, world.sockets), 60)
        target_bank = 1
        empty = target_bank*r.cfg.power.plugs_per_bank+2
        world.reroute(0, empty)
        load = r.rod_load(0, world.sockets)
        self.assertEqual(load[0], 40)  # bank 1 now has three rods, not two
        self.assertEqual(load[3], 120)  # former partner now carries bank 0 alone

    def test_load_cycles_are_slow_and_bounded(self):
        r = rules_for()
        values = np.stack([r.bank_demand(t) for t in range(r.power_period+1)])
        self.assertTrue(np.all(values >= r.cfg.power.demand_min))
        self.assertTrue(np.all(values <= r.cfg.power.demand_max))
        np.testing.assert_array_equal(values[0], values[-1])
        self.assertLessEqual(np.abs(np.diff(values,axis=0)).max(),
                             math.ceil(2*(r.cfg.power.demand_max-r.cfg.power.demand_min)/r.power_period))

    def test_empty_bank_blackout_timer_resets_when_reconnected(self):
        r = rules_for()
        w = Reactor(r)
        w.reroute(0, 5)
        w.reroute(3, 8)  # bank 0 is now empty
        w.step()
        self.assertGreater(w.blackout[0], 0)
        w.reroute(0, 0)
        w.step()
        self.assertEqual(w.blackout[0], 0)


class ThermalTests(unittest.TestCase):
    def test_diffusion_conserves_heat_and_never_wraps(self):
        r = rules_for({"thermal": {"diffusion_passes": 2, "diffusion_256": 128}})
        h = np.zeros(r.count, np.uint8)
        h[0] = 30
        h[r.segments+7] = 30
        random = seed_streams(8, h.shape)
        d = r.diffuse(h, random, 0)
        self.assertEqual(int(d.sum()), int(h.sum()))
        self.assertLess(d[0], 30)
        self.assertGreater(d[1], 0)
        self.assertEqual(d[r.segments-1], 0)
        self.assertEqual(d[2*r.segments], 0)
        self.assertEqual(d.max(), d[:r.segments].max())
        np.testing.assert_array_equal(h[[0,r.segments+7]], [30,30])

    def test_batch_equals_individual_world_and_inputs_are_not_modified(self):
        r = rules_for()
        w = Reactor(r)
        arrays = [w.heat.copy(), w.fault.copy(), w.rng.copy()]
        h, f, rng = r.transition(*arrays, tick=19, sockets=w.sockets)
        batch = r.transition(*(np.stack([a,a]) for a in arrays), tick=19, sockets=w.sockets)
        for individual, paired, original, live in zip((h,f,rng),batch,arrays,(w.heat,w.fault,w.rng)):
            np.testing.assert_array_equal(paired, np.stack([individual,individual]))
            np.testing.assert_array_equal(original,live)
        self.assertTrue(np.all(h <= r.cfg.thermal.critical_heat))

    def test_hotter_bank_means_more_heat(self):
        changes = {"thermal": {"diffusion_passes": 0, "heating_256": 0, "surge_heating_256": 0,
                                "ignition_loaded_256": 0, "ignition_quiet_256": 0,
                                "ignition_if_hot_256": 0, "background_reheating_256": 256,
                                "cooling_quiet_256": 256, "cooling_loaded_256": 256},
                   "power": {"demand_min": 180, "demand_max": 180}}
        r = rules_for(changes)
        w = Reactor(r)
        w.heat[:] = 16
        w.fault[:] = 0
        before = w.heat.copy()
        w.step()
        self.assertTrue(np.all(w.heat > before))
        cool = rules_for(merge(load_config().data, {"power": {"demand_min": 0, "demand_max": 0},
              "thermal": changes["thermal"], "layout": {"reactors":2,"rods_per_reactor":3,"cells_per_rod":16},
              "forecast": {"trials":8,"horizon_seconds":4.0}}))
        h, _, _ = cool.transition(before, np.zeros_like(before), w.rng, 0)
        self.assertTrue(np.all(h < before))

    def test_runaway_latches_until_foam_even_after_rerouting(self):
        r = rules_for({"thermal": {"diffusion_passes": 0, "runaway_heating_256": 256},
                       "power": {"demand_min": 0, "demand_max": 0}})
        w = Reactor(r)
        w.heat[:] = 5
        w.heat[4] = r.cfg.thermal.surge_heat
        w.fault[:] = 0
        w.step()
        self.assertTrue(w.runaway_rods[0])
        previous = int(w.heat[4])
        w.reroute(0, 5)
        w.step()
        self.assertGreater(w.heat[4],previous)

    def test_forecast_uses_exact_live_kernel_and_rod_union(self):
        r = rules_for({"timing": {"rod_step_seconds": .5}, "forecast": {"horizon_seconds": 3.1}})
        w = Reactor(r)
        w.heat[r.segments//2] = r.cfg.thermal.critical_heat
        original = w.heat.copy()
        h,f=np.tile(w.heat,(8,1)),np.tile(w.fault,(8,1))
        rng=seed_streams(r.cfg.forecast.seed,h.shape)
        failed=h>=r.cfg.thermal.critical_heat
        for offset in range(r.steps):
            h,f,rng=r.transition(h,f,rng,187+offset,w.sockets)
            failed|=h>=r.cfg.thermal.critical_heat
        p=r.forecast(w.heat,w.fault,tick=187,sockets=w.sockets)
        np.testing.assert_array_equal(p.probability,failed.mean(axis=0))
        np.testing.assert_array_equal(p.rod_probability,failed.reshape(8,r.total_rods,r.segments).any(-1).mean(0))
        self.assertEqual(p.rod_probability[0],1.)
        self.assertEqual(p.seconds,3.5)
        np.testing.assert_array_equal(w.heat,original)

    def test_forecast_changes_with_routing_and_old_rules_stay_valid(self):
        r = rules_for({"forecast": {"horizon_seconds": 90.0}})
        w = Reactor(r)
        before = r.forecast(w.heat,w.fault,sockets=w.sockets).rod_probability
        w.reroute(0,5)
        after = r.forecast(w.heat,w.fault,sockets=w.sockets).rod_probability
        self.assertFalse(np.array_equal(before,after))
        rules_for({"layout":{"reactors":1,"rods_per_reactor":3,"cells_per_rod":1}})
        np.testing.assert_array_equal(after,r.forecast(w.heat,w.fault,sockets=w.sockets).rod_probability)

    def test_micro_batches_preserve_every_rng_stream_and_result(self):
        # Include an uneven final batch and intermediate probabilities.
        original = rules_for({"forecast":{"trials":19,"trial_batch_size":19,"horizon_seconds":12.0}})
        world = Reactor(original)
        world.heat[:] = original.cfg.thermal.surge_heat-1
        expected = original.forecast(world.heat,world.fault,tick=317,sockets=world.sockets)
        for size in (1,3,8,64):
            r = rules_for({"forecast":{"trials":19,"trial_batch_size":size,"horizon_seconds":12.0}})
            actual = r.forecast(world.heat,world.fault,tick=317,sockets=world.sockets)
            np.testing.assert_array_equal(actual.probability,expected.probability)
            np.testing.assert_array_equal(actual.rod_probability,expected.rod_probability)
            np.testing.assert_array_equal(actual.rod_hits,expected.rod_hits)
            self.assertEqual(actual.persistent_batch_bytes,min(size,19)*r.count*7)
            payload = actual.result_packet()
            self.assertEqual(len(payload),4+2*r.total_rods)
            self.assertEqual(int(np.frombuffer(payload[:4],dtype="<u4")[0]),19)
            np.testing.assert_array_equal(np.frombuffer(payload[4:],dtype="<u2"),actual.rod_hits)

    def test_forecast_compiles_load_once_per_step_not_per_batch(self):
        from unittest.mock import patch
        r = rules_for({"forecast":{"trial_batch_size":1}})
        w = Reactor(r)
        with patch.object(r,"operating_heat",wraps=r.operating_heat) as operating:
            r.forecast(w.heat,w.fault,trials=8,sockets=w.sockets)
            self.assertEqual(operating.call_count,r.steps)

    def test_cancel_and_smooth_start(self):
        r=Rules()
        w=Reactor(r)
        gradient=np.diff(w.heat.reshape(r.total_rods,r.segments).astype(int),axis=1)
        self.assertLessEqual(np.abs(gradient).max(),1)
        cancel=threading.Event()
        cancel.set()
        self.assertIsNone(r.forecast(w.heat,w.fault,cancel=cancel))

    def test_forecast_is_empirical_and_hotspots_can_be_disabled(self):
        r=Rules()
        w=Reactor(r)
        p=r.forecast(w.heat,w.fault,sockets=w.sockets)
        self.assertTrue(np.all((p.rod_probability>=0)&(p.rod_probability<=1)))
        # Any valid tuning may honestly yield 0% or 100%; do not force an
        # intermediate percentage merely to satisfy a visual expectation.
        np.testing.assert_allclose(p.rod_probability*p.trials,np.rint(p.rod_probability*p.trials))
        # A disabled event cannot inject a new hot spot.
        off=rules_for({"thermal":{"hotspot_loaded_256":0,"hotspot_quiet_256":0,
                "diffusion_passes":0,"heating_256":0,"background_reheating_256":0,
                "surge_heating_256":0,"runaway_heating_256":0}})
        world=Reactor(off)
        before=world.heat.copy()
        world.step()
        self.assertTrue(np.all(world.heat<=before))


class CoolingTests(unittest.TestCase):
    def setup_world(self):
        r=rules_for({"cooling":{"pulse_seconds":.1,"axial_radius_cells":2},
                     "timing":{"treatment_seconds":1.0}})
        w=Reactor(r)
        w.heat[:]=12
        w.heat[4]=28
        w.fault[4]=2
        return r,w,Repair(w,4,travel=0)

    def test_every_touched_rod_cools_without_cooling_other_chambers(self):
        r,w,p=self.setup_world()
        hits=np.zeros(r.count,int)
        other=r.segments+5
        w.heat[other]=25
        hits[other]=1
        for _ in range(p.pulse_frames):
            p.cool(hits)
        self.assertLess(w.heat[other],25)
        self.assertEqual(w.heat[4],28)
        self.assertEqual(p.contact_seconds,0)
        self.assertEqual(w.ticks,0)

    def test_full_rod_objective_and_runaway_clear(self):
        r,w,p=self.setup_world()
        hits=np.zeros(r.count,int)
        hits[4]=1
        for _ in range(r.cfg.foam.steps_per_second):
            p.cool(hits)
        self.assertTrue(p.won)
        self.assertFalse(w.runaway_rods[0])
        self.assertLessEqual(w.heat[:r.segments].max(),r.cfg.thermal.safe_heat)
        self.assertEqual(w.ticks,0)
        self.assertEqual(w.heat[r.segments],12)

    def test_cooling_reach_stays_on_same_rod(self):
        r,w,p=self.setup_world()
        w.heat[:]=24
        hits=np.zeros(r.count,int); hits[0]=1
        for _ in range(p.pulse_frames): p.cool(hits)
        self.assertLess(w.heat[0],24)
        self.assertLess(w.heat[2],24)
        self.assertEqual(w.heat[3],24)
        self.assertEqual(w.heat[r.segments-1],24)

    def test_real_foam_contact_and_mission_deadline(self):
        r,w,p=self.setup_world()
        f=Foam(r,p.chamber)
        x0,y0,x1,y1=f.bounds[p.rod]
        rng=w.rng.copy()
        for _ in range(90):
            f.inject((x0+x1)/2,y0+4.5*(y1-y0)/r.segments)
            p.cool(f.step())
        self.assertTrue(p.won)
        np.testing.assert_array_equal(w.rng,rng)
        _,w,p=self.setup_world()
        before=w.heat.copy()
        p.travel=1
        p.cool(np.ones(r.count,int))
        np.testing.assert_array_equal(before,w.heat)
        p.advance(p.remaining+1)
        self.assertTrue(p.lost)
        p.cool(np.ones(r.count,int))
        np.testing.assert_array_equal(before,w.heat)


def gui_smoke(app):
    def wait_forecast():
        deadline=time.perf_counter()+20
        while app.prediction is None and time.perf_counter()<deadline:
            app.root.update()
            time.sleep(.01)
        assert app.prediction is not None, "Forecast did not finish"
    app.root.update()
    wait_forecast()
    assert len(app.boxes)==app.rules.total_rods
    assert len(app.socket_ports)==app.cfg.power.banks*app.cfg.power.plugs_per_bank
    print(f"PASS GUI forecast: {app.forecast_ms:.1f} ms, {app.rules.total_rods} rods")
    assert len(set(app.rules.rod_colors))==app.rules.total_rods
    assert app.canvas.find_withtag("flow_arrow"),"No moving flow arrows drawn"
    labels=app.canvas.find_withtag("rod_forecast_label")
    assert len(labels)==app.rules.total_rods
    assert all(f"/{app.rules.horizon:g}s" in app.canvas.itemcget(item,"text") for item in labels)
    # Force an actual supply shortfall, check alarms/sparks, then restore wiring.
    saved=app.reactor.sockets.copy()
    random_before=app.reactor.rng.copy()
    bank=int(app.rules.bank_demand(app.reactor.ticks).argmax())
    plugs=app.cfg.power.plugs_per_bank
    for rod in np.flatnonzero(saved//plugs==bank):
        if app.rules.bank_deficit(app.reactor.ticks,app.reactor.sockets)[bank]>1e-6:
            break
        free=next((i for i in app.socket_ports if i//plugs!=bank and i not in app.reactor.sockets),None)
        if free is None:
            break
        app.reactor.reroute(int(rod),free)
    assert app.rules.bank_deficit(app.reactor.ticks,app.reactor.sockets)[bank]>0
    app.refresh()
    alarm=app.canvas.find_withtag(f"bank_alarm_{bank}")
    assert any(app.canvas.type(i)=="text" and app.canvas.itemcget(i,"text")=="NOT ENOUGH POWER" for i in alarm)
    assert app.canvas.find_withtag(f"bank_spark_{bank}")
    light=next(i for i in alarm if app.canvas.type(i)=="oval")
    before=app.canvas.itemcget(light,"fill")
    app.visual_time+=1/(2*app.cfg.ui.alarm_flash_hz)
    app.refresh()
    light=next(i for i in app.canvas.find_withtag(f"bank_alarm_{bank}") if app.canvas.type(i)=="oval")
    assert app.canvas.itemcget(light,"fill")!=before,"Shortage light did not flash"
    np.testing.assert_array_equal(random_before,app.reactor.rng)
    app.reactor.sockets=saved
    app.refresh()
    actual_short=app.rules.bank_deficit(app.reactor.ticks,saved)>1e-6
    for bank_index,short in enumerate(actual_short):
        assert bool(app.canvas.find_withtag(f"bank_alarm_{bank_index}"))==bool(short),"Alarm must match restored supply"
    # Exercise actual UI handlers, including scroll-coordinate conversion.
    app.paused=True
    rod=0
    old=int(app.reactor.sockets[rod])
    empty=next(i for i in app.socket_ports if i not in app.reactor.sockets)
    box=app.boxes[rod]
    sx,sy=(box[0]+box[2])/2,(box[1]+box[3])/2
    app.paused=False
    app.motion(SimpleNamespace(x=sx-app.canvas.canvasx(0),y=sy-app.canvas.canvasy(0)))
    assert app.hover_rod==rod
    app.refresh()
    assert any(float(app.canvas.itemcget(item,"width")) == app.cfg.ui.cable_width_pixels+6
               for item in app.canvas.find_withtag(f"cable_{rod}")),"Hover should add a cable halo"
    app.release()
    assert app.hover_rod is None
    app.press(SimpleNamespace(x=sx-app.canvas.canvasx(0),y=sy-app.canvas.canvasy(0)))
    tx,ty=app.socket_ports[empty]
    app.motion(SimpleNamespace(x=tx-app.canvas.canvasx(0),y=ty-app.canvas.canvasy(0)))
    app.finish_drag(SimpleNamespace(x=tx-app.canvas.canvasx(0),y=ty-app.canvas.canvasy(0),widget=app.canvas))
    assert app.reactor.sockets[rod]==empty and app.prediction is None
    assert len(set(app.reactor.sockets))==app.rules.total_rods
    app.reactor.reroute(rod,old)
    # Real room: sweep the hottest area; other thermal state must remain frozen.
    app.dispatch()
    app.repair.travel=0
    r,p,f=app.rules,app.repair,app.foam
    frozen_ticks,frozen_rng=app.reactor.ticks,app.reactor.rng.copy()
    x0,y0,x1,y1=f.bounds[p.rod]
    target_rod=p.target//r.segments
    for frame in range(math.ceil((r.cfg.timing.treatment_seconds+12)*r.cfg.foam.steps_per_second)):
        h=app.reactor.heat.reshape(r.total_rods,r.segments)[target_rod]
        hottest=int(h.argmax())
        p.advance(1/r.cfg.foam.steps_per_second)
        f.inject((x0+x1)/2,y0+(hottest+.5)*(y1-y0)/r.segments)
        p.cool(f.step())
        if p.won: break
    assert p.won,"Real foam did not complete target rod treatment"
    assert app.reactor.ticks==frozen_ticks
    np.testing.assert_array_equal(app.reactor.rng,frozen_rng)
    app.dispatch()
    assert app.phase=="console" and app.completed==1
    # Config/editor: count changes should rebuild cables, rods and sockets.
    cfg=load_config(overrides={"layout":{"reactors":3,"rods_per_reactor":2,"cells_per_rod":32},
                              "power":{"banks":2,"plugs_per_bank":4},
                              "forecast":{"trials":8,"horizon_seconds":2.0}})
    app.apply_config(cfg)
    app.root.update()
    wait_forecast()
    assert len(app.boxes)==6 and len(app.socket_ports)==8
    assert len(app.prediction.rod_probability)==6

    def widgets(widget):
        for child in widget.winfo_children():
            yield child
            yield from widgets(child)
    changed='[layout]\nreactors = 1\nrods_per_reactor = 3\ncells_per_rod = 64\n[power]\nbanks = 2\nplugs_per_bank = 2\ndemand_max = "= layout.rods_per_reactor * power.rod_capacity_gw / power.banks * .8"\n[forecast]\ntrials = 8\nhorizon_seconds = 2.0\n'
    app.edit_settings()
    app.root.update()
    assert app.paused
    editor=app.editor
    contents=next(w for w in widgets(editor) if w.winfo_class()=="Text")
    contents.delete("1.0","end")
    contents.insert("1.0",changed)
    next(w for w in widgets(editor) if w.winfo_class()=="Button" and
         w.cget("text").startswith("APPLY TEMPORARILY")).invoke()
    app.root.update()
    assert app.rules.total_rods==3 and len(app.socket_ports)==4
    with tempfile.TemporaryDirectory(prefix="reactor-config-test-") as temporary:
        path=Path(temporary)/"trial.toml"
        path.write_text(changed,encoding="utf-8")
        app.apply_config(load_config(path),path)
        app.edit_settings()
        app.root.update()
        editor=app.editor
        contents=next(w for w in widgets(editor) if w.winfo_class()=="Text")
        contents.insert("end",'\n[cooling]\ngroup_size = 3\n')
        next(w for w in widgets(editor) if w.winfo_class()=="Button" and w.cget("text")=="SAVE + APPLY").invoke()
        app.root.update()
        assert load_config(path).cooling.group_size==3
        assert load_config(path).power.demand_max==120.
        assert path.with_suffix(".toml.bak").read_text(encoding="utf-8")==changed
        app.apply_config(load_config(),DEFAULT_PATH)
    app.dispatch()
    app.refresh()
    app.root.update_idletasks()
    print("PASS GUI dark wires / flow / flashing shortage + sparks / risk labels / hover / drag / foam / configs")


if __name__=="__main__":
    unittest.main(verbosity=2)
