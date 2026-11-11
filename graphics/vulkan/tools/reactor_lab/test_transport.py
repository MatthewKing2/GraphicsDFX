"""New defaults: neutron physics/forecasts and FHP film invariants, no hardware."""
import unittest
from unittest.mock import patch
import numpy as np
from config import load_config
from model import Rules,Reactor,seed_streams
from neutron import sample
from lattice import LatticeGas,COLLISION,POPCOUNT,VELOCITY
from voxel import SurfaceWorld,SpraySession,WATER


def rules(extra=None):
    from config import merge
    cfg = load_config(overrides={"layout":{"reactors":2,"rods_per_reactor":3,"cells_per_rod":32},
                                "power":{"banks":3,"plugs_per_bank":3},
                                "forecast":{"trials":5,"horizon_seconds":4.0},
                                "lattice":{"film_rows":16,"film_columns":16}})
    return Rules(load_config(overrides=merge(cfg.source,extra or {})))


class NeutronTests(unittest.TestCase):
    def test_terminal_outcomes_and_heat_counts_account_for_every_history(self):
        r=rules()
        w=Reactor(r)
        h,f,rng=w.heat.copy(),w.fault.copy(),w.rng.copy()
        counts,stats=sample(r,h,rng,r.operating_heat(0))
        self.assertEqual(int(counts.sum()),stats["fissions"])
        self.assertEqual(stats["sources"]+stats["children"],sum(stats[k] for k in ("fissions","absorbed","leaked","capped")))
        self.assertGreater(stats["steps"],0)
        np.testing.assert_array_equal(w.heat,h)
        np.testing.assert_array_equal(w.fault,f)
        np.testing.assert_array_equal(w.rng,rng)

    def test_transport_does_not_depend_on_other_worlds_or_batch_length(self):
        r=rules({"neutron":{"walkers_per_rod":8}})
        w=Reactor(r)
        h=np.stack([w.heat,w.heat//2,np.full(r.count,23,np.uint8)])
        rng=seed_streams(567,h.shape)
        batch,stats=sample(r,h,rng,r.operating_heat(8))
        totals={key:0 for key in stats}
        for i in range(3):
            single,s=sample(r,h[i],rng[i],r.operating_heat(8))
            np.testing.assert_array_equal(single,batch[i])
            for key in s: totals[key]+=s[key]
        self.assertEqual(totals,stats)

    def test_neutrons_couple_rods_but_never_cross_separate_reactors(self):
        r=rules({"neutron":{"rod_pitch":2,"fuel_width":1,"walkers_per_rod":64,"max_steps":96,
                            "fuel_absorb_256":0,"moderator_absorb_256":0,"fission_base_256":12,
                            "fission_load_256":0,"hot_feedback_256":0}})
        h=np.full((24,r.count),12,np.uint8)
        counts,_=sample(r,h,seed_streams(31,h.shape),r.operating_heat(0),source_rods=[0])
        self.assertGreater(int(counts[:,r.segments:2*r.segments].sum()),0)
        self.assertEqual(int(counts[:,r.per_chamber:].sum()),0)

    def test_absorbing_material_suppresses_all_fissions(self):
        r=rules({"neutron":{"fuel_absorb_256":256,"moderator_absorb_256":256}})
        w=Reactor(r)
        counts,stats=sample(r,w.heat,w.rng,r.operating_heat(0))
        self.assertEqual(int(counts.sum()),0)
        self.assertEqual(stats["children"],0)
        self.assertGreater(stats["absorbed"],0)

    def test_power_load_changes_reaction_rate_not_only_ui_labels(self):
        r=rules({"neutron":{"hot_feedback_256":0,"walkers_per_rod":16}})
        h=np.full((24,r.count),12,np.uint8)
        rng=seed_streams(777,h.shape)
        low,_=sample(r,h,rng,np.full(r.total_rods,r.cfg.power.base_heat,np.int16))
        high,_=sample(r,h,rng,np.full(r.total_rods,r.cfg.thermal.surge_heat,np.int16))
        self.assertGreater(int(high.sum()),int(low.sum()))

    def test_fission_heat_is_a_real_thermal_input_and_forecasts_are_exact(self):
        r=rules({"forecast":{"trial_batch_size":2},"thermal":{"diffusion_passes":0,
                "background_reheating_256":0,"cooling_quiet_256":0,"cooling_loaded_256":0},
                "neutron":{"walkers_per_rod":8}})
        w=Reactor(r)
        w.heat[:]=10
        h,flags,_=r.transition(w.heat,w.fault,w.rng,sockets=w.sockets)
        self.assertGreater(int((h.astype(np.uint32)*64+(flags>>2)).sum()),int(w.heat.sum())*64)
        a=r.forecast(w.heat,w.fault,sockets=w.sockets)
        r2=rules({"forecast":{"trial_batch_size":5},"thermal":{"diffusion_passes":0,
                "background_reheating_256":0,"cooling_quiet_256":0,"cooling_loaded_256":0},
                "neutron":{"walkers_per_rod":8}})
        b=r2.forecast(w.heat,w.fault,sockets=w.sockets)
        np.testing.assert_array_equal(a.probability,b.probability)
        np.testing.assert_array_equal(a.rod_hits,b.rod_hits)

    def test_new_config_fields_support_formulas_and_reject_bad_budgets(self):
        self.assertEqual(rules().cfg.neutron.walkers_per_rod,2)
        for bad in ({"neutron":{"generations":4}}, {"lattice":{"film_rows":0}},
                    {"neutron":{"heat_capacity":0}},
                    {"neutron":{"fuel_width":4,"rod_pitch":4}}):
            with self.assertRaises(ValueError): rules(bad)

    def test_default_opening_no_longer_runs_away_on_first_step(self):
        # Actual shipped config/seed: previously latched on step 1, died on 6.
        r=Rules();w=Reactor(r)
        for _ in range(10):
            w.step()
            self.assertFalse(w.runaway_rods.any())
            self.assertLess(int(w.heat.max()),r.cfg.thermal.critical_heat)

    def test_rerouting_buys_time_but_ignoring_overload_is_still_dangerous(self):
        r=Rules()
        ignored,managed=Reactor(r),Reactor(r)
        # Move the spare rod on bank 0 to the initially overloaded bank 2.
        managed.reroute(4,9)
        failure=None
        for tick in range(60):
            managed.step()
            self.assertFalse(managed.runaway_rods.any())
            self.assertFalse(managed.blackout.any())
            self.assertLess(int(managed.heat.max()),r.cfg.thermal.critical_heat)
            ignored.step()
            if failure is None and int(ignored.heat.max())>=r.cfg.thermal.critical_heat:
                failure=tick+1
        self.assertIsNotNone(failure,"Fix must not disable dangerous reactions")
        self.assertGreaterEqual(failure,25)

    def test_heat_is_normalized_by_samples_and_fractional_energy_accumulates(self):
        output=[]
        quiet={"diffusion_passes":0,"background_reheating_256":0,"cooling_loaded_256":0,
               "cooling_quiet_256":0,"runaway_heating_256":0}
        for samples in (4,8,16,32):
            r=rules({"neutron":{"walkers_per_rod":samples},"thermal":quiet})
            h=np.full(r.count,10,np.uint8);f=np.zeros(r.count,np.uint8)
            rng=seed_streams(17,h.shape)
            # Same physical tally estimate at four different sampling budgets.
            counts=np.full(r.count,samples//4,np.uint32)
            with patch("neutron.sample",return_value=(counts,{})):
                h,f,rng=r.transition(h,f,rng)
                np.testing.assert_array_equal(h,10)
                np.testing.assert_array_equal(f>>2,32) # half a heat unit, not lost
                h,f,rng=r.transition(h,f,rng)
                np.testing.assert_array_equal(h,11)
                np.testing.assert_array_equal(f>>2,0)
            output.append((h,f))
        for h,f in output[1:]:
            np.testing.assert_array_equal(h,output[0][0])
            np.testing.assert_array_equal(f,output[0][1])

    def test_forecast_snapshots_retain_fractional_heat(self):
        r=rules({"forecast":{"horizon_seconds":1.0},"neutron":{"walkers_per_rod":8},
                 "thermal":{"diffusion_passes":0,"background_reheating_256":0,
                            "cooling_loaded_256":0,"cooling_quiet_256":0,"runaway_heating_256":0}})
        h=np.full(r.count,10,np.uint8);h[0]=r.cfg.thermal.critical_heat-1
        f=np.zeros(r.count,np.uint8)
        counts=np.ones(r.count,np.uint32)
        with patch("neutron.sample",return_value=(counts,{})):
            cold=r.forecast(h,f)
            f[0]=63<<2
            warm=r.forecast(h,f)
        self.assertEqual(cold.rod_probability[0],0.)
        self.assertEqual(warm.rod_probability[0],1.)

    def test_spraying_preserves_fractional_energy_on_untouched_rods(self):
        from model import Repair
        r=rules({"cooling":{"pulse_seconds":.1,"axial_radius_cells":0,"linked_heat_per_pulse":0}})
        w=Reactor(r)
        w.heat[:]=12
        w.fault[:]=23<<2
        p=Repair(w,0,travel=0)
        untouched=w.fault[r.segments:].copy()
        contacts=np.zeros(r.count,np.uint32);contacts[0]=1
        for _ in range(p.pulse_frames): p.cool(contacts)
        np.testing.assert_array_equal(w.fault[r.segments:],untouched)
        self.assertEqual(int(w.fault[0]>>2),23)
        self.assertLess(w.heat[0],12)


class GasTests(unittest.TestCase):
    def test_every_collision_conserves_particle_count_and_hex_momentum(self):
        velocity=np.asarray(VELOCITY)
        bits=((np.arange(64)[:,None]>>np.arange(6))&1)
        momentum=bits@velocity
        for noise in range(2):
            np.testing.assert_array_equal(POPCOUNT[COLLISION[noise]],POPCOUNT)
            np.testing.assert_array_equal(momentum[COLLISION[noise]],momentum)
        self.assertFalse(np.array_equal(COLLISION[0],COLLISION[1]))

    def test_sealed_walls_and_forcing_never_create_or_destroy_particles(self):
        for gravity in (0,256):
            solids=np.zeros((12,18),bool)
            solids[3:9,8]=True
            g=LatticeGas(solids.shape,solids,drain_bottom=False,gravity=gravity)
            g.channels=np.random.default_rng(54).integers(0,64,solids.shape,dtype=np.uint8)
            g.channels[solids]=0
            before=g.particle_count
            for _ in range(60):
                g.step()
                self.assertEqual(g.particle_count,before)
                self.assertFalse(np.any(g.channels[solids]))

    def test_periodic_cylinder_seam_and_wall_bounce(self):
        g=LatticeGas((8,8),wrap_x=True,drain_bottom=False)
        g.channels[3,-1]=1
        g.step()
        self.assertEqual(g.channels[3,0],1)
        self.assertEqual(g.particle_count,1)
        solids=np.zeros((8,8),bool);solids[3,4]=True
        g=LatticeGas((8,8),solids,drain_bottom=False)
        g.channels[3,3]=1
        g.step()
        self.assertEqual(g.channels[3,3],1<<3)

    def test_source_and_drain_ledger_over_many_steps(self):
        g=LatticeGas((3,16,16),wrap_x=True,gravity=128)
        mask=np.zeros(g.channels.shape,bool);mask[:,2,4:12]=True
        for tick in range(120):
            if tick<70: g.inject_mask(mask)
            g.step()
            self.assertEqual(g.injected,g.particle_count+g.drained)
        self.assertGreater(g.drained,0)


class SurfaceTests(unittest.TestCase):
    def test_legacy_models_remain_selectable_for_comparison(self):
        from voxel import VoxelWorld
        r=rules({"neutron":{"enabled":False},"lattice":{"enabled":False}})
        s=SpraySession(r)
        self.assertIs(type(s.world),VoxelWorld)
        for _ in range(35): s.step(True)
        self.assertGreater(s.world.injected,0)
        self.assertIsNotNone(r.forecast(s.reactor.heat,s.reactor.fault,sockets=s.reactor.sockets))

    def test_new_default_uses_only_film_solver_and_closes_both_mass_ledgers(self):
        s=SpraySession(rules())
        self.assertIsInstance(s.world,SurfaceWorld)
        with patch("voxel.plane_pass",side_effect=AssertionError("Legacy water solver ran")), \
             patch("neutron.sample",side_effect=AssertionError("Neutrons ran while spraying")):
            for tick in range(180):
                s.step(tick<120)
                self.assertEqual(s.jet.emitted,len(s.jet.positions)+s.jet.deposited+s.jet.escaped)
                self.assertEqual(s.world.injected,s.world.water_particles+s.world.drained)
        self.assertGreater(s.world.injected,0)
        self.assertGreater(s.world.drained,0)
        self.assertFalse(np.any(s.world.cells==WATER))
        self.assertEqual(s.world.packed_bytes,s.rules.rods*16*16)

    def test_film_rod_isolation_resolution_mapping_and_compact_commands(self):
        r=rules({"layout":{"cells_per_rod":128}})
        w=SurfaceWorld(r,chamber=1)
        q=r.cfg.lattice
        # Wet every row on only rod 1, including angular seam.
        ids=((1*q.film_rows+np.arange(q.film_rows))*q.film_columns).astype("<u4")
        payload=ids.tobytes()
        accepted=w.deposit_impacts(payload)
        self.assertEqual(np.unpackbits(np.frombuffer(accepted,np.uint8),bitorder="little")[:len(ids)].sum(),len(ids))
        contacts=w.contacts().reshape(r.chambers,r.rods,r.segments)
        self.assertTrue(np.all(contacts[1,1]>0))
        self.assertEqual(int(contacts[0].sum()),0)
        self.assertEqual(int(contacts[1,[0,2]].sum()),0)
        for _ in range(12): w.step()
        self.assertFalse(w.film.gas.channels[[0,2]].any())
        self.assertEqual(w.impact_command_bytes,len(ids)*4)

    def test_host_mapping_and_rendering_do_not_mutate_physics(self):
        from spray3d import scene,Camera
        s=SpraySession(rules())
        w=s.world
        x,z=w.centers[0]
        payload=w.encode_impacts(np.array([[x+w.cfg.rod_radius+1,w.y0+5,z],[1,-100,1]]))
        ids=np.frombuffer(payload,dtype="<u4")
        self.assertNotEqual(ids[0],w.film.INVALID)
        self.assertEqual(ids[1],w.film.INVALID)
        w.deposit_impacts(payload)
        before=w.film.gas.channels.copy()
        items,_=scene(s,Camera(w),800,600)
        self.assertTrue(any(item[1]=="particle" for item in items))
        np.testing.assert_array_equal(w.film.gas.channels,before)

    def test_forecasting_never_runs_fluid_core(self):
        r=rules();w=Reactor(r)
        with patch.object(LatticeGas,"step",side_effect=AssertionError("Fluid ran in forecast phase")):
            self.assertIsNotNone(r.forecast(w.heat,w.fault,sockets=w.sockets))


if __name__=="__main__": unittest.main(verbosity=2)
