"""3D-view coolant demo: ballistic jet + cylindrical lattice-gas surface films.

Run: python graphics/vulkan/tools/reactor_lab/spray3d.py
Mouse: move the nozzle; hold left to spray toward the aim marker above it.
Right drag: orbit. Wheel: zoom. 1..9: target rod. Space: pause.
R: reset. --self-test / --smoke-test / --benchmark / --render output.png.
No FPGA, PCIe, Vulkan game, GPU physics, or actual DFX is touched.
"""
import argparse
import math
from pathlib import Path
import time
import tkinter as tk
import numpy as np
from PIL import Image, ImageDraw, ImageTk
from config import HERE, DEFAULT_PATH, load_config
from model import Rules
from voxel import VoxelWorld, SpraySession, EMPTY, WATER, DEBRIS, WALL


class Camera:
    def __init__(self, world):
        v = world.cfg
        self.target = np.array([v.width*.5,v.height*.5,v.depth*.5],float)
        self.distance = max(v.width,v.height,v.depth)*1.65
        self.yaw,self.pitch = -.48,.22

    def basis(self):
        key=(self.pitch,self.yaw,self.distance,*self.target)
        if getattr(self,"_basis_key",None)==key:
            return self._basis
        p,y,d=self.pitch,self.yaw,self.distance
        self.eye=self.target+np.array([math.sin(y)*math.cos(p),-math.sin(p),math.cos(y)*math.cos(p)])*d
        forward=self.target-self.eye
        forward/=np.linalg.norm(forward)
        right=np.cross([0.,-1.,0.],forward)
        right/=np.linalg.norm(right)
        up=np.cross(forward,right)
        self._basis_key,self._basis=key,(forward,right,up)
        return self._basis

    def project(self, xyz, width, height):
        forward,right,up=self.basis()
        rel=np.atleast_2d(xyz)-self.eye
        depth=rel@forward
        focal=min(width,height)*1.35
        xy=np.column_stack((width/2+focal*(rel@right)/np.maximum(.01,depth),
                            height/2-focal*(rel@up)/np.maximum(.01,depth)))
        return xy,depth

    def ray(self, x, y, width, height):
        forward,right,up=self.basis()
        focal=min(width,height)*1.35
        ray=forward+(x-width/2)/focal*right-(y-height/2)/focal*up
        ray/=np.linalg.norm(ray)
        return self.eye.copy(),ray

    def outlet(self, x, y, width, height, world):
        eye,ray=self.ray(x,y,width,height)
        forward,_,_=self.basis()
        distance=self.distance-max(world.cells.shape)*.6
        return eye+ray*distance/max(.01,ray@forward)

    def pick(self, x, y, width, height, world):
        eye,ray=self.ray(x,y,width,height)
        # CPU UI ray picking only. Accelerator receives an integer nozzle/aim command.
        limit=self.distance+2*max(world.cells.shape)
        for distance in np.arange(0,limit,.5):
            point=eye+ray*distance
            ix,iy,iz=np.rint(point).astype(int)
            if 1 <= ix < world.cfg.width-1 and 1 <= iy < world.cfg.height-1 and 1 <= iz < world.cfg.depth-1:
                if world.node_map[iz,iy,ix]>=0:
                    return np.array([ix,iy,iz])
        # Miss: aim where the ray meets the chamber's center-depth plane.
        if abs(ray[2])>.001:
            t=(self.target[2]-self.eye[2])/ray[2]
            return self.eye+ray*max(0,t)
        return self.target


def shade(color, factor):
    return "#"+"".join(f"{min(255,max(0,round(int(color[k:k+2],16)*factor))):02x}" for k in (1,3,5))


