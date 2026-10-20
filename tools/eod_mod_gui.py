#!/usr/bin/env python3
"""EOD Recomp - Modding Suite and Vehicle Creator GUI.

Graphical tool to create custom vehicles, tune physics, customize liveries,
demux audio, export track splines/collision primitives to OBJ, and configure gameplay mods.
"""

import configparser
import math
import re
import shutil
import subprocess
import tempfile
import queue
import shlex
import os
import sys
import threading
import tkinter as tk
from tkinter import ttk, messagebox, colorchooser, filedialog
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools"))

from rst0 import unpack_archive, pack_archive
from valt import parse_binary_valt, compile_binary_valt
from tex import compile_tex, decompile_tex, write_png
from modl import parse_modl, retexture_modl, export_obj
from audio import sfx_to_wav, wav_to_sfx, extract_pss_audio
from trk import decompile_ild, export_ild_to_obj, decompile_sol, export_sol_to_obj


def edit_career_money(save_path, money, executable=None, runner=None):
    """Reuse native --save-edit on a copy; publish only a verified career save."""
    if not re.fullmatch(r"[0-9]+", str(money)) or not 0 <= int(money) <= 2147483647:
        raise ValueError("Career money must be an integer from 0 to 2147483647")
    money = int(money)
    save = Path(save_path).resolve(strict=True)
    binary = Path(executable) if executable is not None else REPO_ROOT / "build-mac/eod"
    if not binary.is_file():
        raise FileNotFoundError(f"Build the native save editor first: {binary}")
    runner = subprocess.run if runner is None else runner
    original = save.read_bytes()
    env = dict(os.environ, EOD_HEADLESS="1", EOD_MUTE="1")
    with tempfile.TemporaryDirectory(prefix=".eod-save-", dir=save.parent) as directory:
        scratch = Path(directory)
        working = scratch / "career.save"
        working.write_bytes(original)
        config = scratch / "eod.ini"
        config.write_text("")
        def inspect(*edits):
            result = runner([str(binary), "--config", str(config), "--headless", "--save-edit", str(working), *edits],
                            capture_output=True, text=True, env=env, timeout=30)
            if result.returncode:
                raise ValueError((result.stderr or result.stdout or "Native save editor failed").strip())
            if not re.search(r"\bchecksum ok\s*$", result.stdout, re.MULTILINE):
                raise ValueError("Native save editor did not verify a valid checksum")
            amount = re.search(r"^money (-?[0-9]+)\s*$", result.stdout, re.MULTILINE)
            if amount is None:
                raise ValueError("Selected save has no recognized career (Crer) section")
            return int(amount.group(1))
        inspect()
        inspect(f"money={money}")
        if inspect() != money:
            raise ValueError("Career money verification failed; original save was preserved")
        if save.read_bytes() != original:
            raise ValueError("Save changed during editing; close the game and retry")
        handle, backup_name = tempfile.mkstemp(prefix=save.name + ".backup-", dir=save.parent)
        with os.fdopen(handle, "wb") as backup:
            backup.write(original)
        shutil.copystat(save, backup_name)
        shutil.copystat(save, working)
        working.replace(save)
        return Path(backup_name)


def default_config_path(platform=None, home=None):
    """Match runtime/main.cpp's native default on macOS."""
    platform = sys.platform if platform is None else platform
    home = Path.home() if home is None else Path(home)
    if platform == "darwin":
        return home / "Library/Application Support/TDEODRecomp/eod.ini"
    return REPO_ROOT / "build-mac/eod.ini"


def save_gameplay_settings(path, settings):
    """Update recognized runtime keys without accumulating duplicate sections."""
    parser = configparser.ConfigParser(strict=False, interpolation=None, inline_comment_prefixes=(";", "#"))
    parser.optionxform = str
    path = Path(path)
    if path.exists():
        parser.read(path)
    target = next((s for s in parser.sections() if s.lower() == "gameplay"), "Gameplay")
    if not parser.has_section(target):
        parser.add_section(target)
    # Remove aliases in either accepted section: later keys must not override changes.
    aliases = {k.lower() for k in settings}
    for group in ({"invincible", "godmode"}, {"chickenfight", "chickens"}, {"turboboost", "turbo"}, {"singleplayerbattle", "spbattle"}):
        if aliases & group:
            aliases.update(group)
    for section in parser.sections():
        if section.lower() in ("gameplay", "cheats"):
            for key in list(parser[section]):
                if key.lower() in aliases:
                    parser.remove_option(section, key)
    for key, value in settings.items():
        parser.set(target, key, str(value))
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w") as handle:
        parser.write(handle)
    temporary.replace(path)


