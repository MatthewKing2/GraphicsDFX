"""Interactive teaching aid for the game's synthetic 128-state failure model.

Python/Tk only. Does not launch, edit or drive the game. The integer model is
checked against C++ reactorStep using --self-test after building this folder.
"""
import argparse
from collections import deque
import json
from pathlib import Path
import queue
import subprocess
import threading
import time
import tkinter as tk
from tkinter import ttk

HERE = Path(__file__).resolve().parent
MASK32 = 0xffffffff
COLORS = ["#30be99", "#b5bd62", "#dbb34c", "#f39a3e", "#f07744", "#ee5a51", "#e74962", "#d53875"]
BANK_COLORS = ["#58cce9", "#b19bff", "#f6bf62", "#71dbaa"]
BG, PANEL, TEXT, MUTED = "#101720", "#192531", "#edf3f7", "#a5b6c5"


def random_next(x):
    x ^= (x << 13) & MASK32
    x ^= x >> 17
    x ^= (x << 5) & MASK32
    return x & MASK32


def mix(x):
    x ^= x >> 16
    x = x * 0x7feb352d & MASK32
    x ^= x >> 15
    x = x * 0x846ca68b & MASK32
    return (x ^ (x >> 16)) | 1


def rules_for(seed=12345):
    rng = mix(seed ^ 0x72656163)
    rules = []
    for node in range(128):
        rng = random_next(rng)
        a, b, c = (node + 127) & 127, (node + 17) & 127, (rng >> 9) & 127
        rules.append(a | b << 7 | c << 14 | (node >> 5) << 21 | (4 + (rng & 7)) << 23)
    return rules


def bit(state, node, plane=0):
    return state[plane * 4 + (node >> 5)] >> (node & 31) & 1


def age(state, node):
    return bit(state, node, 1) | bit(state, node, 2) << 1 | bit(state, node, 3) << 2


def set_node(state, node, fault, warning):
    word, mask = node >> 5, 1 << (node & 31)
    for plane, value in enumerate([fault, warning & 1, warning >> 1 & 1, warning >> 2 & 1]):
        pos = plane * 4 + word
        state[pos] = (state[pos] | mask) if value else (state[pos] & ~mask)