def scene(session,camera,width,height):
    """Project real XYZ state; painter's sorting/splats are intentionally simple."""
    r,w=session.rules,session.world
    v,u=w.cfg,r.cfg.ui
    primitives=[]
    world_primitives=[]
    def polygon(points,color,depth_override=None):
        world_primitives.append(("polygon",points,color,1,depth_override))
    def line(points,color,size=1,depth_override=None):
        world_primitives.append(("line",points,color,size,depth_override))
    floor=v.height-1
    polygon([[1,floor,1],[v.width-1,floor,1],[v.width-1,floor,v.depth-1],[1,floor,v.depth-1]],"#17282c",1e9)
    for x in np.linspace(1,v.width-1,9):
        line([[x,floor,1],[x,floor,v.depth-1]],"#2b4247",depth_override=1e9-1)
    for z in np.linspace(1,v.depth-1,9):
        line([[1,floor,z],[v.width-1,floor,z]],"#2b4247",depth_override=1e9-1)
    for x in (1,v.width-1):
        for z in (1,v.depth-1):
            line([[x,1,z],[x,floor,z]],"#20393f",depth_override=1e9-1)
    camera.basis()
    angles=np.arange(9)*math.tau/8
    for rod,(cx,cz) in enumerate(w.centers):
        global_rod=w.chamber*r.rods+rod
        heat=session.reactor.heat[global_rod*r.segments:(global_rod+1)*r.segments]
        bands=min(v.render_bands,w.y1-w.y0)
        for side in range(8):
            middle=(angles[side]+angles[side+1])/2
            normal=np.array([math.cos(middle),0,math.sin(middle)])
            if normal@(camera.eye-np.array([cx,(w.y0+w.y1)/2,cz])) <= 0:
                continue
            ax,az=cx+v.rod_radius*math.cos(angles[side]),cz+v.rod_radius*math.sin(angles[side])
            bx,bz=cx+v.rod_radius*math.cos(angles[side+1]),cz+v.rod_radius*math.sin(angles[side+1])
            for band in range(bands):
                y0=w.y0+band*(w.y1-w.y0)/bands
                y1=w.y0+(band+1)*(w.y1-w.y0)/bands
                value=np.interp((band+.5)*r.segments/bands,np.arange(r.segments),heat)
                color=shade(r.heat_color(value),.68+.28*max(0,normal@np.array([-.4,0,.85])))
                polygon([[ax,y0,az],[bx,y0,bz],[bx,y1,bz],[ax,y1,az]],color)
        cap=[[cx+v.rod_radius*math.cos(a),w.y0,cz+v.rod_radius*math.sin(a)] for a in angles[:-1]]
        polygon(cap,r.rod_color(global_rod))
        # Outline the selected rod without replacing its actual heat gradient.
        if rod==session.repair.rod:
            for angle in (0,math.pi):
                x,z=cx+v.rod_radius*math.cos(angle),cz+v.rod_radius*math.sin(angle)
                line([[x,w.y0,z],[x,w.y1,z]],u.selected_color,2)
    material=(w.cells==WATER)|(w.cells==DEBRIS)
    exposed=np.zeros_like(material)
    for axis in range(3):
        a,b=[slice(None)]*3,[slice(None)]*3
        a[axis],b[axis]=slice(1,None),slice(None,-1)
        exposed[tuple(a)] |= w.cells[tuple(b)]==EMPTY
        exposed[tuple(b)] |= w.cells[tuple(a)]==EMPTY
    coords=np.argwhere(material&exposed)
    if len(coords)>v.max_drawn_particles:
        coords=coords[np.linspace(0,len(coords)-1,v.max_drawn_particles,dtype=int)]
    if len(coords):
        xyz=coords[:,[2,1,0]].astype(float)
        xy,depth=camera.project(xyz,width,height)
        for position,point,d in zip(coords,xy,depth):
            if d<=0:
                continue
            size=max(1.,min(12.,min(width,height)*1.05*v.water_render_scale/d))
            color=u.foam_color if w.cells[tuple(position)]==WATER else u.debris_color
            primitives.append((float(d),"particle",[point[0]-size/2,point[1]-size/2,point[0]+size/2,point[1]+size/2],color,1))
    film_count = 0
    if hasattr(w,"film"):
        available = max(0,v.max_drawn_particles-len(coords))
        drips = w.drips[:min(len(w.drips),available),:3]
        xyz = w.film.samples(available-len(drips))
        xyz = np.concatenate((xyz,drips))
        film_count = len(xyz)
        if film_count:
            xy,depth = camera.project(xyz,width,height)
            for point,d in zip(xy,depth):
                if d<=0: continue
                size=max(1.,min(12.,min(width,height)*1.05*v.water_render_scale/d))
                primitives.append((float(d),"particle",[point[0]-size/2,point[1]-size/2,point[0]+size/2,point[1]+size/2],u.foam_color,1))
    jet=session.jet
    if len(jet.positions):
        indices=np.linspace(0,len(jet.positions)-1,min(v.max_drawn_jet_particles,len(jet.positions)),dtype=int)
        heads=jet.positions[indices]/jet.Q
        tails=heads-jet.velocities[indices]/jet.Q*.7
        xy,depth=camera.project(np.concatenate((tails,heads)),width,height)
        count=len(heads)
        for i in range(count):
            if min(depth[i],depth[i+count])<=0: continue
            coords=[*xy[i],*xy[i+count]]
            size=max(1,round(min(4,min(width,height)*.35*v.jet_render_scale/depth[i+count])))
            primitives.append((float(depth[i+count]),"jet",coords,"#eaffff",size))
    # One vectorized projection for every mesh vertex, not one NumPy camera
    # setup per face. Rendering stays independent of the cellular hot loop.
    sizes=[len(item[1]) for item in world_primitives]
    all_xyz=np.concatenate([np.asarray(item[1]) for item in world_primitives])
    all_xy,all_depth=camera.project(all_xyz,width,height)
    offset=0
    for (kind,_,color,size,override),count in zip(world_primitives,sizes):
        xy,depth=all_xy[offset:offset+count],all_depth[offset:offset+count]
        offset+=count
        if np.all(depth>0):
            primitives.append((float(depth.mean()) if override is None else override,kind,xy.ravel().tolist(),color,size))
    return sorted(primitives,key=lambda item:item[0],reverse=True),len(coords)+film_count


