"""Standalone reactor tuning playground (Python 3.11+, NumPy + Tk).
Run: python graphics/vulkan/tools/reactor_lab/lab.py
SETTINGS edits TOML in-app. F5 reloads/restarts; R resets; Space pauses; E dispatches.
--config presets/calm.toml selects another config (relative to this folder).
No GPU/FPGA, real reactor physics, or true 3D fluids are implemented here.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import math
from pathlib import Path
import time
import threading
import tkinter as tk
from tkinter import ttk, messagebox, filedialog
import numpy as np
from config import HERE, DEFAULT_PATH, load_config
from model import Rules, Reactor, Repair, Foam


def gw(value):
    return f"{float(value):.1f}".rstrip("0").rstrip(".")


def calculate(rules, heat, fault, tick, cancel=None, sockets=None):
    start = time.perf_counter()
    result = rules.forecast(heat, fault, tick=tick, cancel=cancel, sockets=sockets)
    return result, (time.perf_counter() - start) * 1000


class Lab:
    def __init__(self, root, config_path=DEFAULT_PATH):
        self.root, self.config_path = root, Path(config_path)
        self.edit_text = self.config_path.read_text(encoding="utf-8")
        self.cfg = load_config(self.config_path)
        self.rules = Rules(self.cfg)
        self.executor = ThreadPoolExecutor(max_workers=1)
        self.job, self.job_version, self.version = None, -1, 0
        self.closed, self.paused, self.spraying = False, False, False
        self.mouse, self.boxes = (0, 0), []
        root.title("ROD / RESCUE | configurable playground")
        root.geometry(f"{min(1440, root.winfo_screenwidth()-70)}x{min(900, root.winfo_screenheight()-110)}+20+30")
        root.minsize(1000, 650)
        self.build_widgets()
        root.bind("<space>", lambda e: self.toggle_pause())
        root.bind("r", lambda e: self.reset())
        root.bind("e", lambda e: self.dispatch())
        root.bind("<F5>", lambda e: self.reload_config())
        root.bind("<Escape>", lambda e: self.toggle_pause())
        root.bind("<FocusOut>", lambda e: self.release())
        root.bind("<ButtonRelease-1>", self.finish_drag)
        root.protocol("WM_DELETE_WINDOW", self.close)
        self.reset()
        root.after(self.cfg.ui.frame_milliseconds, self.frame)

    def label(self, parent, text="", size=11, color=None, **kw):
        return tk.Label(parent, text=text, bg=parent.cget("bg"), fg=color or self.cfg.ui.text_color,
                        font=("Consolas", size), anchor="w", justify="left", **kw)

    def button(self, parent, text, command):
        return tk.Button(parent, text=text, command=command, bg="#23434b", fg=self.cfg.ui.text_color,
                         activebackground="#3b6265", activeforeground="white", relief="flat",
                         font=("Consolas", 10), padx=10, pady=8)

    def build_widgets(self):
        u = self.cfg.ui
        self.root.configure(bg=u.background_color)
        header = tk.Frame(self.root, bg=u.background_color)
        header.pack(fill="x", padx=18, pady=(12, 4))
        self.label(header, "ROD / RESCUE", 22, u.linked_color).pack(side="left")
        self.mode = self.label(header, size=11, color=u.accent_color)
        self.mode.pack(side="right")
        controls = tk.Frame(self.root, bg=u.background_color)
        controls.pack(fill="x", padx=18, pady=6)
        self.presets = {"settings.toml": DEFAULT_PATH}
        self.presets.update({p.stem: p for p in sorted((HERE / "presets").glob("*.toml"))})
        if self.config_path not in self.presets.values():
            self.presets[self.config_path.name] = self.config_path
        self.preset = tk.StringVar(value=next((k for k,v in self.presets.items() if v == self.config_path), "settings.toml"))
        combo = ttk.Combobox(controls, textvariable=self.preset, values=list(self.presets), state="readonly", width=17)
        combo.pack(side="left")
        combo.bind("<<ComboboxSelected>>", lambda e: self.reload_config(self.presets[self.preset.get()]))
        self.button(controls, "SETTINGS", self.edit_settings).pack(side="left", padx=6)
        self.button(controls, "RELOAD [F5]", self.reload_config).pack(side="left")
        self.button(controls, "LOAD...", self.choose_config).pack(side="left", padx=6)
        self.label(controls, "Edit -> Apply / Save -> play. Reload restarts the run.", 9, u.muted_color).pack(side="left")
        body = tk.Frame(self.root, bg=u.background_color)
        body.pack(fill="both", expand=True, padx=16)
        holder = tk.Frame(body, bg=u.background_color)
        holder.pack(side="left", fill="both", expand=True)
        self.canvas = tk.Canvas(holder, bg=u.background_color, highlightthickness=0)
        vs, hs = tk.Scrollbar(holder, command=self.canvas.yview), tk.Scrollbar(holder, orient="horizontal", command=self.canvas.xview)
        vs.pack(side="right", fill="y")
        hs.pack(side="bottom", fill="x")
        self.canvas.pack(fill="both", expand=True)
        self.canvas.configure(yscrollcommand=vs.set, xscrollcommand=hs.set)
        self.canvas.bind("<Button-1>", self.press)
        self.canvas.bind("<B1-Motion>", self.motion)
        self.canvas.bind("<Motion>", self.motion)
        self.canvas.bind("<Leave>", lambda e: self.release())
        shell = tk.Frame(body, bg=u.panel_color, width=340)
        shell.pack(side="right", fill="y", padx=(12, 0))
        shell.pack_propagate(False)
        viewport = tk.Canvas(shell, bg=u.panel_color, width=320, highlightthickness=0)
        scrollbar = tk.Scrollbar(shell, command=viewport.yview)
        scrollbar.pack(side="right", fill="y")
        viewport.pack(side="left", fill="both", expand=True)
        viewport.configure(yscrollcommand=scrollbar.set)
        side = tk.Frame(viewport, bg=u.panel_color)
        window = viewport.create_window(0, 0, window=side, anchor="nw")
        side.bind("<Configure>", lambda e: viewport.configure(scrollregion=viewport.bbox("all")))
        viewport.bind("<Configure>", lambda e: viewport.itemconfigure(window, width=e.width))
        self.root.bind_all("<MouseWheel>", lambda e: viewport.yview_scroll(-int(e.delta/120), "units")
                           if str(e.widget).startswith(str(shell)) else None)
        self.headline = self.label(side, size=16, color=u.linked_color)
        self.headline.pack(fill="x", padx=14, pady=(15, 10))
        self.detail = self.label(side, size=10, wraplength=280)
        self.detail.pack(fill="x", padx=14)
        self.action = self.button(side, "DISPATCH [E]", self.dispatch)
        self.action.pack(fill="x", padx=14, pady=14)
        self.message = self.label(side, size=10, color=u.accent_color, wraplength=280)
        self.message.pack(fill="x", padx=14, pady=8)
        self.coverage = self.label(side, size=9, color=u.linked_color)
        self.coverage.pack(fill="x", padx=14, pady=8)
        self.compute = self.label(side, size=9, color=u.muted_color, wraplength=280)
        self.compute.pack(fill="x", padx=14, pady=12)
        footer = tk.Frame(self.root, bg=u.background_color)
        footer.pack(fill="x", padx=18, pady=10)
        self.pause_button = self.button(footer, "PAUSE [Space]", self.toggle_pause)
        self.pause_button.pack(side="left")
        self.button(footer, "RESET [R]", self.reset).pack(side="left", padx=8)
        self.status = self.label(footer, size=9, color=u.muted_color)
        self.status.pack(side="right")

    def apply_config(self, cfg, path=None, text=None):
        rules = Rules(cfg)  # Validate/build before mutating the current run.
        new_path = Path(path) if path is not None else self.config_path
        new_text = text if text is not None else new_path.read_text(encoding="utf-8")
        self.cfg, self.rules = cfg, rules
        self.config_path, self.edit_text = new_path, new_text
        # Rebuild widgets for theme changes; any old worker owns its own Rules.
        for child in self.root.winfo_children():
            if not isinstance(child, tk.Toplevel):
                child.destroy()
        self.build_widgets()
        self.reset()

    def reload_config(self, path=None):
        path = Path(path) if path is not None else self.config_path
        try:
            self.apply_config(load_config(path), path)
        except (ValueError, OSError) as e:
            messagebox.showerror("Settings not applied -- current run kept", str(e), parent=self.root)

    def choose_config(self):
        path = filedialog.askopenfilename(initialdir=HERE, filetypes=[("TOML config", "*.toml")])
        if path:
            self.reload_config(path)

    def edit_settings(self):
        if getattr(self, "editor", None) is not None and self.editor.winfo_exists():
            self.editor.lift()
            return
        was_paused = self.paused
        self.paused, self.spraying = True, False
        self.pause_button.configure(text="RESUME [Space]")
        editor = tk.Toplevel(self.root)
        self.editor = editor
        editor.title(f"Settings -- {self.config_path.name}")
        editor.geometry("960x760")
        self.label(editor, "Apply = temporary run. Save + Apply = write this file. Invalid settings keep your current run.",
                   10, wraplength=900).pack(fill="x", padx=10, pady=8)
        text = tk.Text(editor, wrap="none", undo=True, font=("Consolas", 11))
        scroll = tk.Scrollbar(editor, command=text.yview)
        scroll.pack(side="right", fill="y")
        text.configure(yscrollcommand=scroll.set)
        buttons = tk.Frame(editor)
        buttons.pack(side="bottom", fill="x")
        text.pack(fill="both", expand=True)
        path = self.config_path
        text.insert("1.0", self.edit_text)
        def cancel():
            self.paused = was_paused
            self.pause_button.configure(text="RESUME [Space]" if self.paused else "PAUSE [Space]")
            editor.destroy()
        editor.protocol("WM_DELETE_WINDOW", cancel)
        def apply(save=False, save_as=False):
            content = text.get("1.0", "end-1c")
            try:
                cfg = load_config(path, text=content)
                # Build before saving, so errors cannot overwrite a working file.
                Rules(cfg)
                destination = path
                if save_as:
                    chosen = filedialog.asksaveasfilename(parent=editor, initialdir=HERE/"presets",
                        defaultextension=".toml", filetypes=[("TOML config", "*.toml")])
                    if not chosen:
                        return
                    destination = Path(chosen)
                if save or save_as:
                    if destination.exists():
                        destination.with_suffix(destination.suffix + ".bak").write_bytes(destination.read_bytes())
                    destination.write_text(content + "\n", encoding="utf-8", newline="\n")
                self.apply_config(cfg, destination, content)
                editor.destroy()
            except (ValueError, OSError) as e:
                messagebox.showerror("Invalid settings -- not applied", str(e), parent=editor)
        self.button(buttons, "APPLY TEMPORARILY + RESTART", apply).pack(side="left", padx=8, pady=8)
        self.button(buttons, "SAVE + APPLY", lambda: apply(True)).pack(side="left", padx=8)
        self.button(buttons, "SAVE AS...", lambda: apply(save_as=True)).pack(side="left", padx=8)
        self.button(buttons, "CANCEL", cancel).pack(side="right", padx=8)

    def invalidate_forecast(self):
        if getattr(self, "cancel_job", None) is not None:
            self.cancel_job.set()
        self.version += 1
        self.prediction, self.forecast_due = None, 0.
        self.risk_delta = np.zeros(self.rules.total_rods)

    def reset(self):
        self.invalidate_forecast()
        self.reactor = Reactor(self.rules)
        self.selected = self.reactor.opening_cluster
        self.phase, self.repair, self.foam = "console", None, None
        self.paused = self.spraying = False
        self.drag_rod, self.drag_point = None, (0, 0)
        self.hover_rod = None
        self.flow_distance = np.zeros(self.rules.total_rods)
        self.visual_time = 0.
        self.socket_ports, self.rod_ports = {}, {}
        self.forecast_ms, self.forecast_sim_time = 0., 0.
        self.rod_accumulator = self.foam_accumulator = 0.
        self.completed, self.last = 0, time.perf_counter()
        self.pause_button.configure(text="PAUSE [Space]")
        self.canvas.xview_moveto(0)
        self.canvas.yview_moveto(0)
        self.refresh()

    def toggle_pause(self):
        self.paused = not self.paused
        self.release()
        self.pause_button.configure(text="RESUME [Space]" if self.paused else "PAUSE [Space]")

    def release(self):
        self.spraying = False
        self.drag_rod = None
        self.hover_rod = None

    def finish_drag(self, event):
        self.spraying = False
        if self.drag_rod is not None and event.widget == self.canvas and not self.paused:
            x, y = self.canvas.canvasx(event.x), self.canvas.canvasy(event.y)
            radius = self.cfg.ui.socket_radius_pixels*2
            for socket, (sx, sy) in self.socket_ports.items():
                if (x-sx)**2+(y-sy)**2 <= radius**2:
                    self.reactor.reroute(self.drag_rod, socket)
                    self.invalidate_forecast()  # An old routing's forecast is no longer valid.
                    break
        self.drag_rod = None
        self.refresh()

    def dispatch(self):
        if self.paused:
            return
        if self.phase == "console":
            self.invalidate_forecast()
            # Deliberate workload handoff: cancellation is cooperative, so drain
            # its current transition BEFORE particle simulation can start.
            if self.job is not None:
                self.job.result()
                self.job = None
            # A rod is the user-facing target; choose its hottest axial location internally.
            rod = self.selected//self.rules.segments
            self.selected = rod*self.rules.segments + int(self.reactor.heat.reshape(
                self.rules.total_rods, self.rules.segments)[rod].argmax())
            self.repair = Repair(self.reactor, self.selected)
            self.foam = Foam(self.rules, self.repair.chamber, self.cfg.foam.seed + self.completed)
            self.phase, self.foam_accumulator, self.spraying = "chamber", 0., False
            self.drag_rod = None
        elif self.phase == "chamber" and self.repair.won:
            self.completed += 1
            self.phase = "console" if self.reactor.settle_after_repair() else "failed"
            self.invalidate_forecast()
            self.rod_accumulator = 0.
        elif self.phase == "failed" or (self.repair and self.repair.lost):
            self.reset()
        self.canvas.xview_moveto(0)
        self.canvas.yview_moveto(0)
        self.refresh()

    def motion(self, event):
        x, y = self.canvas.canvasx(event.x), self.canvas.canvasy(event.y)
        self.drag_point = (x, y)
        self.hover_rod = None
        if self.phase == "console":
            for x0,y0,x1,y1,i in self.boxes:
                if x0 <= x <= x1 and y0 <= y <= y1:
                    self.hover_rod = i//self.rules.segments
                    break
            if self.hover_rod is None:
                for item in reversed(self.canvas.find_overlapping(x-4,y-4,x+4,y+4)):
                    cable = next((tag for tag in self.canvas.gettags(item) if tag.startswith("cable_")), None)
                    if cable is not None:
                        self.hover_rod = int(cable.split("_")[1])
                        break
            self.canvas.configure(cursor="hand2" if self.hover_rod is not None else "")
        if self.foam:
            self.mouse = (x*self.foam.width/self.world_width, y*self.foam.height/self.world_height)

    def press(self, event):
        self.motion(event)
        if self.phase == "console":
            x, y = self.drag_point
            # Grab a connected bank plug, or grab the rod itself.
            radius = self.cfg.ui.socket_radius_pixels*2
            for rod, socket in enumerate(self.reactor.sockets):
                sx, sy = self.socket_ports[int(socket)]
                if (x-sx)**2+(y-sy)**2 <= radius**2:
                    self.selected = rod*self.rules.segments
                    self.drag_rod = rod if not self.paused else None
                    self.refresh()
                    return
            for x0, y0, x1, y1, i in self.boxes:
                if x0 <= x <= x1 and y0 <= y <= y1:
                    self.selected = i
                    self.drag_rod = i//self.rules.segments if not self.paused else None
                    break
            else:
                # Cables can also be grabbed along their length, not just at plugs.
                for item in reversed(self.canvas.find_overlapping(x-3,y-3,x+3,y+3)):
                    tags = self.canvas.gettags(item)
                    cable = next((tag for tag in tags if tag.startswith("cable_")), None)
                    if cable is not None:
                        rod = int(cable.split("_")[1])
                        self.selected = rod*self.rules.segments
                        self.drag_rod = rod if not self.paused else None
                        break
        elif self.phase == "chamber" and not self.paused:
            self.spraying = True
        self.refresh()

    def poll_forecast(self, now):
        if self.job is not None and self.job.done():
            result, elapsed = self.job.result()
            if result is not None and self.job_version == self.version and self.phase == "console":
                self.risk_delta = (np.zeros(self.rules.total_rods) if self.prediction is None else
                                   result.rod_probability-self.prediction.rod_probability)
                self.prediction, self.forecast_ms = result, elapsed
                self.forecast_sim_time = self.job_tick*self.cfg.timing.rod_step_seconds
            self.job = None
        if self.phase == "console" and not self.paused and self.job is None and now >= self.forecast_due:
            self.job_version, self.job_tick = self.version, self.reactor.ticks
            self.cancel_job = threading.Event()
            self.job = self.executor.submit(calculate, self.rules, self.reactor.heat.copy(),
                self.reactor.fault.copy(), self.reactor.ticks, self.cancel_job, self.reactor.sockets.copy())
            self.forecast_due = now+self.cfg.forecast.refresh_seconds

    def frame(self):
        if self.closed:
            return
        now = time.perf_counter()
        dt, self.last = now-self.last, now
        if not self.paused:
            if self.phase == "console":
                output = self.rules.rod_output(self.reactor.ticks,self.reactor.sockets)
                self.visual_time += min(dt,.25)
                self.flow_distance += min(dt,.25)*self.rules.flow_speed(output)
                self.rod_accumulator += min(dt, .25)
                while self.rod_accumulator >= self.cfg.timing.rod_step_seconds:
                    self.rod_accumulator -= self.cfg.timing.rod_step_seconds
                    self.reactor.step()
                    if (np.any(self.reactor.heat >= self.cfg.thermal.critical_heat) or
                            np.any(self.reactor.blackout >= self.cfg.power.blackout_seconds)):
                        self.phase = "failed"
                        break
            elif self.phase == "chamber" and not (self.repair.won or self.repair.lost):
                self.repair.advance(dt)
                if self.repair.travel <= 0 and not self.repair.lost:
                    self.foam_accumulator += min(dt, .15)
                    while self.foam_accumulator >= 1/self.cfg.foam.steps_per_second:
                        self.foam_accumulator -= 1/self.cfg.foam.steps_per_second
                        if self.spraying:
                            self.foam.inject(*self.mouse)
                        contacts = self.foam.step()
                        if self.spraying or not self.cfg.cooling.require_active_spray:
                            self.repair.cool(contacts)
        self.poll_forecast(now)
        self.refresh()
        self.root.after(self.cfg.ui.frame_milliseconds, self.frame)

    def name(self, i):
        c, r, _ = self.rules.decode(int(i))
        return f"Reactor {c+1} / Rod {r+1:02}"

    def refresh(self):
        self.canvas.delete("all")
        self.boxes = []
        self.draw_console() if self.phase in ("console", "failed") else self.draw_chamber()
        u, r, cfg = self.cfg.ui, self.rules, self.cfg
        rod = self.selected//r.segments
        heat = self.reactor.heat.reshape(r.total_rods, r.segments)
        bank = int(self.reactor.sockets[rod])//cfg.power.plugs_per_bank
        load = r.rod_load(self.reactor.ticks, self.reactor.sockets)[rod]
        output = r.rod_output(self.reactor.ticks, self.reactor.sockets)[rod]
        connected = np.count_nonzero(self.reactor.sockets//cfg.power.plugs_per_bank == bank)
        target_heat = r.operating_heat(self.reactor.ticks,self.reactor.sockets)[rod]
        self.status.configure(text=f"{self.config_path.name} | {r.total_rods} rods / {cfg.power.banks} banks | "
            f"repairs {self.completed} | t={self.reactor.ticks*cfg.timing.rod_step_seconds:.0f}s"
            + (" | PAUSED" if self.paused else ""))
        self.headline.configure(fg=u.linked_color)
        if self.phase == "console":
            self.mode.configure(text="REROUTE / FORECAST")
            risk = "sampling..." if self.prediction is None else f"{self.prediction.rod_probability[rod]:.0%}"
            delta = self.risk_delta[rod]
            trend = "steady" if abs(delta)<.01 else f"{'up' if delta>0 else 'down'} {abs(delta):.0%}"
            self.headline.configure(text=self.name(self.selected))
            self.detail.configure(text=f"Rod meltdown chance\nwithin the next {r.horizon:g}s: {risk}\nRecent change: {trend}\n"
                f"Hottest point: {int(heat[rod].max())}/{cfg.thermal.critical_heat}\n"
                f"{r.bank_name(bank)}\n"
                f"{gw(r.bank_demand(self.reactor.ticks)[bank])} GW / {connected} rods\n"
                f"= {gw(load)} GW requested each\n"
                f"Output: {gw(output)}/{gw(cfg.power.rod_capacity_gw)} GW ({output/cfg.power.rod_capacity_gw:.0%})\n"
                f"Load-driven heat target: {target_heat}\n"
                f"{self.thermal_status(rod)} ({self.reactor.heat_trend[rod]:+.2f}/s)\n\n"
                + ("RUNAWAY: foam required.\nRerouting cannot clear it." if self.reactor.runaway_rods[rod] else
                   "Reroute to reduce load before\na hot spot goes runaway."))
            self.action.configure(text="VISIT / COOL THIS ROD [E]", state="normal")
            self.message.configure(text="Drag a rod or connected plug\nto another bank socket.\n"
                "Occupied socket = swap rods.\n\nBank demand is shared among\nits connected rods. Moving one\nalso changes the others' load.")
            others = np.flatnonzero(self.reactor.sockets//cfg.power.plugs_per_bank == bank)
            self.coverage.configure(text="SAME POWER BANK\n"+"\n".join(self.name(i*r.segments) for i in others)
                + "\n\n"+("WARNING: a bank has unmet\ndemand. Add more rod capacity." if np.any(self.reactor.blackout>0)
                           else "Heat colors = current heat.\nPercent = future rod meltdown."))
            self.compute.configure(text=f"{cfg.forecast.trials} futures x {r.steps} steps\n"
                f"{r.count:,} internal thermal points\nLast CPU batch: {self.forecast_ms:.1f} ms\n"
                f"Snapshot: t={self.forecast_sim_time:g}s\n"
                f"Resident futures: {min(cfg.forecast.trials,cfg.forecast.trial_batch_size)}\n"
                f"Rod result payload: {4+2*r.total_rods} bytes\n"
                "Assumes current routing remains.\nNo operator cooling in forecast.\n\n"
                +(f"Neutron transport: {cfg.neutron.walkers_per_rod} sources/rod\n"
                  f"{cfg.neutron.max_steps} steps, {cfg.neutron.generations} generations\n"
                  "Fissions -> heat -> future failures.\n" if cfg.neutron.enabled else "Legacy thermal/PRNG model.\n")+
                "No measured FPGA power claim.")
        elif self.phase == "failed":
            self.mode.configure(text="SHIP LOST")
            self.headline.configure(text="TOO LATE", fg="#ff6866")
            cause = "A power bank stayed underpowered." if np.any(
                self.reactor.blackout >= cfg.power.blackout_seconds) else "A rod reached critical heat."
            self.detail.configure(text=cause+"\nTry a different routing/cooling\npriority, or tune SETTINGS.")
            self.action.configure(text="TRY AGAIN", state="normal")
            self.message.configure(text="SETTINGS / F5 / R")
            self.coverage.configure(text="")
            self.compute.configure(text="Forecasts stopped.")
        else:
            p = self.repair
            self.mode.configure(text="FOAM ON / THERMAL MODEL FROZEN")
            self.headline.configure(text=f"CHAMBER {p.chamber+1}")
            self.detail.configure(text=f"Target: {self.name(p.target)}\n"
                f"Rod contact: {p.contact_seconds:.1f}/{cfg.timing.treatment_seconds:g}s\n"
                f"Hottest point: {heat[rod].max()}/{cfg.thermal.safe_heat} safe\n\n"
                "Hold left mouse to spray.\nSweep across the glowing area.\nANY rod touched by foam cools.\n"
                "Treat the outlined rod to return.\n\nOther thermal changes pause;\nthe repair fuse does not.")
            if p.won:
                self.message.configure(text="TREATMENT COMPLETE\nRunaway cleared; cooling persists.")
                self.action.configure(text="RETURN TO CONSOLE [E]", state="normal")
            elif p.lost:
                self.message.configure(text="REPAIR FUSE EXPIRED")
                self.action.configure(text="TRY AGAIN", state="normal")
            else:
                self.message.configure(text=f"{'TRAVELLING' if p.travel>0 else 'REPAIR FUSE'}: {p.remaining:.1f}s")
                self.action.configure(text="COMPLETE TARGET TREATMENT", state="disabled")
            self.coverage.configure(text="CHAMBER / HOTTEST POINT\n"+"\n".join(
                f"Rod {k+1:02}   {int(heat[p.chamber*r.rods+k].max())}/{cfg.thermal.critical_heat}"
                for k in range(r.rods)))
            self.compute.configure(text=f"2D foam: {self.foam.width}x{self.foam.height}\n"
                f"Pulse: {cfg.cooling.pulse_seconds:g}s contact\n"
                f"Axial cooling reach: {cfg.cooling.axial_radius_cells} internal points\n"
                "No Monte Carlo while spraying.")

    def draw_gradient(self, rod, x0, y0, x1, y1):
        r = self.rules
        values = self.reactor.heat[rod*r.segments:(rod+1)*r.segments]
        sigma = self.cfg.ui.gradient_smoothing_cells
        if sigma > 0:
            radius = max(1, math.ceil(sigma*3))
            kernel = np.exp(-.5*(np.arange(-radius,radius+1)/sigma)**2)
            values = np.convolve(np.pad(values.astype(float),radius,mode="edge"),kernel/kernel.sum(),mode="valid")
        strips = max(1, min(r.segments, 128, math.ceil(y1-y0)))
        profile = np.interp(np.linspace(0, max(0,r.segments-1), strips), np.arange(r.segments), values)
        for k, value in enumerate(profile):
            a, b = y0+k*(y1-y0)/strips, y0+(k+1)*(y1-y0)/strips
            self.canvas.create_rectangle(x0,a,x1,b+.5,fill=r.heat_color(value),outline="")

    def thermal_status(self, rod):
        if self.reactor.runaway_rods[rod]:
            return "RUNAWAY"
        change = self.reactor.heat_trend[rod]
        return "HEAT +" if change > self.cfg.ui.heat_trend_deadband else "COOL -" if change < -self.cfg.ui.heat_trend_deadband else "STEADY"

    def draw_cable(self, rod, start, end, highlight=False):
        """Sample one Bezier so arrows and the visible wire follow the SAME path."""
        cv,u = self.canvas,self.cfg.ui
        a,d = np.asarray(start,float),np.asarray(end,float)
        b,c = a+[0,u.cable_gap_pixels*.7],d-[0,u.cable_gap_pixels*.7]
        t = np.linspace(0,1,49)[:,None]
        points = (1-t)**3*a+3*(1-t)**2*t*b+3*(1-t)*t*t*c+t**3*d
        coords = points.ravel().tolist()
        tags = (f"cable_{rod}",)
        if highlight:
            cv.create_line(*coords,width=u.cable_width_pixels+6,fill=u.selected_color,tags=tags)
        cv.create_line(*coords,width=u.cable_width_pixels,fill=self.rules.cable_color(rod),tags=tags)
        if self.rules.rod_output(self.reactor.ticks,self.reactor.sockets)[rod] <= 0:
            return
        distances = np.concatenate(([0.],np.cumsum(np.linalg.norm(np.diff(points,axis=0),axis=1))))
        length = distances[-1]
        if length < 20:
            return
        offset = self.flow_distance[rod] % u.flow_arrow_spacing_pixels
        for position in np.arange(offset,length,u.flow_arrow_spacing_pixels):
            if position < 10 or position > length-5:
                continue
            tail,tip = max(0,position-12),min(length,position+4)
            x0,x1 = np.interp([tail,tip],distances,points[:,0])
            y0,y1 = np.interp([tail,tip],distances,points[:,1])
            cv.create_line(x0,y0,x1,y1,fill=u.flow_color,width=3,arrow=tk.LAST,
                           arrowshape=(7,10,4),tags=tags+("flow_arrow",))

    def draw_bank_alarm(self, bank, x, y, width, height):
        """Purely cosmetic: never consume the thermal model's random stream."""
        cv,u = self.canvas,self.cfg.ui
        lit = int(self.visual_time*u.alarm_flash_hz*2)%2 == 0
        color = u.alarm_color if lit else u.alarm_dim_color
        tags = ("bank_alarm",f"bank_alarm_{bank}")
        cv.create_rectangle(x,y+15,x+width,y+height,outline=color,width=4,tags=tags)
        cv.create_oval(x+width-23,y+23,x+width-9,y+37,fill=color,outline=u.alarm_color,tags=tags)
        cv.create_rectangle(x+5,y+height-37,x+width-5,y+height-5,fill=color,outline="",tags=tags)
        cv.create_text(x+width/2,y+height-21,text="NOT ENOUGH POWER",
                       fill=u.text_color if lit else u.alarm_color,font=("Consolas",11,"bold"),tags=tags)
        stamp = int(self.visual_time*12)
        for k in range(u.spark_count):
            # Tiny changing lightning forks at the bank's corners/edges.
            angle = ((bank*71+k*137+stamp*41)%360)*math.pi/180
            length = u.spark_length_pixels*(.4+.6*((k*29+stamp*17)%97)/96)
            sx = x+8 if k%2 else x+width-8
            sy = y+19 if k%3 else y+height-10
            dx,dy = math.cos(angle)*length,math.sin(angle)*length
            cv.create_line(sx,sy,sx+dx*.5-dy*.2,sy+dy*.5+dx*.2,sx+dx,sy+dy,
                           fill=u.spark_color,width=2,tags=("bank_spark",f"bank_spark_{bank}"))

    def draw_console(self):
        cv, r, u, p = self.canvas, self.rules, self.cfg.ui, self.cfg.power
        columns = min(u.panel_columns,r.chambers)
        rodcols = min(u.panel_rod_columns,r.rods)
        rodrows = math.ceil(r.rods/rodcols)
        rows = math.ceil(r.chambers/columns)
        minpw = rodcols*(u.cell_width_pixels+22)+24
        ph = 48+rodrows*(u.rod_height_pixels+100)
        bankcols = min(u.bank_columns,p.banks)
        bankrows = math.ceil(p.banks/bankcols)
        bank_height = max(145,u.bank_height_pixels)
        # Socket density can widen a bank automatically; no hard-coded 4-plug assumption.
        bankminw = max(u.bank_width_pixels,p.plugs_per_bank*(u.socket_radius_pixels*2+10)+24)
        w = max(cv.winfo_width(),columns*(minpw+12)+12,bankcols*(bankminw+12)+12)
        bank_y = 40+rows*(ph+12)+u.cable_gap_pixels
        h = max(cv.winfo_height(),bank_y+bankrows*(bank_height+36)+12)
        self.world_width, self.world_height = w,h
        cv.configure(scrollregion=(0,0,w,h))
        cv.create_text(12,16,text=f"ROD % = MELTDOWN CHANCE IN NEXT {r.horizon:g}s | DRAG CABLES TO REROUTE",
                       fill=u.muted_color,anchor="w",font=("Consolas",10))
        pw = (w-12)/columns-12
        bw = (w-12)/bankcols-12
        self.rod_ports,self.socket_ports = {},{}
        rod_boxes,bank_boxes = [],[]
        for c in range(r.chambers):
            x,y=12+(c%columns)*(pw+12),38+(c//columns)*(ph+12)
            cv.create_rectangle(x,y,x+pw,y+ph,fill=u.panel_color,outline="#24434b")
            cv.create_text(x+10,y+18,text=f"REACTOR {c+1}",fill=u.linked_color,
                           anchor="w",font=("Consolas",12,"bold"))
            pitch=(pw-24)/rodcols
            for k in range(r.rods):
                row,col=divmod(k,rodcols)
                cx=x+12+(col+.5)*pitch
                y0=y+82+row*(u.rod_height_pixels+100)
                half=min(pitch*.27,u.cell_width_pixels/2)
                rod=c*r.rods+k
                box=(cx-half,y0,cx+half,y0+u.rod_height_pixels)
                self.rod_ports[rod]=(cx,box[3]+53)
                rod_boxes.append((rod,k,box))
        for bank in range(p.banks):
            x=12+(bank%bankcols)*(bw+12)
            y=bank_y+(bank//bankcols)*(bank_height+36)
            bank_boxes.append((bank,x,y))
            for plug in range(p.plugs_per_bank):
                self.socket_ports[bank*p.plugs_per_bank+plug]=(x+(plug+.5)*bw/p.plugs_per_bank,y)
        # Cables behind rods/plugs, selected cable above others.
        selected=self.selected//r.segments
        front = self.hover_rod if self.hover_rod is not None else selected
        order=[i for i in range(r.total_rods) if i!=front]+[front]
        for rod in order:
            sx,sy=self.rod_ports[rod]
            socket=int(self.reactor.sockets[rod])
            tx,ty=self.socket_ports[socket]
            bank=socket//p.plugs_per_bank
            if rod==self.drag_rod:
                tx,ty=self.drag_point
            self.draw_cable(rod,(sx,sy),(tx,ty),rod==front)
        runaway=self.reactor.runaway_rods
        outputs=r.rod_output(self.reactor.ticks,self.reactor.sockets)
        for rod,k,(x0,y0,x1,y1) in rod_boxes:
            self.draw_gradient(rod,x0,y0,x1,y1)
            color=r.rod_color(rod)
            outline=u.selected_color if rod in (selected,self.hover_rod) else color
            cv.create_rectangle(x0,y0,x1,y1,outline=outline,width=2)
            cv.create_rectangle(x0-5,y0,x0-2,y1,fill=color,outline="")
            cv.create_text((x0+x1)/2,y0-39,text=f"R{k+1:02}",fill=color,font=("Consolas",10,"bold"))
            cv.create_text((x0+x1)/2,y0-24,text=f"{gw(outputs[rod])}/{gw(p.rod_capacity_gw)}GW",fill=u.text_color,font=("Consolas",8))
            cv.create_rectangle(x0,y0-12,x1,y0-6,fill=u.background_color,outline="")
            utilization=outputs[rod]/p.rod_capacity_gw
            cv.create_rectangle(x0,y0-12,x0+(x1-x0)*utilization,y0-6,fill=color,outline="")
            risk=None if self.prediction is None else self.prediction.rod_probability[rod]
            text="..." if risk is None else f"{risk:.0%}"
            cv.create_rectangle((x0+x1)/2-31,y1+3,(x0+x1)/2+31,y1+25,
                                fill=u.panel_color if risk is None else r.risk_color(risk),outline="")
            cv.create_text((x0+x1)/2,y1+14,text=f"{text}/{r.horizon:g}s"+("!" if runaway[rod] else ""),
                fill=u.text_color,font=("Consolas",9,"bold"),tags=("rod_forecast_label",))
            status=self.thermal_status(rod)
            cv.create_text((x0+x1)/2,y1+34,text=status,
                           fill=u.accent_color if status in ("HEAT +","RUNAWAY") else u.linked_color,font=("Consolas",8))
            sx,sy=self.rod_ports[rod]
            cv.create_oval(sx-5,sy-5,sx+5,sy+5,fill=color,outline=u.text_color)
            self.boxes.append((x0-7,y0-48,x1+7,y1+60,rod*r.segments))
        demand=r.bank_demand(self.reactor.ticks)
        counts=np.bincount(self.reactor.sockets//p.plugs_per_bank,minlength=p.banks)
        radius=u.socket_radius_pixels
        occupied={int(socket):rod for rod,socket in enumerate(self.reactor.sockets)}
        deficit=r.bank_deficit(self.reactor.ticks,self.reactor.sockets)
        for bank,x,y in bank_boxes:
            color=r.bank_color(bank)
            cv.create_rectangle(x,y+15,x+bw,y+bank_height,fill=u.panel_color,outline=color,width=2)
            cv.create_text(x+10,y+36,text=r.bank_name(bank),anchor="w",fill=color,font=("Consolas",11,"bold"))
            cv.create_text(x+10,y+57,text=f"Draw {gw(demand[bank])} GW / {int(counts[bank])} rods",
                           anchor="w",fill=u.text_color,font=("Consolas",10))
            share=min(demand[bank]/counts[bank],p.rod_capacity_gw) if counts[bank] else 0
            detail=f"{gw(share)} GW each" if deficit[bank] <= 1e-6 else f"SHORT {gw(deficit[bank])} GW / {max(0,p.blackout_seconds-self.reactor.blackout[bank]):.0f}s"
            cv.create_text(x+10,y+77,text=detail,anchor="w",fill=u.accent_color,font=("Consolas",10))
            cv.create_rectangle(x+10,y+92,x+bw-10,y+99,fill=u.background_color,outline="")
            fraction=demand[bank]/max(1,p.demand_max)
            cv.create_rectangle(x+10,y+92,x+10+(bw-20)*fraction,y+99,fill=color,outline="")
            if deficit[bank] > 1e-6:
                self.draw_bank_alarm(bank,x,y,bw,bank_height)
            for plug in range(p.plugs_per_bank):
                socket=bank*p.plugs_per_bank+plug
                sx,sy=self.socket_ports[socket]
                cv.create_oval(sx-radius,sy-radius,sx+radius,sy+radius,
                               fill=r.rod_color(occupied[socket]) if socket in occupied else u.background_color,
                               outline=u.text_color,width=2)

    def draw_chamber(self):
        cv,r,p,f,u=self.canvas,self.rules,self.repair,self.foam,self.cfg.ui
        columns=min(u.room_rod_columns,r.rods)
        w=max(cv.winfo_width(),columns*100)
        h=max(cv.winfo_height(),math.ceil(r.rods/columns)*(u.rod_height_pixels+60))
        self.world_width,self.world_height=w,h
        cv.configure(scrollregion=(0,0,w,h))
        sx,sy=w/f.width,h/f.height
        cv.create_polygon(0,h,w,h,w*.85,h*.1,w*.15,h*.1,fill="#17282e",outline="#40616a")
        for x in range(0,f.width,max(1,f.width//12)):
            cv.create_line(w/2,h*.08,x*sx,h,fill="#223b42")
        for k,(x0,y0,x1,y1) in enumerate(f.bounds):
            rod=p.chamber*r.rods+k
            self.draw_gradient(rod,x0*sx,y0*sy,x1*sx,y1*sy)
            cv.create_line((x0+1)*sx,y0*sy,(x0+1)*sx,y1*sy,fill="#b4c8af",width=2)
            cv.create_line((x0-1)*sx,y0*sy,(x0-1)*sx,y1*sy,fill=r.rod_color(rod),width=4)
            if k==p.rod:
                cv.create_rectangle((x0-2)*sx,y0*sy,(x1+2)*sx,y1*sy,outline=u.selected_color,width=3)
            cv.create_oval(x0*sx,(y0-1)*sy,x1*sx,(y0+1)*sy,fill="#6f7a73",outline="#c4bfa1")
            cv.create_text((x0+x1)*sx/2,(y1+5)*sy,text=f"ROD {k+1:02}",
                           fill=r.rod_color(rod),font=("Consolas",10))
        for material,color in ((1,u.foam_color),(2,u.debris_color)):
            points=np.argwhere(f.cells==material)
            for y,x in points[::max(1,math.ceil(len(points)/u.max_drawn_particles))]:
                cv.create_rectangle(x*sx,y*sy,(x+1)*sx,(y+1)*sy,fill=color,outline="")
        if self.spraying and p.travel<=0 and not (p.won or p.lost):
            cv.create_line(w*.52,h,self.mouse[0]*sx,self.mouse[1]*sy,fill=u.foam_color,width=3,dash=(4,7))
        text="TREATMENT COMPLETE [E]" if p.won else "DEADLINE MISSED [R]" if p.lost else (
            "RUNNING TO CHAMBER..." if p.travel>0 else f"{p.remaining:.1f}s / spray the glowing hot spots")
        cv.create_rectangle(0,0,w,30,fill=u.background_color,outline="")
        cv.create_text(w/2,15,text=text,fill=u.accent_color,font=("Consolas",13,"bold"))

    def close(self):
        self.closed=True
        if getattr(self, "cancel_job", None) is not None:
            self.cancel_job.set()
        self.executor.shutdown(wait=False,cancel_futures=True)
        self.root.destroy()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config",type=Path,default=DEFAULT_PATH)
    parser.add_argument("--check-config",action="store_true")
    parser.add_argument("--self-test",action="store_true")
    parser.add_argument("--smoke-test",action="store_true")
    parser.add_argument("--screenshot",type=Path)
    parser.add_argument("--chamber",action="store_true")
    args=parser.parse_args()
    if not args.config.is_absolute() and not args.config.exists():
        args.config=HERE/args.config
    if args.self_test:
        import unittest,test_model
        result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromModule(test_model))
        raise SystemExit(not result.wasSuccessful())
    if args.check_config:
        rules=Rules(load_config(args.config))
        print(f"OK: {rules.chambers} reactors x {rules.rods} rods x {rules.segments} cells = {rules.count}; "
              f"{rules.cfg.power.banks} banks x {rules.cfg.power.plugs_per_bank} plugs; horizon {rules.horizon:g}s")
        return
    import sys
    if sys.platform=="win32":
        import ctypes
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except (AttributeError,OSError):
            pass
    root=tk.Tk()
    app=Lab(root,args.config)
    if args.chamber:
        app.dispatch()
    if args.smoke_test:
        from test_model import gui_smoke
        try:
            gui_smoke(app)
        finally:
            app.close()
        return
    if args.screenshot:
        def capture():
            from PIL import ImageGrab
            root.update_idletasks()
            x,y=root.winfo_rootx(),root.winfo_rooty()
            ImageGrab.grab(bbox=(x,y,x+root.winfo_width(),y+root.winfo_height())).save(args.screenshot)
            print(f"Screenshot: {args.screenshot}")
            app.close()
        root.after(1800,capture)
    root.mainloop()


if __name__=="__main__":
    main()
