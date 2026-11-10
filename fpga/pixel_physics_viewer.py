"""Software preview of the FPGA repo's EXACT falling-sand rule and golden worlds.

Git Bash, from the outer repo: python fpga/pixel_physics_viewer.py
Tests: python fpga/pixel_physics_viewer.py --self-test
Needs Python, NumPy and Tk (already installed on this laptop).

This is a CPU/NumPy teaching viewer, NOT the team's Vulkan/FPGA host demo and
NOT a performance or power benchmark. No FPGA, Jetson, or driver is accessed.
The upstream host/GPU source is absent from the supplied repository. Keep this
file OUTSIDE the submodule so its pinned upstream source remains unmodified.
"""
import argparse
from pathlib import Path
import re
import time
import tkinter as tk
from tkinter import ttk

try:
    import numpy as np
except ImportError:
    raise SystemExit("NumPy is required. Install it with: python -m pip install numpy")

HERE = Path(__file__).resolve().parent
UPSTREAM = HERE / "fpga_graphics_dfx"
VECTORS = UPSTREAM / "sim/sand_vectors"
NAMES = ("hourglass_512", "dam_512", "random_512", "torus_512")
PALETTE = np.array([[12, 20, 30], [239, 187, 78], [62, 154, 237], [150, 161, 175]], dtype=np.uint8)
BG, FG = "#111923", "#e5edf3"


def xorshift(value):
    value ^= value << 13 & 0xffffffff
    value ^= value >> 17
    value ^= value << 5 & 0xffffffff
    return value & 0xffffffff


def hex_values(path):
    return [int(token, 16) for token in path.read_text(encoding="ascii").split()]


def unpack(path, width=512, height=512):
    words = np.array(hex_values(path), dtype=np.uint64)
    if words.size != width * height // 32:
        raise ValueError(f"Invalid packed world size: {path}")
    shifts = np.arange(32, dtype=np.uint64) * 2
    return ((words[:, None] >> shifts) & 3).astype(np.uint8).reshape(height, width)


def assets():
    if not VECTORS.exists():
        raise RuntimeError("Missing submodule. Run: git submodule update --init --recursive")
    table = np.array(hex_values(VECTORS / "table.hex"), dtype=np.uint16)
    if table.shape != (512,) or np.any(table > 255):
        raise ValueError("Expected 512 eight-bit rule entries")
    rows = hex_values(VECTORS / "noise.hex")
    if len(rows) != 256:
        raise ValueError("Expected 256 packed noise rows")
    noise = np.array([[(row >> x) & 1 for x in range(256)] for row in rows], dtype=np.uint16)
    return table, noise


class World:
    def __init__(self, table, noise, name="hourglass_512"):
        self.table, self.noise = table, noise
        self.coordinates = np.arange(256)
        self.load(name)

    def load(self, name):
        width, height, steps = hex_values(VECTORS / f"{name}.params.hex")
        if (width, height) != (512, 512):
            raise ValueError("This viewer expects the original 512x512 workload")
        self.name, self.expected_steps = name, steps
        self.cells = unpack(VECTORS / f"{name}.start.hex")
        self.steps, self.rng = 0, 0x9e3779b9

    def advance(self, count=1):
        for _ in range(count):
            # Matches S_STEP and RULE_TABLE indexing in sand_core.v exactly.
            self.rng = xorshift(self.rng)
            ox, oy = self.rng & 255, self.rng >> 8 & 255
            offset = self.steps & 1
            grid = np.roll(self.cells, (-offset, -offset), axis=(0, 1)) if offset else self.cells
            random_bits = self.noise[((self.coordinates + oy) & 255)[:, None],
                                     ((self.coordinates + ox) & 255)[None, :]]
            index = (grid[0::2, 0::2].astype(np.uint16)
                     | grid[0::2, 1::2].astype(np.uint16) << 2
                     | grid[1::2, 0::2].astype(np.uint16) << 4
                     | grid[1::2, 1::2].astype(np.uint16) << 6
                     | random_bits << 8)
            result = self.table[index]
            updated = np.empty_like(grid)
            updated[0::2, 0::2] = result & 3
            updated[0::2, 1::2] = result >> 2 & 3
            updated[1::2, 0::2] = result >> 4 & 3
            updated[1::2, 1::2] = result >> 6 & 3
            self.cells = np.roll(updated, (offset, offset), axis=(0, 1)) if offset else updated
            self.steps += 1