def rasterize(primitives,width,height,background,water_opacity,jet_opacity):
    """Depth-sorted real alpha blending, then ONE Tk image instead of 1000 items.

    Drawing RGBA onto an RGB image blends against the already-rendered rods;
    merely tinting toward the background would not make rods visible through it.
    """
    picture=Image.new("RGB",(width,height),background)
    draw=ImageDraw.Draw(picture,"RGBA")
    for _,kind,coords,color,size in primitives:
        if kind in ("particle","jet"):
            opacity=water_opacity if kind=="particle" else jet_opacity
            color=tuple(int(color[k:k+2],16) for k in (1,3,5))+(round(opacity*255),)
        if kind=="polygon": draw.polygon(list(zip(coords[::2],coords[1::2])),fill=color)
        elif kind in ("line","jet"): draw.line(list(zip(coords[::2],coords[1::2])),fill=color,width=size)
        else: draw.ellipse(coords,fill=color)
    return picture


def save_preview(session,camera,path,width=1100,height=740):
    # Render our OWN scene directly: never screenshot somebody else's desktop.
    v=session.world.cfg
    primitives,_=scene(session,camera,width,height)
    rasterize(primitives,width,height,session.rules.cfg.ui.background_color,
              v.water_render_opacity,v.jet_render_opacity).save(path)