ALL_CARS = [
    "Algar", "Blitz", "Bulldog", "Carrere", "Desio", "Digit", "Hurricane", "Moth", "Palo", "Top",
    "Arrow", "Cannon", "Empire", "Guerilla", "Hunter", "Limo", "Ocelot", "Prince",
    "Coyote", "Durand", "Fillmore", "Gladiator", "Mongoose", "Orbital", "Outlaw", "Rocket", "Venom",
    "Ambulance", "Bus", "Clown", "Copcar", "Hearse", "Postal", "Taxi", "Tow"
]


class EodModdingApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Test Drive: Eve of Destruction - Modding Studio")
        self.geometry("880x720")
        self.minsize(800, 600)

        # Apply clean dark/metallic theme
        style = ttk.Style(self)
        try:
            style.theme_use("clam")
        except Exception:
            pass

        self.notebook = ttk.Notebook(self)
        self.notebook.pack(fill=tk.BOTH, expand=True, padx=8, pady=8)

        self.tab_vehicle = ttk.Frame(self.notebook)
        self.tab_gameplay = ttk.Frame(self.notebook)
        self.tab_assets = ttk.Frame(self.notebook)

        self.notebook.add(self.tab_vehicle, text="🏎️ Custom Vehicle Creator")
        self.notebook.add(self.tab_gameplay, text="⚡ Gameplay Mods & Cheats")
        self.notebook.add(self.tab_assets, text="🛠️ Tracks, Models & Audio Tools")

        self.build_events = queue.Queue()
        self.after(100, self.poll_build_events)
        self.setup_vehicle_tab()
        self.setup_gameplay_tab()
        self.setup_assets_tab()

    def setup_vehicle_tab(self):
        container = ttk.Frame(self.tab_vehicle, padding=12)
        container.pack(fill=tk.BOTH, expand=True)

        # Left Column: Identity & Specs
        left = ttk.LabelFrame(container, text=" Vehicle Identity & Performance Specs ", padding=10)
        left.grid(row=0, column=0, sticky="nsew", padx=6, pady=6)

        # Base Car
        ttk.Label(left, text="Base Chassis:").grid(row=0, column=0, sticky="w", pady=4)
        self.base_car_var = tk.StringVar(value="Algar")
        from create_custom_car import car_archive_filename, BODY_TEXTURES
        disc_cars = Path.home() / "Library/Application Support/TDEODRecomp/disc/DATA/CAR"
        choices = [car for car in ALL_CARS if car.lower() in BODY_TEXTURES and (disc_cars / car_archive_filename(car)).is_file()] if disc_cars.is_dir() else ALL_CARS
        cb_base = ttk.Combobox(left, textvariable=self.base_car_var, values=choices, state="readonly", width=16)
        cb_base.grid(row=0, column=1, sticky="w", pady=4)

        # Custom Car Name
        ttk.Label(left, text="Custom Car Name:").grid(row=1, column=0, sticky="w", pady=4)
        self.car_name_var = tk.StringVar(value="Phantom")
        ttk.Entry(left, textvariable=self.car_name_var, width=18).grid(row=1, column=1, sticky="w", pady=4)

        # Racing Number
        ttk.Label(left, text="Racing Number (#):").grid(row=2, column=0, sticky="w", pady=4)
        self.race_num_var = tk.StringVar(value="777")
        ttk.Entry(left, textvariable=self.race_num_var, width=18).grid(row=2, column=1, sticky="w", pady=4)

        # Classes
        ttk.Label(left, text="Car Classes:").grid(row=3, column=0, sticky="w", pady=4)
        cls_frame = ttk.Frame(left)
        cls_frame.grid(row=3, column=1, sticky="w", pady=4)
        self.class_var = tk.IntVar(value=1)
        for value, label in enumerate(("Compact", "Midsize", "Muscle")):
            ttk.Radiobutton(cls_frame, text=label, variable=self.class_var, value=value).pack(anchor="w")
        ttk.Label(cls_frame, text="One native class per chassis").pack(anchor="w")

        # Tuning Specs
        ttk.Separator(left, orient=tk.HORIZONTAL).grid(row=4, column=0, columnspan=2, sticky="ew", pady=8)

        ttk.Label(left, text="Engine Power (HP):").grid(row=5, column=0, sticky="w", pady=3)
        self.hp_var = tk.StringVar(value="380")
        ttk.Entry(left, textvariable=self.hp_var, width=12).grid(row=5, column=1, sticky="w", pady=3)

        ttk.Label(left, text="Engine Torque (lb-ft):").grid(row=6, column=0, sticky="w", pady=3)
        self.torque_var = tk.StringVar(value="420")
        ttk.Entry(left, textvariable=self.torque_var, width=12).grid(row=6, column=1, sticky="w", pady=3)

        ttk.Label(left, text="Engine Redline (RPM):").grid(row=7, column=0, sticky="w", pady=3)
        self.redline_var = tk.StringVar(value="9000")
        ttk.Entry(left, textvariable=self.redline_var, width=12).grid(row=7, column=1, sticky="w", pady=3)

        ttk.Label(left, text="Curb Weight (lbs):").grid(row=8, column=0, sticky="w", pady=3)
        self.mass_var = tk.StringVar(value="3200")
        ttk.Entry(left, textvariable=self.mass_var, width=12).grid(row=8, column=1, sticky="w", pady=3)

        ttk.Label(left, text="Damage Scale (lower=tougher):").grid(row=9, column=0, sticky="w", pady=3)
        self.damage_var = tk.StringVar(value="0.65")
        ttk.Entry(left, textvariable=self.damage_var, width=12).grid(row=9, column=1, sticky="w", pady=3)

        ttk.Label(left, text="Tire Grip Scale:").grid(row=10, column=0, sticky="w", pady=3)
        self.grip_var = tk.StringVar(value="0.95")
        ttk.Entry(left, textvariable=self.grip_var, width=12).grid(row=10, column=1, sticky="w", pady=3)

        # Right Column: Livery, Dealer & Output Log
        right = ttk.LabelFrame(container, text=" Livery Styling & Career Integration ", padding=10)
        right.grid(row=0, column=1, sticky="nsew", padx=6, pady=6)

        ttk.Label(right, text="Primary Body Color:").grid(row=0, column=0, sticky="w", pady=4)
        self.body_color = "#141414"
        self.btn_color = tk.Button(right, text="       ", bg=self.body_color, command=self.pick_color)
        self.btn_color.grid(row=0, column=1, sticky="w", pady=4)

        ttk.Label(right, text="Racing Stripes Style:").grid(row=1, column=0, sticky="w", pady=4)
        self.stripe_style_var = tk.StringVar(value="Cyan + Magenta Dual Stripe")
        cb_stripes = ttk.Combobox(
            right,
            textvariable=self.stripe_style_var,
            values=["Cyan + Magenta Dual Stripe", "Solid Gold Stripe", "Neon Lime Stripe", "Classic White Double", "None (Stealth)"],
            state="readonly",
            width=24,
        )
        cb_stripes.grid(row=1, column=1, sticky="w", pady=4)

        ttk.Label(right, text="Career Dealership Price ($):").grid(row=2, column=0, sticky="w", pady=4)
        self.price_var = tk.StringVar(value="1200")
        ttk.Entry(right, textvariable=self.price_var, width=14).grid(row=2, column=1, sticky="w", pady=4)

        ttk.Label(right, text="Career Description:").grid(row=3, column=0, sticky="w", pady=4)
        self.desc_var = tk.StringVar(value="Custom tuned V8 machine with reinforced rollcage and racing suspension")
        ttk.Entry(right, textvariable=self.desc_var, width=32).grid(row=3, column=1, sticky="w", pady=4)

        self.btn_build = ttk.Button(right, text="🚀 Build & Package Custom Car Mod", command=self.build_custom_car_action)
        self.btn_build.grid(row=4, column=0, columnspan=2, pady=12, sticky="ew")

        ttk.Label(right, text="Build Output Log:").grid(row=5, column=0, columnspan=2, sticky="w", pady=2)
        self.log_txt = tk.Text(right, height=12, width=42, wrap=tk.WORD, bg="#1e1e1e", fg="#e0e0e0")
        self.log_txt.grid(row=6, column=0, columnspan=2, sticky="nsew", pady=4)

        container.columnconfigure(0, weight=1)
        container.columnconfigure(1, weight=1)

    def pick_color(self):
        c = colorchooser.askcolor(self.body_color, title="Choose Body Paint Color")
        if c and c[1]:
            self.body_color = c[1]
            self.btn_color.configure(bg=self.body_color)

    def log(self, msg):
        self.log_txt.insert(tk.END, msg + "\n")
        self.log_txt.see(tk.END)
        self.update_idletasks()

    def poll_build_events(self):
        try:
            while True:
                kind, payload = self.build_events.get_nowait()
                if kind in ("success", "error"):
                    self.btn_build.config(state="normal")
                if kind == "money":
                    self.btn_money.config(state="normal")
                    messagebox.showinfo("Career Money Updated", f"Verified career money saved. Original backup: {payload}")
                elif kind == "money_error":
                    self.btn_money.config(state="normal")
                    messagebox.showerror("Career Save Error", payload)
                elif kind == "success":
                    self.log(f"Package created: {payload}")
                    self.log(f"Launch: EOD_MOD_DIR={shlex.quote(payload)} ./build-mac/eod")
                    messagebox.showinfo("Package Created", "Package built. In-game menu and driving acceptance still require validation.")
                else:
                    self.log(f"Build error: {payload}")
                    messagebox.showerror("Build Error", payload)
        except queue.Empty:
            pass
        self.after(100, self.poll_build_events)

    def build_custom_car_action(self):
        # Read Tk state on the UI thread; the worker performs only asset work.
        options = dict(name=self.car_name_var.get(), base=self.base_car_var.get(),
                       number=self.race_num_var.get(),
                       classes=[self.class_var.get()],
                       power_max=self.hp_var.get(), torque_max=self.torque_var.get(),
                       redline=self.redline_var.get(), mass=self.mass_var.get(),
                       damage_scale=self.damage_var.get(), grip=self.grip_var.get(),
                       price=self.price_var.get(), description=self.desc_var.get(),
                       body_color=self.body_color, stripe_style=self.stripe_style_var.get())
        self.btn_build.config(state="disabled")
        self.log_txt.delete("1.0", tk.END)
        self.log("Building retail chassis variant; mesh geometry is preserved.")
        def worker():
            try:
                from create_custom_car import build_custom_car
                disc_data = str(Path.home() / "Library/Application Support/TDEODRecomp/disc/DATA")
                # Builder validates the name before any output is written.
                mod_out = str(REPO_ROOT / "mods" / options["name"].lower())
                build_custom_car(disc_data, mod_out, **options)
                self.build_events.put(("success", mod_out))
            except Exception as exc:
                self.build_events.put(("error", str(exc)))
        threading.Thread(target=worker, daemon=True).start()

    def setup_gameplay_tab(self):
        container = ttk.Frame(self.tab_gameplay, padding=16)
        container.pack(fill=tk.BOTH, expand=True)

        # Career Mods
        cf = ttk.LabelFrame(container, text=" 🏆 Career Mode Modifications ", padding=12)
        cf.pack(fill=tk.X, pady=8)

        row = ttk.Frame(cf)
        row.pack(fill=tk.X, pady=4)
        ttk.Label(row, text="Career Bank Account ($):").pack(side=tk.LEFT, padx=6)
        self.career_money_var = tk.StringVar(value="9999")
        ttk.Entry(row, textvariable=self.career_money_var, width=10).pack(side=tk.LEFT, padx=6)
        self.btn_money = ttk.Button(row, text="Choose Save & Apply Money", command=self.apply_career_money)
        self.btn_money.pack(side=tk.LEFT, padx=8)

        row2 = ttk.Frame(cf)
        row2.pack(fill=tk.X, pady=4)
        self.unlock_all_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(row2, text="Unlock All Vehicles, Classes, Tracks & Dares out of the box", variable=self.unlock_all_var).pack(side=tk.LEFT)

        # Chicken Fight Weapon Mods
        wf = ttk.LabelFrame(container, text=" 🐔 Chicken Fight Weapon Enhancements ", padding=12)
        wf.pack(fill=tk.X, pady=8)

        r_chk1 = ttk.Frame(wf)
        r_chk1.pack(fill=tk.X, pady=4)
        self.rapid_fire_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(r_chk1, text="Enable explosive chickens (L2)", variable=self.rapid_fire_var).pack(side=tk.LEFT)

        r_chk2 = ttk.Frame(wf)
        r_chk2.pack(fill=tk.X, pady=4)
        self.no_cooldown_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(r_chk2, text="Zero reload delay (unsupported)", variable=self.no_cooldown_var, state="disabled").pack(side=tk.LEFT)

        r_chk3 = ttk.Frame(wf)
        r_chk3.pack(fill=tk.X, pady=4)
        ttk.Label(r_chk3, text="Explosion Blast Force Scale:").pack(side=tk.LEFT, padx=6)
        self.force_scale_var = tk.StringVar(value="2.0")
        ttk.Entry(r_chk3, textvariable=self.force_scale_var, width=8).pack(side=tk.LEFT, padx=6)

        ttk.Label(r_chk3, text="Blast Radius Scale:").pack(side=tk.LEFT, padx=6)
        self.radius_scale_var = tk.StringVar(value="2.0")
        ttk.Entry(r_chk3, textvariable=self.radius_scale_var, width=8).pack(side=tk.LEFT, padx=6)

        # Invincibility & Barriers
        tf = ttk.LabelFrame(container, text=" 🛡️ Durability & Track Collision Controls ", padding=12)
        tf.pack(fill=tk.X, pady=8)

        r_t1 = ttk.Frame(tf)
        r_t1.pack(fill=tk.X, pady=4)
        self.god_mode_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(r_t1, text="Invincibility: protect all human players (AI remain vulnerable)", variable=self.god_mode_var).pack(side=tk.LEFT)

        r_t2 = ttk.Frame(tf)
        r_t2.pack(fill=tk.X, pady=4)
        self.free_repairs_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(r_t2, text="Allow native cheats in Career", variable=self.free_repairs_var).pack(side=tk.LEFT)

        r_t3 = ttk.Frame(tf)
        r_t3.pack(fill=tk.X, pady=4)
        self.open_highway_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(r_t3, text="Remove road barriers (requires track edits; unsupported here)", variable=self.open_highway_var, state="disabled").pack(side=tk.LEFT)

        ttk.Label(container, text=f"Settings: {default_config_path()}", wraplength=760).pack(pady=4)
        ttk.Button(container, text="💾 Save Gameplay Mod Settings (eod.ini)", command=self.save_gameplay_ini).pack(pady=12)

    def apply_career_money(self):
        money = self.career_money_var.get()
        if not re.fullmatch(r"[0-9]+", money) or not 0 <= int(money) <= 2147483647:
            messagebox.showerror("Invalid Money", "Enter an integer from 0 to 2147483647.")
            return
        path = filedialog.askopenfilename(title="Select EOD career save (BASLUS-20910DRIVEN)")
        if not path:
            return
        if not messagebox.askokcancel("Close the Game Before Editing",
                f"Close Eve of Destruction before continuing so it cannot overwrite your changes. Set money to {int(money)} in the selected save? A backup will be kept."):
            return
        self.btn_money.config(state="disabled")
        def worker():
            try:
                backup = edit_career_money(path, money)
                self.build_events.put(("money", str(backup)))
            except Exception as exc:
                self.build_events.put(("money_error", str(exc)))
        threading.Thread(target=worker, daemon=True).start()

    def save_gameplay_ini(self):
        ini_path = default_config_path()
        try:
            scales = [float(self.force_scale_var.get()), float(self.radius_scale_var.get())]
            if any(not math.isfinite(v) or not 0 < v <= 100 for v in scales):
                raise ValueError("Blast scales must be finite, positive, and at most 100")
            save_gameplay_settings(ini_path, dict(
                UnlockAll=int(self.unlock_all_var.get()),
                ChickenFight=int(self.rapid_fire_var.get()),
                ChickenForceScale=scales[0], ChickenRadiusScale=scales[1],
                Invincible=int(self.god_mode_var.get()),
                CareerCheats=int(self.free_repairs_var.get())))
            messagebox.showinfo("Settings Saved", f"Updated {ini_path}. Settings take effect on the next launch using that config.")
        except Exception as exc:
            messagebox.showerror("Error", str(exc))

    def setup_assets_tab(self):
        container = ttk.Frame(self.tab_assets, padding=16)
        container.pack(fill=tk.BOTH, expand=True)

        # Audio Tools
        af = ttk.LabelFrame(container, text=" 🔊 Audio Pipeline (.sfx, .bnk, .pss) ", padding=12)
        af.pack(fill=tk.X, pady=8)

        r1 = ttk.Frame(af)
        r1.pack(fill=tk.X, pady=4)
        ttk.Button(r1, text="Decompile .sfx to WAV Audio", command=self.decompile_sfx_action).pack(side=tk.LEFT, padx=6)
        ttk.Button(r1, text="Compile WAV Audio to .sfx", command=self.compile_sfx_action).pack(side=tk.LEFT, padx=6)
        ttk.Button(r1, text="Demux 48kHz Audio from .pss Video", command=self.demux_pss_action).pack(side=tk.LEFT, padx=6)

        # Track & 3D Model Tools
        mf = ttk.LabelFrame(container, text=" 🏁 Track Splines, Collision & 3D Meshes (.mod, .ild, .sol) ", padding=12)
        mf.pack(fill=tk.X, pady=8)

        r2 = ttk.Frame(mf)
        r2.pack(fill=tk.X, pady=4)
        ttk.Button(r2, text="Export Model Bounds (.mod) to OBJ", command=self.export_mod_obj_action).pack(side=tk.LEFT, padx=6)
        ttk.Button(r2, text="Export Track Racing Line (.ild) to 3D OBJ", command=self.export_ild_obj_action).pack(side=tk.LEFT, padx=6)
        ttk.Button(r2, text="Export Track Solids (.sol) to 3D OBJ", command=self.export_sol_obj_action).pack(side=tk.LEFT, padx=6)

    def decompile_sfx_action(self):
        p = filedialog.askopenfilename(title="Select .sfx file", filetypes=[("SFX audio", "*.sfx")])
        if p:
            out = os.path.splitext(p)[0] + ".wav"
            sfx_to_wav(p, out)
            messagebox.showinfo("Export Complete", f"Exported {out}")

    def compile_sfx_action(self):
        p = filedialog.askopenfilename(title="Select .wav file", filetypes=[("WAV audio", "*.wav")])
        if p:
            out = os.path.splitext(p)[0] + ".sfx"
            wav_to_sfx(p, out)
            messagebox.showinfo("Compile Complete", f"Compiled {out}")

    def demux_pss_action(self):
        p = filedialog.askopenfilename(title="Select .pss movie file", filetypes=[("PSS Video", "*.pss")])
        if p:
            out = os.path.splitext(p)[0] + "_audio.wav"
            extract_pss_audio(p, out)
            messagebox.showinfo("Demux Complete", f"Extracted 48kHz audio to {out}")

    def export_mod_obj_action(self):
        p = filedialog.askopenfilename(title="Select .mod 3D model", filetypes=[("MODL 3D file", "*.mod")])
        if p:
            out = os.path.splitext(p)[0] + ".obj"
            export_obj(p, out)
            messagebox.showinfo("Export Complete", f"Exported model bounding box (not mesh geometry): {out}")

    def export_ild_obj_action(self):
        p = filedialog.askopenfilename(title="Select .ild racing line", filetypes=[("ILIN racing line", "*.ild")])
        if p:
            out = os.path.splitext(p)[0] + "_spline.obj"
            export_ild_to_obj(decompile_ild(p), out)
            messagebox.showinfo("Export Complete", f"Exported 3D AI Spline: {out}")

    def export_sol_obj_action(self):
        p = filedialog.askopenfilename(title="Select track.sol solid collision", filetypes=[("SOBL collision", "*.sol")])
        if p:
            out = os.path.splitext(p)[0] + "_solids.obj"
            export_sol_to_obj(decompile_sol(p), out)
            messagebox.showinfo("Export Complete", f"Exported 3D Collision Primitives: {out}")


def main():
    app = EodModdingApp()
    app.mainloop()


if __name__ == "__main__":
    main()
