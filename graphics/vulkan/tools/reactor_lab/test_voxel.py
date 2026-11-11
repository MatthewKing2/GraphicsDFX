"""3D rule provenance, conservation, contact, phase isolation, and GUI checks."""
import importlib.util
import json
from pathlib import Path
import time
import unittest
from unittest.mock import patch
import numpy as np
from config import load_config
from model import Rules, Reactor
from voxel import ROOT, EMPTY, DEBRIS, WATER, WALL, VoxelWorld, SpraySession, BallisticJet, plane_pass, upstream_table, pack_cells, unpack_cells


def small_rules(changes=None):
    from config import merge
    base={"layout":{"reactors":2,"rods_per_reactor":3,"cells_per_rod":64},
          "power":{"banks":3,"plugs_per_bank":3},
          "voxel":{"width":32,"height":40,"depth":32,"rod_radius":2,"rod_columns":3,
                   "debris_every_steps":0,"drain_every_steps":0}}
    return Rules(load_config(overrides=merge(load_config(overrides=base).source,changes or {})))


class RuleTests(unittest.TestCase):
    def test_exact_upstream_table_conserves_material_and_locks_walls(self):
        table=upstream_table()
        for index,value in enumerate(table):
            before=[(index>>(2*k))&3 for k in range(4)]
            after=[(int(value)>>(2*k))&3 for k in range(4)]
            self.assertEqual(sorted(before),sorted(after))
            for i in range(4):
                if before[i]==WALL: self.assertEqual(after[i],WALL)

    def test_xy_batch_matches_original_golden_viewer_over_both_phases(self):
        root=ROOT/("FPGA" if (ROOT/"FPGA").exists() else "fpga")
        spec=importlib.util.spec_from_file_location("pixel_reference",root/"pixel_physics_viewer.py")
        reference=importlib.util.module_from_spec(spec)
        spec.loader.exec_module(reference)
        table,noise=reference.assets()
        world=reference.World(table,noise,"random_512")
        cells=world.cells[None,:,:].copy()
        for tick in range(8):
            plane_pass(cells,upstream_table(),(1,2),tick&1)
            world.advance()
            np.testing.assert_array_equal(cells[0],world.cells)

    def test_pack_roundtrip_matches_two_bit_byte_layout(self):
        values=np.tile(np.array([0,1,2,3],np.uint8),16).reshape(4,4,4)
        payload=pack_cells(values)
        self.assertEqual(payload,bytes([0b11100100])*16)
        np.testing.assert_array_equal(unpack_cells(payload,values.shape),values)
        with self.assertRaises(ValueError): unpack_cells(payload,(2,2,2))