class SprayApp:
    def __init__(self,root,rules,chamber=0,rod=1):
        self.root,self.rules,self.chamber,self.rod=root,rules,chamber,rod
        u=rules.cfg.ui
        root.title("COOLANT / 3D | "+("lattice-gas surface film" if rules.cfg.lattice.enabled else "legacy voxel runoff"))
        root.geometry(f"{min(1400,root.winfo_screenwidth()-70)}x{min(900,root.winfo_screenheight()-100)}+20+30")
        root.configure(bg=u.background_color)
        top=tk.Frame(root,bg=u.background_color)
        top.pack(fill="x",padx=16,pady=12)
        tk.Label(top,text="COOLANT / 3D",font=("Consolas",22),bg=u.background_color,fg=u.linked_color).pack(side="left")
        tk.Label(top,text="PARTICLES ON / REACTOR FORECAST OFF",font=("Consolas",11),bg=u.background_color,fg=u.accent_color).pack(side="right")
        body=tk.Frame(root,bg=u.background_color)
        body.pack(fill="both",expand=True,padx=16,pady=(0,16))
        self.canvas=tk.Canvas(body,bg=u.background_color,highlightthickness=0)
        self.canvas.pack(side="left",fill="both",expand=True)
        side=tk.Frame(body,bg=u.panel_color,width=320)
        side.pack(side="right",fill="y",padx=(12,0))
        side.pack_propagate(False)
        self.info=tk.Label(side,bg=u.panel_color,fg=u.text_color,font=("Consolas",10),anchor="nw",justify="left",wraplength=295)
        self.info.pack(fill="x",padx=12,pady=12)
        for label,command in (("RESET [R]",self.reset),("PAUSE [Space]",self.toggle),("RE-CENTER CAMERA [C]",self.recenter)):
            tk.Button(side,text=label,command=command,bg="#28484c",fg=u.text_color,relief="flat",padx=10,pady=8).pack(fill="x",padx=12,pady=5)
        self.canvas.bind("<Button-1>",self.press)
        self.canvas.bind("<Configure>",lambda e:setattr(self,"dirty",True))
        self.canvas.bind("<Motion>",self.motion)
        self.canvas.bind("<B1-Motion>",self.motion)
        self.canvas.bind("<ButtonRelease-1>",lambda e:self.release())
        self.canvas.bind("<Leave>",lambda e:self.release())
        self.canvas.bind("<Button-3>",lambda e:setattr(self,"orbit_start",(e.x,e.y)))
        self.canvas.bind("<B3-Motion>",self.orbit)
        self.canvas.bind("<MouseWheel>",self.zoom)
        self.canvas.bind("<Button-4>",lambda e:self.zoom(e,1))
        self.canvas.bind("<Button-5>",lambda e:self.zoom(e,-1))
        root.bind("<KeyPress>",self.key)
        root.bind("<FocusOut>",lambda e:self.release())
        root.bind("<ButtonRelease-1>",lambda e:self.release())
        root.protocol("WM_DELETE_WINDOW",self.close)
        self.closed=False
        self.reset()
        root.after(rules.cfg.voxel.frame_milliseconds,self.frame)

    def reset(self):
        self.session=SpraySession(self.rules,self.chamber,self.rod)
        # A local demo starts with one clearly overheating rod. This edits only
        # this session, not the panel's saved settings or an ongoing game.
        r=self.rules
        start=r.node(self.chamber,self.rod,0)
        profile=self.session.reactor.heat[start:start+r.segments]
        x=np.linspace(-1,1,r.segments)
        self.session.reactor.heat[start:start+r.segments]=np.maximum(profile,np.rint(12+16*np.exp(-x*x/.12))).astype(np.uint8)
        self.session.reactor.fault[start:start+r.segments] |= np.where(
            self.session.reactor.heat[start:start+r.segments]>=r.cfg.thermal.surge_heat,2,0).astype(np.uint8)
        self.session.select(self.rod)
        self.camera=Camera(self.session.world)
        self.cursor=None
        self.paused=self.spraying=False
        self.last=time.perf_counter()
        self.accumulator=0.
        self.render_ms=0.
        self.draw()

    def release(self):
        self.spraying=False

    def toggle(self):
        self.paused=not self.paused
        self.release()
        self.dirty=True

    def recenter(self):
        self.camera=Camera(self.session.world)
        if self.cursor is not None: self.point_nozzle(*self.cursor)
        self.draw()

    def press(self,event):
        self.motion(event)
        self.spraying=not self.paused

    def motion(self,event):
        self.point_nozzle(event.x,event.y)

    def point_nozzle(self,x,y):
        self.cursor=(x,y)
        self.dirty=True
        width,height=max(1,self.canvas.winfo_width()),max(1,self.canvas.winfo_height())
        world=self.session.world
        origin=self.camera.outlet(x,y,width,height,world)
        aim=self.camera.pick(x,y-world.cfg.nozzle_aim_offset_pixels,width,height,world)
        self.session.jet.set_stream(origin,aim)

    def orbit(self,event):
        x,y=self.orbit_start
        self.camera.yaw+=(event.x-x)*.008
        self.camera.pitch=float(np.clip(self.camera.pitch+(event.y-y)*.005,.03,.85))
        self.orbit_start=(event.x,event.y)
        self.release()
        self.point_nozzle(event.x,event.y)

    def zoom(self,event,direction=None):
        direction=(1 if event.delta>0 else -1) if direction is None else direction
        scale=max(self.session.world.cells.shape)
        self.camera.distance=float(np.clip(self.camera.distance*(.9 if direction>0 else 1.1),scale*.8,scale*3))
        if self.cursor is not None: self.point_nozzle(*self.cursor)

    def key(self,event):
        key=event.keysym.lower()
        if key=="space": self.toggle()
        elif key=="r": self.reset()
        elif key=="c": self.recenter()
        elif key.isdigit() and 1 <= int(key) <= self.rules.rods:
            self.session.select(int(key)-1)
            if self.cursor is not None: self.point_nozzle(*self.cursor)

    def frame(self):
        if self.closed: return
        now=time.perf_counter()
        dt,self.last=now-self.last,now
        if not self.paused and not self.session.repair.lost:
            v=self.rules.cfg.voxel
            step=1/v.steps_per_second
            self.accumulator=min(self.accumulator+dt,v.max_simulation_steps_per_frame*step)
            for _ in range(v.max_simulation_steps_per_frame):
                if self.accumulator<step: break
                self.accumulator-=step
                self.session.step(self.spraying)
            self.dirty=True
        if self.dirty:
            self.draw()
        self.root.after(self.rules.cfg.voxel.frame_milliseconds,self.frame)

    def draw(self):
        started=time.perf_counter()
        cv,s=self.canvas,self.session
        cv.delete("all")
        width,height=max(1,cv.winfo_width()),max(1,cv.winfo_height())
        if self.cursor is None and width>100 and height>100:
            w=self.session.world
            rod=self.session.repair.rod
            x,z=w.centers[rod]
            start=self.rules.node(w.chamber,rod,0)
            profile=self.session.reactor.heat[start:start+self.rules.segments]
            y=w.y0+(profile.argmax()+.5)*(w.y1-w.y0)/self.rules.segments
            point,_=self.camera.project([[x,y,z+w.cfg.rod_radius]],width,height)
            px,py=point[0]
            self.point_nozzle(float(np.clip(px,20,width-20)),float(np.clip(py+w.cfg.nozzle_aim_offset_pixels,30,height-20)))
        primitives,drawn=scene(s,self.camera,width,height)
        v=s.world.cfg
        picture=rasterize(primitives,width,height,self.rules.cfg.ui.background_color,
                          v.water_render_opacity,v.jet_render_opacity)
        self.frame_photo=ImageTk.PhotoImage(picture,master=self.root)
        kinds={item[1] for item in primitives}
        tags=("scene_image",)+(("particle",) if "particle" in kinds else ())+(("jet_packet",) if "jet" in kinds else ())
        cv.create_image(0,0,anchor="nw",image=self.frame_photo,tags=tags)
        if self.cursor is not None:
            x,y=self.cursor
            aim_y=y-self.session.world.cfg.nozzle_aim_offset_pixels
            cv.create_line(x-7,aim_y,x+7,aim_y,fill="#75bfc4",tags=("aim_marker",))
            cv.create_line(x,aim_y-7,x,aim_y+7,fill="#75bfc4",tags=("aim_marker",))
            cv.create_oval(x-11,y-11,x+11,y+11,fill="#1d424d",outline="#d9f7ff",width=2,tags=("nozzle_cursor",))
            cv.create_rectangle(x-5,y-18,x+5,y-4,fill="#b9dbdf",outline="#ffffff",tags=("nozzle_cursor",))
        w,r,p=s.world,s.rules,s.repair
        heat=s.reactor.heat.reshape(r.total_rods,r.segments)[w.chamber*r.rods+p.rod]
        status="TARGET COOLED" if p.won else "TIME EXPIRED [R]" if p.lost else "SPRAYING" if self.spraying else "AIM / HOLD LEFT MOUSE"
        cv.create_text(18,22,text=status,fill=r.cfg.ui.accent_color,anchor="w",font=("Consolas",14,"bold"))
        self.render_ms=(time.perf_counter()-started)*1000
        self.info.configure(text=f"REACTOR {w.chamber+1} / ROD {p.rod+1}\nHottest: {heat.max()}/{r.cfg.thermal.critical_heat}\n"
            f"Safe target: <= {r.cfg.thermal.safe_heat}\nWet contact: {p.contact_seconds:.1f}/{r.cfg.timing.treatment_seconds:g}s\n"
            f"Repair fuse: {p.remaining:.0f}s\n\n"
            "Mouse: move hose outlet\nHold left: high-pressure jet\nAim marker ABOVE the nozzle\nRight drag: orbit in 3D\nWheel: zoom\n1..9: select another target\n\n"
            f"WORLD: {w.cfg.width}x{w.cfg.height}x{w.cfg.depth}\n"
            +(f"FILM: {s.rules.cfg.lattice.film_rows} x {s.rules.cfg.lattice.film_columns} / rod\n{w.water_particles:,} moving fluid bits\n" if hasattr(w,"film") else f"{np.count_nonzero(w.cells==WATER):,} coolant voxels\n")+
            f"Fluid snapshot: {w.packed_bytes/1024:.1f} KiB\nImpact command: 4 bytes + reply bit\n"
            f"In-flight packets: {len(s.jet.positions):,}\nJet speed: {w.cfg.jet_speed_voxels_per_second:g} voxels/s\n"
            f"Physics step: {s.simulation_ms:.2f} ms\nSoftware draw: {self.render_ms:.2f} ms\nDrawing {drawn:,} surface samples\n\n"
            "CPU/NumPy prototype.\nNo GPU/FPGA power claim.\nNo neutron forecast runs here.\nFlight / decorative drips: host.\n"
            +("After impact: 2D films on 3D rods." if hasattr(w,"film") else "After impact: legacy voxel runoff.")
            +( "\nPAUSED" if self.paused else ""))
        self.dirty=False

    def close(self):
        self.closed=True
        self.root.destroy()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config",type=Path,default=DEFAULT_PATH)
    parser.add_argument("--chamber",type=int,default=1)
    parser.add_argument("--rod",type=int,default=None)
    parser.add_argument("--self-test",action="store_true")
    parser.add_argument("--smoke-test",action="store_true")
    parser.add_argument("--benchmark",action="store_true")
    parser.add_argument("--render",type=Path)
    args=parser.parse_args()
    if args.self_test:
        import unittest,test_voxel
        result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromModule(test_voxel))
        raise SystemExit(not result.wasSuccessful())
    if not args.config.is_absolute() and not args.config.exists(): args.config=HERE/args.config
    rules=Rules(load_config(args.config))
    rod=min(1,rules.rods-1) if args.rod is None else args.rod-1
    if not 0 <= args.chamber-1 < rules.chambers or not 0 <= rod < rules.rods:
        parser.error("--chamber/--rod must be within the configured 1-based range")
    if args.benchmark:
        from test_voxel import benchmark
        benchmark(rules)
        return
    if args.render:
        s=SpraySession(rules,args.chamber-1,rod)
        for _ in range(150): s.step(True)
        save_preview(s,Camera(s.world),args.render)
        print(f"Rendered own 3D scene: {args.render}")
        return
    import sys
    if sys.platform=="win32":
        import ctypes
        try: ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except (AttributeError,OSError): pass
    root=tk.Tk()
    app=SprayApp(root,rules,args.chamber-1,rod)
    if args.smoke_test:
        from test_voxel import gui_smoke
        try: gui_smoke(app)
        finally: app.close()
        return
    root.mainloop()


if __name__=="__main__": main()