def transition(rules, old, tick, power, rng, trace_node=None):
    """All inputs read OLD; commit NEXT simultaneously. Exactly the C++ rule."""
    result = [0] * 16
    trace = None
    for node, rule in enumerate(rules):
        a, b, c, bank = rule & 127, rule >> 7 & 127, rule >> 14 & 127, rule >> 21 & 3
        rate = (rule >> 23 & 255) + min(20, tick // 180)
        if power & (1 << bank):
            rate = max(1, rate // 4)
        if power & (1 << ((bank + 3) & 3)):
            rate += 8
        rng = random_next(rng)
        causes = [bool((rng & 255) < rate),
                  bool(bit(old, a) and bit(old, b) and (rng >> 24 & 255) < 96),
                  bool(bit(old, c) and (rng >> 16 & 255) < 12),
                  bool(bit(old, node) and (rng >> 8 & 255) >= 64)]
        fault = any(causes)
        previous_age = age(old, node)
        warning = min(7, previous_age + 1) if fault else max(0, previous_age - 2)
        set_node(result, node, fault, warning)
        if node == trace_node:
            trace = dict(causes=causes, rate=rate, before=previous_age, after=warning,
                         inputs=[bit(old, a), bit(old, b), bit(old, c)], rng=rng,
                         old_fault=bit(old, node))
    return result, rng, trace


def self_test():
    rules = rules_for()
    exe = HERE / "../../build/relay_lab/compute.exe"
    if not exe.exists():
        raise RuntimeError("Build the C++ reference first: make -C graphics/vulkan/tools/relay_lab all")
    fixture = json.loads(subprocess.check_output([str(exe), "--fixture"], text=True))
    assert fixture["rules"] == rules, "rule-table generation differs from game"
    checks = 0
    for case in fixture["cases"]:
        state, tick, rng = case["initial"], case["tick"], case["rng"]
        for expected in case["steps"]:
            tick += 6
            state, rng, _ = transition(rules, state, tick, expected["mask"], rng)
            assert state == expected["state"] and rng == expected["rng"], f"C++ mismatch at {checks}"
            checks += 1
    initial = fixture["cases"][1]["initial"]
    after, _, _ = transition(rules, initial, 1806, 0, 12345)
    boosted, _, _ = transition(rules, initial, 1806, 1, 12345)
    assert after != boosted, "bank boost does not affect transitions"
    print(f"PASS Python view matches actual C++ rule table and {checks} complete 512-bit states/RNGs")
    root = tk.Tk()
    root.withdraw()
    try:
        app = RelayLab(root)
        before = app.state.copy()
        app.pulse()
        assert app.state == before and app.charges == 1, "pulse changed relay state"
        app.reroute(0)
        assert app.state == before and app.power == 1 and app.charges == 2
        for _ in range(8):
            app.step()
        assert app.rng == app.base_rng, "counterfactual does not share random draws"
        assert app.state != app.baseline, "rerouting has no observable effect"
        app.inject()
        assert bit(app.state, app.selected) == 1 and age(app.state, app.selected) == 7
        app.futures()
        deadline = time.perf_counter() + 15
        while app.future_button.cget("state") == "disabled" and time.perf_counter() < deadline:
            root.update()
            time.sleep(.01)
        assert len(app.future_colors) == 64, "background future preview failed"
        app.reset()
        assert not any(app.state) and app.tick == 0 and app.power == 0
    finally:
        root.destroy()
    print("PASS visualization pulse/reroute/injection/reset, paired replay and 64-future preview")


class RelayLab:
    def __init__(self, root):
        self.root = root
        root.title("Relay lab — what are these 128 things?")
        root.configure(bg=BG)
        root.geometry("1260x920")
        root.minsize(1160, 860)
        self.rules = rules_for()
        self.selected = 17
        self.running = False
        self.period = tk.DoubleVar(value=.5)
        self.history = deque(maxlen=180)
        self.messages = deque(maxlen=3)
        self.future_queue = queue.Queue()
        self.future_colors = []
        self.future_text = "Optional: sample 64 possible futures of this same snapshot."
        self.version = 0
        self.trace = None
        self.positions = {}
        self.make_widgets()
        root.bind("<space>", lambda _: self.toggle())
        root.bind("<Right>", lambda _: self.step())
        root.bind("r", lambda _: self.reset())
        root.bind("p", lambda _: self.pulse())
        for bank in range(4):
            root.bind(str(bank + 1), lambda _, b=bank: self.reroute(b))
        self.reset(warm=True)
        self.last_step = time.perf_counter()
        root.after(60, self.update)

    def label(self, parent, text="", size=11, color=TEXT, **kwargs):
        return tk.Label(parent, text=text, font=("Segoe UI", size), fg=color,
                        bg=parent.cget("bg"), anchor="w", justify="left", **kwargs)

    def button(self, parent, text, command):
        return tk.Button(parent, text=text, command=command, font=("Segoe UI", 10),
                         bg="#26394a", fg=TEXT, activebackground="#385568",
                         activeforeground="white", relief="flat", padx=11, pady=5)

    def make_widgets(self):
        top = tk.Frame(self.root, bg=BG)
        top.pack(fill="x", padx=18, pady=(10, 5))
        self.label(top, "128 tiny state machines. Not real electrical relays.", 20).pack(anchor="w")
        self.label(top, "Each light stores 1 fault bit + a 3-bit warning counter. Three other lights influence its NEXT state.", 11, MUTED).pack(anchor="w")
        controls = tk.Frame(self.root, bg=BG)
        controls.pack(fill="x", padx=18, pady=5)
        self.play_button = self.button(controls, "▶ Play [Space]", self.toggle)
        self.play_button.pack(side="left", padx=(0, 5))
        self.button(controls, "Step 0.1 s [→]", self.step).pack(side="left", padx=5)
        self.button(controls, "Reset healthy [R]", self.reset).pack(side="left", padx=5)
        self.button(controls, "Load 30 s snapshot", lambda: self.reset(warm=True)).pack(side="left", padx=5)
        self.label(controls, "Wall seconds / step:", 10, MUTED).pack(side="left", padx=(20, 2))
        tk.Scale(controls, from_=.1, to=1.0, resolution=.1, orient="horizontal",
                 variable=self.period, length=120, bg=BG, fg=TEXT, highlightthickness=0,
                 troughcolor=PANEL).pack(side="left")
        body = tk.Frame(self.root, bg=BG)
        body.pack(fill="both", expand=True, padx=12)
        left = tk.Frame(body, bg=BG)
        left.pack(side="left", fill="both", expand=True)
        self.canvas = tk.Canvas(left, width=770, height=535, bg=BG, highlightthickness=0)
        self.canvas.pack(fill="both", expand=True)
        self.canvas.bind("<Button-1>", self.click)
        self.status = self.label(left, size=10, color=MUTED)
        self.status.pack(anchor="w", padx=8)
        self.plot = tk.Canvas(left, height=84, bg=PANEL, highlightthickness=0)
        self.plot.pack(fill="x", padx=8, pady=(4, 5))
        self.log = self.label(left, size=10, color=MUTED, wraplength=740, height=3)
        self.log.pack(fill="x", padx=8)
        right = tk.Frame(body, bg=PANEL, width=440)
        right.pack(side="right", fill="y", padx=(8, 0), pady=8)
        right.pack_propagate(False)
        self.title = self.label(right, size=16)
        self.title.pack(fill="x", padx=12, pady=(8, 2))
        self.details = self.label(right, size=10, wraplength=412)
        self.details.configure(font=("Consolas", 10))
        self.details.pack(fill="x", padx=12, pady=3)
        self.door = tk.Canvas(right, width=410, height=65, bg=PANEL, highlightthickness=0)
        self.door.pack(fill="x", padx=12)
        self.label(right, "This sample door reads the selected light's age.\nA pulse bypasses the door; it does NOT fix the relays.", 10, MUTED).pack(fill="x", padx=12, pady=4)
        self.pulse_button = self.button(right, "Emergency pulse [P]", self.pulse)
        self.pulse_button.pack(fill="x", padx=12, pady=4)
        self.button(right, "Inject selected fault (LAB ONLY)", self.inject).pack(fill="x", padx=12, pady=4)
        self.label(right, "Game action: fly through a green ring", 11).pack(fill="x", padx=12, pady=(8, 2))
        self.bank_buttons = []
        for bank in range(4):
            button = self.button(right, "", lambda b=bank: self.reroute(b))
            button.pack(fill="x", padx=12, pady=1)
            self.bank_buttons.append(button)
        self.label(right, "Boost: own spontaneous-fault rate ÷4.\nCost: next bank gets +8/256 fault chance.\nRing also recharges a pulse and grants a short bypass.", 10, MUTED).pack(fill="x", padx=12, pady=6)
        bottom = tk.Frame(self.root, bg=BG)
        bottom.pack(fill="x", padx=18, pady=(2, 12))
        self.future_button = self.button(bottom, "Try 64 possible futures (+2 s)", self.futures)
        self.future_button.pack(anchor="w")
        self.futures_canvas = tk.Canvas(bottom, height=34, bg=BG, highlightthickness=0)
        self.futures_canvas.pack(fill="x")
        self.future_label = self.label(bottom, size=10, color=MUTED)
        self.future_label.pack(fill="x")

    def reset(self, warm=False):
        self.running = False
        self.state, self.baseline = [0] * 16, [0] * 16
        self.rng = self.base_rng = 12345
        self.tick = 0
        self.power = 0
        self.charges, self.pulse_until = 2, 0
        self.trace = None
        self.version += 1
        self.future_colors = []
        self.future_text = "64 futures are a teaching demo, not a game survival forecast."
        self.history.clear()
        self.messages.clear()
        if warm:
            for _ in range(300):
                self.tick += 6
                self.state, self.rng, self.trace = transition(self.rules, self.state, self.tick, 0, self.rng, self.selected)
            self.baseline, self.base_rng = self.state.copy(), self.rng
        self.note("Paused. Click any numbered light; press → to follow one simultaneous update.")
        self.redraw()

    def note(self, message):
        self.messages.append(f"{self.tick / 60:.1f}s  {message}")

    def toggle(self):
        self.running = not self.running
        self.last_step = time.perf_counter()
        self.redraw()

    def step(self):
        self.tick += 6
        self.state, self.rng, self.trace = transition(self.rules, self.state, self.tick, self.power, self.rng, self.selected)
        self.baseline, self.base_rng, _ = transition(self.rules, self.baseline, self.tick, 0, self.base_rng)
        self.history.append([sum(age(self.state, node) >= 4 for node in range(b * 32, (b + 1) * 32)) for b in range(4)])
        self.version += 1
        self.redraw()

    def reroute(self, bank):
        self.power ^= 1 << bank
        self.charges = min(2, self.charges + 1)
        self.pulse_until = max(self.pulse_until, self.tick + 36)
        self.version += 1
        self.note(f"Ring {chr(65+bank)}: bank {'boosted' if self.power & (1 << bank) else 'unboosted'}; neighbors react on following ticks.")
        self.redraw()

    def pulse(self):
        if self.charges and self.tick >= self.pulse_until:
            self.charges -= 1
            self.pulse_until = self.tick + 72
            self.version += 1
            self.note("Pulse: 1.2 simulated seconds of door/laser bypass. The network itself is unchanged.")
        else:
            self.note("No pulse used: already bypassing, or no charges left.")
        self.redraw()

    def inject(self):
        set_node(self.state, self.selected, 1, 7)
        self.trace = None
        self.version += 1
        self.note(f"LAB-ONLY fault injected at {self.selected}. This button is NOT a game mechanic.")
        self.redraw()

    def click(self, event):
        nearest = min(self.positions, key=lambda n: (event.x - self.positions[n][0]) ** 2 + (event.y - self.positions[n][1]) ** 2)
        x, y = self.positions[nearest]
        if abs(event.x - x) < 22 and abs(event.y - y) < 22:
            self.selected = nearest
            self.trace = None
            self.redraw()

    def redraw(self):
        c = self.canvas
        c.delete("all")
        self.play_button.configure(text="Pause [Space]" if self.running else "▶ Play [Space]")
        self.positions = {}
        for bank in range(4):
            bx, by = 12 + (bank % 2) * 378, 12 + (bank // 2) * 255
            c.create_rectangle(bx, by, bx + 362, by + 240, fill=PANEL, outline="#304453")
            status = "BOOSTED" if self.power & (1 << bank) else "normal"
            c.create_text(bx + 12, by + 17, anchor="w", text=f"BANK {chr(65+bank)} · {status}", fill=BANK_COLORS[bank], font=("Segoe UI", 11, "bold"))
            for local in range(32):
                node = bank * 32 + local
                self.positions[node] = bx + 28 + local % 8 * 43, by + 58 + local // 8 * 46
        rule = self.rules[self.selected]
        inputs = [rule & 127, rule >> 7 & 127, rule >> 14 & 127]
        # Only three selected input wires: drawing every wire would hide the lesson.
        x, y = self.positions[self.selected]
        for k, node in enumerate(inputs):
            sx, sy = self.positions[node]
            c.create_line(sx, sy, x, y, fill=["#6ecfff", "#afafff", "#f5c56c"][k], width=2, arrow=tk.LAST, dash=(4, 3))
        differences = 0
        for node, (x, y) in self.positions.items():
            warning = age(self.state, node)
            different = warning != age(self.baseline, node) or bit(self.state, node) != bit(self.baseline, node)
            differences += different
            outline = "#ffffff" if node == self.selected else "#e09fff" if different else "#304453"
            c.create_rectangle(x - 17, y - 17, x + 17, y + 17, fill=COLORS[warning], outline=outline, width=3 if node == self.selected or different else 1)
            c.create_text(x, y - 3, text=str(node), fill="#0e1b24", font=("Consolas", 10, "bold"))
            c.create_text(x, y + 10, text=str(warning), fill="#15232e", font=("Consolas", 8))
            if bit(self.state, node):
                c.create_oval(x + 9, y - 15, x + 14, y - 10, fill="#07141b", outline="")
        c.create_text(12, 529, anchor="w", text="Green age 0 · Amber ages 1–3 · Red ages 4–7 · dot = fault bit · white = selected · purple = changed by actions", fill=MUTED, font=("Segoe UI", 9))
        self.status.configure(text=f"Simulated clock {self.tick / 60:.1f}s  |  0.1s per step  |  {differences}/128 differ from no-action replay (same random draws)")
        self.log.configure(text="\n".join(self.messages))
        self.title.configure(text=f"Relay {self.selected} / bank {chr(65 + self.selected // 32)}")
        warning = age(self.state, self.selected)
        detail = (f"NOW: fault={bit(self.state, self.selected)}, warning={warning}/7\n"
                  f"WIRES: A={inputs[0]}, B={inputs[1]}, C={inputs[2]}\n\n"
                  "NEXT fault = any of these events:\n"
                  " • spontaneous failure\n"
                  " • A AND B faulty, then 96/256 chance\n"
                  " • C faulty, then 12/256 chance\n"
                  " • old fault persists (75% chance)\n\n"
                  "NEXT warning: fault ? +1 : −2\n"
                  "Clamp warning to 0..7. All relays\n"
                  "read OLD state, then update together.\n")
        if self.trace:
            t = self.trace
            names = ["spontaneous", "A+B cascade", "C spill", "persistence"]
            causes = ", ".join(name for name, happened in zip(names, t["causes"]) if happened) or "none (recover)"
            detail += (f"\nLAST STEP: {t['before']} → {t['after']}\n"
                       f"Spontaneous rate: {t['rate']}/256\n"
                       f"OLD A/B/C faults: {t['inputs']}\n"
                       f"Cause(s): {causes}")
        else:
            detail += "\nPress → to inspect this relay's next update."
        self.details.configure(text=detail)
        self.door.delete("all")
        pulsed = self.tick < self.pulse_until
        gap = 38 if pulsed or warning < 4 else max(1, 38 * (7 - warning) / 4)
        self.door.create_rectangle(0, 2, 120, 62, fill="#090f15", outline="#49606f")
        for sign in [-1, 1]:
            center = 60 + sign * (gap + (60 - gap) / 2)
            self.door.create_rectangle(center - (60 - gap) / 2, 3, center + (60 - gap) / 2, 61, fill="#7a858c", outline="")
        self.door.create_text(135, 30, anchor="w", fill="#68d8fa" if pulsed else COLORS[warning], text="BYPASSED" if pulsed else "CLOSING" if warning >= 4 else "OPEN", font=("Segoe UI", 13, "bold"))
        self.pulse_button.configure(text=f"Emergency pulse [P] — {self.charges}/2 charges")
        for bank, button in enumerate(self.bank_buttons):
            button.configure(text=f"Ring {chr(65 + bank)} [{bank + 1}]: " + ("BOOST ON" if self.power & (1 << bank) else "boost off"))
        self.plot.delete("all")
        self.plot.create_text(8, 10, text="Dangerous relays per bank (age ≥4)", fill=MUTED, anchor="w", font=("Segoe UI", 9))
        for bank in range(4):
            points = []
            for i, counts in enumerate(self.history):
                points.extend([8 + i * 4, 77 - counts[bank] * 1.65])
            if len(points) >= 4:
                self.plot.create_line(*points, fill=BANK_COLORS[bank], width=2)
        self.futures_canvas.delete("all")
        for i, color in enumerate(self.future_colors):
            self.futures_canvas.create_rectangle(2 + i * 17, 5, 15 + i * 17, 25, fill=color, outline="")
        self.future_label.configure(text=self.future_text)

    def futures(self):
        self.running = False
        version, selected = self.version, self.selected
        snapshot, tick, power = self.state.copy(), self.tick, self.power
        self.future_button.configure(state="disabled")
        self.future_text = "Simulating 64 independent futures in a background CPU thread..."
        self.redraw()
        def work():
            final_ages = []
            for trial in range(64):
                state, rng = snapshot.copy(), mix(12345 ^ (trial * 0x9e3779b9 & MASK32))
                for step in range(20):
                    state, rng, _ = transition(self.rules, state, tick + (step + 1) * 6, power, rng)
                final_ages.append(age(state, selected))
            self.future_queue.put((version, selected, final_ages, tick))
        threading.Thread(target=work, daemon=True).start()

    def update(self):
        if self.running and time.perf_counter() - self.last_step >= self.period.get():
            self.step()
            self.last_step = time.perf_counter()
        try:
            version, selected, final_ages, tick = self.future_queue.get_nowait()
            self.future_button.configure(state="normal")
            if version == self.version and selected == self.selected:
                self.future_colors = [COLORS[a] for a in final_ages]
                count = sum(a >= 4 for a in final_ages)
                self.future_text = (f"Relay {selected} at {tick / 60 + 2:.1f}s: {count}/64 futures have age ≥4. "
                                    "NOT a survival probability. Only 64 trials: high sampling uncertainty.")
            else:
                self.future_text = "Snapshot changed; old sample discarded. Try again while paused."
            self.redraw()
        except queue.Empty:
            pass
        self.root.after(60, self.update)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--capture", type=Path, help="Debug screenshot; requires installed Pillow")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    # Keep Windows logical/physical pixels consistent; otherwise the inspector
    # and screenshot can be clipped on a laptop set to 125% display scaling.
    try:
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        pass
    root = tk.Tk()
    root.tk.call("tk", "scaling", 1.15)
    app = RelayLab(root)
    if args.capture:
        app.reroute(0)
        app.inject()
        for _ in range(12):
            app.step()
        root.attributes("-topmost", True)
        def capture():
            from PIL import ImageGrab
            x, y = root.winfo_rootx(), root.winfo_rooty()
            ImageGrab.grab((x, y, x + root.winfo_width(), y + root.winfo_height())).save(args.capture)
            root.destroy()
        root.after(1500, capture)
    root.mainloop()


if __name__ == "__main__":
    main()