class WorldTests(unittest.TestCase):
    def test_surface_contact_gather_equals_original_volume_scan(self):
        w = VoxelWorld(small_rules())
        rng = np.random.default_rng(311)
        for density in (0., .01, .25, 1.):
            w.cells[~w.solids] = np.where(rng.random(np.count_nonzero(~w.solids))<density,WATER,EMPTY)
            wet = w.cells==WATER
            neighbor = np.zeros_like(wet)
            for axis in range(3):
                a,b = [slice(None)]*3,[slice(None)]*3
                a[axis],b[axis]=slice(1,None),slice(None,-1)
                neighbor[tuple(a)] |= wet[tuple(b)]
                neighbor[tuple(b)] |= wet[tuple(a)]
            expected = np.bincount(w.node_map[neighbor & (w.node_map>=0)],minlength=w.rules.count)
            np.testing.assert_array_equal(w.contacts(),expected)

    def test_idle_skips_preserve_tick_phase_and_debris_rng(self):
        w = VoxelWorld(small_rules({"voxel":{"debris_every_steps":7}}))
        rng = np.random.default_rng(w.cfg.seed)
        expected_x,expected_z = int(rng.integers(1,w.cfg.width-1)),int(rng.integers(1,w.cfg.depth-1))
        before = w.cells.copy()
        with patch("voxel.plane_pass",wraps=plane_pass) as core:
            for _ in range(7):
                np.testing.assert_array_equal(w.step(),np.zeros(w.rules.count))
            self.assertEqual(core.call_count,0)
            self.assertEqual(w.idle_steps,7)
            self.assertEqual(w.active_steps,0)
            before[expected_z,2,expected_x]=DEBRIS
            np.testing.assert_array_equal(w.cells,before)
            w.step()
            self.assertEqual(core.call_count,1)
            self.assertEqual(core.call_args.args[2:],((1,0),1))  # unchanged tick=7 phase

    def test_impact_wire_contract_is_ordered_compact_and_mass_conserving(self):
        w = VoxelWorld(small_rules())
        empty = np.flatnonzero(w.cells.ravel()==EMPTY)
        ids = np.repeat(empty[len(empty)//2],19).astype("<u4")
        reply = w.deposit_impacts(ids.tobytes())
        accepted = np.unpackbits(np.frombuffer(reply,np.uint8),bitorder="little")[:len(ids)]
        self.assertEqual(len(reply),3)
        self.assertEqual(w.impact_command_bytes,19*4)
        self.assertEqual(w.injected,int(accepted.sum()))
        self.assertEqual(np.count_nonzero(w.cells==WATER),w.injected)
        np.testing.assert_array_equal(w.cells==WALL,w.solids)
        with self.assertRaises(ValueError): w.deposit_impacts(b"bad")
        with self.assertRaises(ValueError): w.deposit_impacts(np.asarray([w.cells.size],"<u4").tobytes())

    def test_default_packed_volume_under_old_world_payload_and_link_budget(self):
        world=VoxelWorld(Rules())
        self.assertEqual(world.packed_bytes,36864)
        self.assertLess(world.packed_bytes,65536)
        self.assertLess(world.packed_bytes*60,250_000_000)
        self.assertEqual(world.cells.dtype,np.uint8) # host reference; wire is 2 bits

    def test_sealed_volume_conserves_material_and_every_wall(self):
        w=VoxelWorld(small_rules())
        rng=np.random.default_rng(17)
        candidates=np.argwhere(w.cells==EMPTY)
        chosen=candidates[rng.choice(len(candidates),1500,replace=False)]
        w.cells[tuple(chosen.T)]=rng.integers(1,3,len(chosen),dtype=np.uint8)
        before=np.bincount(w.cells.ravel(),minlength=4)
        wall=w.solids.copy()
        for _ in range(50): w.step()
        np.testing.assert_array_equal(before,np.bincount(w.cells.ravel(),minlength=4))
        np.testing.assert_array_equal(w.cells==WALL,wall)

    def test_genuine_depth_transport_not_independent_xy_sheets(self):
        w=VoxelWorld(small_rules())
        w.cells[:]=EMPTY
        w.cells[[0,-1],:,:]=WALL
        w.cells[:,[0,-1],:]=WALL
        w.cells[:,:,[0,-1]]=WALL
        w.cells[:,15,:]=WALL  # floor for a water pool
        w.solids=w.cells==WALL
        w.cells[10,14,12]=WATER
        for _ in range(8): w.step()
        z,y,x=np.where(w.cells==WATER)
        self.assertTrue(np.any(z!=10),"No depth coupling occurred")
        self.assertEqual(len(z),1)

    def test_only_nozzle_spawns_water_and_no_spawn_inside_rods(self):
        w=VoxelWorld(small_rules())
        w.emit()
        xyz=np.argwhere(w.cells==WATER)[:,[2,1,0]]
        self.assertTrue(np.all(np.sum((xyz-w.nozzle)**2,axis=1)<=w.cfg.nozzle_radius**2))
        self.assertFalse(np.any((w.cells==WATER)&w.solids))
        before=np.count_nonzero(w.cells==WATER)
        for _ in range(50): w.step(False)
        self.assertEqual(np.count_nonzero(w.cells==WATER),before)

    def test_pressure_jet_reaches_rod_and_mass_ledger_balances(self):
        s=SpraySession(Rules())
        w=s.world
        touched=0
        for _ in range(180): touched+=int(s.step(True).sum())
        self.assertGreater(touched,0)
        self.assertGreater(s.repair.contact_seconds,0)
        self.assertLessEqual(s.repair.contact_seconds,180/w.cfg.steps_per_second)
        self.assertEqual(np.count_nonzero(w.cells==WATER)+w.drained,w.injected)
        self.assertEqual(np.count_nonzero(w.cells==DEBRIS),w.debris_added)
        np.testing.assert_array_equal(w.cells==WALL,w.solids)
        water=np.argwhere(w.cells==WATER)
        self.assertGreater(np.ptp(water[:,0]),3)
        self.assertGreater(np.ptp(water[:,1]),3)

    def test_aim_stops_at_wall_and_swaps_never_cross_wall(self):
        w=VoxelWorld(small_rules())
        w.cells[20,:,:]=WALL
        w.solids=w.cells==WALL
        w.aim_at([w.cfg.width//2,10,4])
        # The corridor has a finite radius around its last free center point;
        # material still cannot swap THROUGH the intervening wall plane.
        self.assertFalse(np.any(w.beam[:20-w.cfg.jet_radius]))
        for _ in range(80): w.step(True)
        self.assertFalse(np.any(w.cells[:20]==WATER))

    def test_repeatable_and_bad_geometry_rejected(self):
        a,b=VoxelWorld(small_rules()),VoxelWorld(small_rules())
        for w in (a,b): w.aim_at([10,15,25])
        for _ in range(40): a.step(True); b.step(True)
        np.testing.assert_array_equal(a.cells,b.cells)
        with self.assertRaises(ValueError):
            VoxelWorld(small_rules({"voxel":{"rod_radius":8}}))
        with self.assertRaises(ValueError):
            load_config(overrides={"voxel":{"depth":33}})


class PhaseTests(unittest.TestCase):
    def test_existing_panel_drains_forecast_before_constructing_particle_world(self):
        import threading
        from unittest.mock import MagicMock
        import lab
        app=lab.Lab.__new__(lab.Lab)
        app.rules=small_rules()
        app.cfg=app.rules.cfg
        app.reactor=Reactor(app.rules)
        app.phase,app.paused,app.selected,app.completed="console",False,0,0
        app.version=0
        app.cancel_job=threading.Event()
        app.canvas,app.refresh=MagicMock(),MagicMock()
        drained=[]
        class PendingForecast:
            def result(self):
                self_test.assertTrue(app.cancel_job.is_set())
                drained.append(True)
                return None,0.
        self_test=self
        app.job=PendingForecast()
        original=lab.Foam
        def construct(*args,**kwargs):
            self.assertTrue(drained,"Particles started before forecast completion")
            return original(*args,**kwargs)
        with patch.object(lab,"Foam",side_effect=construct):
            app.dispatch()
        self.assertEqual(app.phase,"chamber")
        self.assertIsNone(app.job)

    def test_particle_phase_cools_without_thermal_or_forecast_steps(self):
        r=Rules(load_config(overrides={"layout":{"reactors":2}}))
        s=SpraySession(r)
        node=r.node(0,1,r.segments//2)
        s.reactor.heat[:]=12
        s.reactor.heat[node-6:node+7]=28
        s.reactor.fault[node-6:node+7]=2
        s.select(1)
        original_rng=s.reactor.rng.copy()
        original_ticks=s.reactor.ticks
        before=int(s.reactor.heat[node])
        with patch.object(r,"forecast",side_effect=AssertionError("Forecast ran in particle phase")), \
             patch.object(s.reactor,"step",side_effect=AssertionError("Thermal simulation ran in particle phase")):
            for _ in range(480): s.step(True)
        self.assertLess(int(s.reactor.heat[node]),before)
        self.assertEqual(s.reactor.ticks,original_ticks)
        np.testing.assert_array_equal(s.reactor.rng,original_rng)
        # A different reactor is unaffected by this local physical spray.
        np.testing.assert_array_equal(s.reactor.heat[r.per_chamber:],12)


class JetTests(unittest.TestCase):
    def test_velocity_before_impact_no_premature_voxel_creation(self):
        w=VoxelWorld(Rules())
        jet=BallisticJet(w)
        jet.set_stream([3.,5.,42.],[3.,5.,3.])
        jet.emit()
        before=jet.positions.copy()
        self.assertEqual(np.count_nonzero(w.cells==WATER),0)
        jet.step(False) # release does not freeze existing water in mid-air
        self.assertEqual(jet.positions.dtype,np.int32)
        self.assertEqual(len(jet.positions),len(before))
        self.assertGreater(np.mean((jet.positions-before)@jet.direction/jet.Q),1.)
        self.assertEqual(np.count_nonzero(w.cells==WATER),0)

    def test_fast_jet_cannot_tunnel_or_deposit_through_thin_wall(self):
        r=Rules(load_config(overrides={"voxel":{"jet_speed_voxels_per_second":400.}}))
        w=VoxelWorld(r)
        w.cells[:]=EMPTY
        w.cells[20,:,:]=WALL
        w.solids=w.cells==WALL
        jet=BallisticJet(w)
        jet.set_stream([24.,30.,42.],[24.,30.,3.])
        for _ in range(12): jet.step(True)
        self.assertGreater(jet.deposited,0)
        self.assertFalse(np.any(w.cells[:20]==WATER))
        self.assertEqual(jet.emitted,len(jet.positions)+jet.deposited+jet.escaped)

    def test_dense_hybrid_ledger_and_no_pressure_on_runoff(self):
        s=SpraySession()
        with patch.object(s.world,"_push_axis",side_effect=AssertionError("Old pressure applied to runoff")):
            for _ in range(70): s.step(True)
        self.assertGreater(np.count_nonzero(s.world.cells==WATER),500)
        self.assertEqual(s.jet.emitted,len(s.jet.positions)+s.jet.deposited+s.jet.escaped)
        self.assertEqual(s.jet.deposited,s.world.injected)
        self.assertEqual(s.world.injected,np.count_nonzero(s.world.cells==WATER)+s.world.drained)


class RenderTests(unittest.TestCase):
    def test_transparency_reveals_rod_not_just_background(self):
        from spray3d import rasterize
        geometry=[(20.,"polygon",[0,0,9,0,9,9,0,9],"#ff0000",1),
                  (10.,"particle",[1,1,8,8],"#ffffff",1)]
        image=rasterize(geometry,10,10,"#000000",.35,.25)
        red,green,blue=image.getpixel((4,4))
        self.assertEqual(red,255)
        self.assertTrue(0<green<255 and green==blue)
        self.assertEqual(rasterize(geometry,10,10,"#000000",0.,.25).getpixel((4,4)),(255,0,0))

    def test_render_caps_and_sizes_do_not_change_physics(self):
        from spray3d import scene,Camera
        r=small_rules({"voxel":{"max_drawn_particles":120,"max_drawn_jet_particles":32}})
        s=SpraySession(r)
        empty=np.argwhere(s.world.cells==EMPTY)[:1000]
        s.world.cells[tuple(empty.T)]=WATER
        for _ in range(3): s.jet.emit()
        before=s.world.cells.copy()
        packets=s.jet.positions.copy()
        items,_=scene(s,Camera(s.world),800,600)
        drops=[item for item in items if item[1]=="particle"]
        self.assertLessEqual(len(drops),120)
        self.assertLessEqual(sum(item[1]=="jet" for item in items),32)
        self.assertTrue(all(item[2][2]-item[2][0]<=12.001 for item in drops))
        np.testing.assert_array_equal(before,s.world.cells)
        np.testing.assert_array_equal(packets,s.jet.positions)

    def test_frame_catchup_is_bounded_for_responsive_input(self):
        from spray3d import SprayApp
        from unittest.mock import MagicMock
        app=SprayApp.__new__(SprayApp)
        app.rules=small_rules()
        app.root=MagicMock()
        app.session=MagicMock()
        app.session.repair.lost=False
        app.closed=app.paused=False
        app.spraying=True
        app.last=time.perf_counter()-10
        app.accumulator=100.
        app.dirty=False
        app.draw=MagicMock()
        app.frame()
        self.assertEqual(app.session.step.call_count,app.rules.cfg.voxel.max_simulation_steps_per_frame)
        self.assertLess(app.accumulator,1/app.rules.cfg.voxel.steps_per_second)


def gui_smoke(app):
    from spray3d import scene
    from types import SimpleNamespace
    app.root.update()
    app.draw()
    assert app.canvas.find_all()
    # Disable scheduled advancement while controlling the simulation directly.
    app.paused=True
    w=app.session.world
    node=app.rules.node(w.chamber,app.session.repair.rod,app.rules.segments//2)
    before=int(app.session.reactor.heat[node])
    rng=app.session.reactor.rng.copy()
    for _ in range(360): app.session.step(True)
    app.draw()
    assert app.canvas.find_withtag("particle"),"3D coolant was not drawn"
    assert app.canvas.find_withtag("jet_packet"),"Directed in-flight stream was not drawn"
    assert app.canvas.find_withtag("nozzle_cursor")
    assert len(app.canvas.find_withtag("scene_image"))==1
    assert len(app.canvas.find_all())<30,"Preview recreated hundreds of Tk particle items"
    assert np.count_nonzero(w.cells==WATER)>0
    assert app.session.repair.contact_seconds>0
    assert app.session.reactor.heat[node]<before,"Spray failed to cool the highlighted hot spot"
    np.testing.assert_array_equal(app.session.reactor.rng,rng)
    app.motion(SimpleNamespace(x=320,y=410))
    projected,_=app.camera.project([app.session.jet.origin],app.canvas.winfo_width(),app.canvas.winfo_height())
    np.testing.assert_allclose(projected[0],[320,410],atol=1e-6)
    position=np.array([*w.centers[0][:1],20,w.centers[0][1]])
    a,_=app.camera.project([position],800,600)
    app.orbit_start=(100,100)
    app.orbit(SimpleNamespace(x=145,y=115))
    b,_=app.camera.project([position],800,600)
    assert not np.allclose(a,b),"Camera orbit did not change 3D projection"
    app.draw()
    app.canvas.event_generate("<Button-1>",x=250,y=250)
    app.root.update()
    app.release()
    assert not app.spraying
    print(f"PASS 3D GUI: mouse nozzle, ballistic torrent, impact cooling, orbit; {np.count_nonzero(w.cells==WATER)} coolant voxels")


def benchmark(rules):
    """CPU-only timing, fixed useful simulation rate; explicitly NOT energy."""
    from spray3d import Camera, scene, rasterize
    s=SpraySession(rules)
    for _ in range(20): s.step(True)
    samples=[]
    for _ in range(240):
        t=time.perf_counter(); s.step(True); samples.append((time.perf_counter()-t)*1000)
    draw=[]
    raster=[]
    camera=Camera(s.world)
    for _ in range(12):
        t=time.perf_counter(); items,_=scene(s,camera,1000,740); draw.append((time.perf_counter()-t)*1000)
        t=time.perf_counter()
        rasterize(items,1000,740,rules.cfg.ui.background_color,rules.cfg.voxel.water_render_opacity,rules.cfg.voxel.jet_render_opacity)
        raster.append((time.perf_counter()-t)*1000)
    result={
        "backend":"CPU NumPy + software 3D projection (no FPGA/GPU physics)",
        "volume_xyz":[rules.cfg.voxel.width,rules.cfg.voxel.height,rules.cfg.voxel.depth],
        "measured_steps":len(samples),"simulation_mean_ms":round(float(np.mean(samples)),3),
        "simulation_p95_ms":round(float(np.percentile(samples,95)),3),
        "scene_generation_mean_ms":round(float(np.mean(draw)),3),
        "alpha_raster_mean_ms":round(float(np.mean(raster)),3),
        "note":"Scene timing excludes Tk rasterization; milliseconds are NOT watts or energy.",
        "packed_world_bytes":s.world.packed_bytes,
        "packed_world_MB_per_second_at_30fps":s.world.packed_bytes*30/1e6,
        "reactor_forecast_calls":0,"reactor_thermal_ticks":s.reactor.ticks,
        "in_flight_packets":len(s.jet.positions),
        "jet_state_host_bytes":s.jet.positions.nbytes+s.jet.velocities.nbytes+s.jet.age.nbytes+s.jet.entered.nbytes,
        "offload_boundary":"Proposed CPU/GPU: visual Q10 flight; FPGA: impact deposition + 2-bit runoff + wet contacts. All currently CPU.",
        "impact_request_bytes_per_simulation_second":round(s.world.impact_command_bytes/s.world.ticks*rules.cfg.voxel.steps_per_second,1),
        "impact_commands":s.world.impact_commands,
        "idle_core_steps":s.world.idle_steps,
        "active_core_steps":s.world.active_steps,
        "surface_contact_voxels":len(s.world.contact_nodes),
        "upstream":"XY/ZY cell-table extension for runoff; NEW Q10 velocity packets before impact",
        "power_status":"No LiteFury whole-board measurement or GPU baseline for this new 3D kernel"
    }
    print(json.dumps(result,indent=2))


if __name__=="__main__": unittest.main(verbosity=2)