def self_test():
    table, noise = assets()
    rtl = (UPSTREAM / "rtl/accelerators/pixel_physics/sand_core.v").read_text()
    match = re.search(r"RULE_TABLE\s*=\s*4096'h([0-9a-fA-F]+)", rtl)
    if not match:
        raise AssertionError("RTL rule table not found")
    literal = int(match[1], 16)
    assert np.array_equal(table, [(literal >> (8 * i)) & 255 for i in range(512)])
    rng = 0x2545f491
    for y in range(256):
        row = 0
        for word in range(8):
            rng = xorshift(rng)
            row |= rng << (32 * word)
        assert all(int(noise[y, x]) == (row >> x & 1) for x in range(256))
    for index, output in enumerate(table):
        before = [(index >> (2 * n)) & 3 for n in range(4)]
        after = [(int(output) >> (2 * n)) & 3 for n in range(4)]
        assert sorted(before) == sorted(after), "Material lost in rule table"
        assert [a == 3 for a in before] == [a == 3 for a in after], "Wall moved"
    for name in NAMES:
        world = World(table, noise, name)
        counts = np.bincount(world.cells.ravel(), minlength=4)
        walls = world.cells == 3
        world.advance(world.expected_steps)
        expected = unpack(VECTORS / f"{name}.expected.hex")
        mismatches = int(np.count_nonzero(world.cells != expected))
        assert mismatches == 0, f"{name}: {mismatches} mismatched cells"
        assert np.array_equal(np.bincount(world.cells.ravel(), minlength=4), counts)
        assert np.array_equal(world.cells == 3, walls)
        print(f"PASS {name}: all 262144 cells match after {world.steps} steps")
    # This is an audit finding, not a claim that randomness generally never matters.
    sensitivity = int(np.count_nonzero(table[:256] != table[256:]))
    assert sensitivity == 0, "Upstream rules changed; update the audit conclusion"
    a, b = World(table, noise), World(table, noise)
    b.rng = 0x12345678
    a.advance(32)
    b.advance(32)
    assert a.rng != b.rng and np.array_equal(a.cells, b.cells)
    print("PASS rule/noise provenance, material conservation, fixed walls and seed-independent motion")
    print("AUDIT: random bit changes 0/256 block patterns; committed rule-table halves are identical")


class Viewer:
    def __init__(self, root):
        self.root = root
        self.table, self.noise = assets()
        self.world = World(self.table, self.noise)
        self.running = True
        self.name = tk.StringVar(value=NAMES[0])
        self.batch = tk.IntVar(value=4)
        self.material = tk.IntVar(value=1)
        self.radius = tk.IntVar(value=9)
        self.image = None
        root.title("Pixel Physics — exact software preview, not a hardware benchmark")
        root.configure(bg=BG)
        root.geometry("950x750")
        root.resizable(False, False)
        self.label(root, "Falling sand — the FPGA workload", 20).pack(anchor="w", padx=18, pady=(12, 3))
        self.label(root, "512×512 cells · each cell is EMPTY, SAND, WATER or WALL · local 2×2 rules, not N-body gravity", 10).pack(anchor="w", padx=18)
        body = tk.Frame(root, bg=BG)
        body.pack(fill="x", padx=18, pady=12)
        self.canvas = tk.Canvas(body, width=512, height=512, highlightthickness=1, highlightbackground="#405065")
        self.canvas.pack(side="left")
        self.canvas.bind("<Button-1>", lambda e: self.paint(e, self.material.get()))
        self.canvas.bind("<B1-Motion>", lambda e: self.paint(e, self.material.get()))
        self.canvas.bind("<Button-3>", lambda e: self.paint(e, 0))
        self.canvas.bind("<B3-Motion>", lambda e: self.paint(e, 0))
        side = tk.Frame(body, bg=BG, width=370)
        side.pack(side="left", fill="both", expand=True, padx=(18, 0))
        self.label(side, "Published starting worlds", 12).pack(anchor="w")
        combo = ttk.Combobox(side, textvariable=self.name, values=NAMES, state="readonly", width=29)
        combo.pack(fill="x", pady=7)
        combo.bind("<<ComboboxSelected>>", lambda _: self.reset())
        self.play = self.button(side, "Pause [Space]", self.toggle)
        self.button(side, "One step [N]", lambda: self.single())
        self.button(side, "Reset same world [R]", self.reset)
        self.label(side, "Steps per displayed frame (original demo: 4)", 10).pack(anchor="w", pady=(7, 0))
        tk.Scale(side, from_=1, to=32, orient="horizontal", variable=self.batch, bg=BG,
                 fg=FG, highlightthickness=0, troughcolor="#283a4b").pack(fill="x")
        self.label(side, "Paint with left mouse; right mouse erases", 10).pack(anchor="w", pady=(8, 3))
        palette = tk.Frame(side, bg=BG)
        palette.pack(fill="x")
        for material, text in enumerate(["Empty", "Sand", "Water", "Wall"]):
            tk.Radiobutton(palette, text=text, variable=self.material, value=material,
                           bg=BG, fg=FG, selectcolor="#294357", activebackground=BG,
                           activeforeground=FG).pack(side="left")
        tk.Scale(side, from_=1, to=24, orient="horizontal", label="Brush radius", variable=self.radius,
                 bg=BG, fg=FG, highlightthickness=0, troughcolor="#283a4b").pack(fill="x")
        self.button(side, "Verify scene against its golden result", self.verify_scene)
        self.button(side, "Compare two different random seeds", self.compare_seeds)
        self.label(side, "Audit finding: random bit affects 0/256\nblock patterns in the committed table.\nThe noise generator is present, but changing\nits seed does not change particle motion.", 10, "#f4c776").pack(anchor="w", pady=8)
        self.status = self.label(root, "", 10)
        self.status.pack(anchor="w", padx=18)
        self.notice = self.label(root, "This runs on your laptop CPU/NumPy. It is a faithful visualization, NOT a Jetson/FPGA speed or power measurement.", 10, "#aebfd0")
        self.notice.pack(anchor="w", padx=18, pady=(4, 0))
        self.message = self.label(root, "GPU/CPU baseline source and the team's interactive host demo are not included in the supplied repository.", 10, "#aebfd0")
        self.message.pack(anchor="w", padx=18, pady=4)
        root.bind("<space>", lambda _: self.toggle())
        root.bind("n", lambda _: self.single())
        root.bind("r", lambda _: self.reset())
        self.draw()
        root.after(16, self.frame)

    @staticmethod
    def label(parent, text, size=11, color=FG):
        return tk.Label(parent, text=text, bg=BG, fg=color, justify="left", anchor="w", font=("Segoe UI", size))

    @staticmethod
    def button(parent, text, command):
        button = tk.Button(parent, text=text, command=command, bg="#294357", fg=FG,
                           activebackground="#3c5b71", activeforeground=FG, relief="flat", pady=5)
        button.pack(fill="x", pady=3)
        return button

    def reset(self):
        self.world.load(self.name.get())
        self.message.configure(text="Loaded the upstream start vector; step counter and random state reset.")
        self.draw()

    def toggle(self):
        self.running = not self.running
        self.play.configure(text="Pause [Space]" if self.running else "Play [Space]")

    def single(self):
        if self.running:
            self.toggle()
        self.world.advance()
        self.draw()

    def paint(self, event, material):
        x, y, r = event.x, event.y, self.radius.get()
        x0, x1, y0, y1 = max(0, x-r), min(512, x+r+1), max(0, y-r), min(512, y+r+1)
        if x1 <= x0 or y1 <= y0:
            return
        yy, xx = np.ogrid[y0:y1, x0:x1]
        self.world.cells[y0:y1, x0:x1][(xx-x)**2+(yy-y)**2 <= r*r] = material
        self.draw()

    def draw(self):
        pixels = PALETTE[self.world.cells]
        self.image = tk.PhotoImage(data=b"P6\n512 512\n255\n" + pixels.tobytes(), format="PPM")
        self.canvas.delete("all")
        self.canvas.create_image(0, 0, image=self.image, anchor="nw")
        counts = np.bincount(self.world.cells.ravel(), minlength=4)
        self.status.configure(text=f"Step {self.world.steps}   |   next 2×2 partition offset ({self.world.steps&1},{self.world.steps&1})   |   sand {counts[1]:,}   water {counts[2]:,}   walls {counts[3]:,}")

    def verify_scene(self):
        self.running = False
        self.play.configure(text="Play [Space]")
        self.world.load(self.name.get())
        self.world.advance(self.world.expected_steps)
        expected = unpack(VECTORS / f"{self.world.name}.expected.hex")
        bad = int(np.count_nonzero(self.world.cells != expected))
        self.message.configure(text=f"Golden comparison after {self.world.steps} steps: {bad} mismatched cells out of 262,144.")
        self.draw()

    def compare_seeds(self):
        a, b = World(self.table, self.noise, self.name.get()), World(self.table, self.noise, self.name.get())
        b.rng = 0x12345678
        a.advance(32)
        b.advance(32)
        different = int(np.count_nonzero(a.cells != b.cells))
        self.message.configure(text=f"Different RNG seeds, same starting world, 32 steps: {different} cells differ. (This tests the committed rule table.)")

    def frame(self):
        start = time.perf_counter()
        if self.running:
            self.world.advance(self.batch.get())
            self.draw()
        self.root.after(max(1, 16 - int((time.perf_counter()-start)*1000)), self.frame)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--capture", type=Path, help="Debug screenshot; requires Pillow")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    try:
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        pass
    root = tk.Tk()
    root.tk.call("tk", "scaling", 1.15)
    viewer = Viewer(root)
    if args.capture:
        viewer.running = False
        viewer.world.advance(90)
        viewer.draw()
        root.attributes("-topmost", True)
        def capture():
            from PIL import ImageGrab
            x, y = root.winfo_rootx(), root.winfo_rooty()
            ImageGrab.grab((x, y, x+root.winfo_width(), y+root.winfo_height())).save(args.capture)
            root.destroy()
        root.after(1500, capture)
    root.mainloop()


if __name__ == "__main__":
    main()
