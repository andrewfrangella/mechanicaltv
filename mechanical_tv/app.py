"""Mechanical TV local desktop app: the operator console running on the Pi itself.

Everything happens in one process. The Tk window talks to the Studio engine (frame pipeline,
realtime controller, library) through plain method calls: no HTTP server, no browser, no
polling over a socket and no login. Control actions run on a worker thread so a slow
hardware call (stopping the motor, re-opening the capture device) never freezes the window.

    python3 -m mechanical_tv.app                       # window, hardware if present
    python3 -m mechanical_tv.app --fullscreen          # appliance / kiosk
    python3 -m mechanical_tv.app --mock-hardware       # development without a Pi
    python3 -m mechanical_tv.app --check               # no window: report what would run

Only the Python standard library (tkinter) is needed: frames are pushed to Tk as PGM
images and scaled with integer zoom, so the 32 x 25 picture keeps its crisp pixel look.
"""
import argparse
import json
import logging
import os
from pathlib import Path
import queue
import shutil
import sys
import threading
import time

# lgpio (behind RPi.GPIO on Pi OS Bookworm/Trixie) writes helper files to its working directory,
# which is read-only under /opt. Set before any hardware module is imported.
os.environ.setdefault("LG_WD", "/tmp")

from . import __version__  # noqa: E402
from .capture import WIDTH, HEIGHT, FRAME_PIXELS, HDMICapture  # noqa: E402
from .hardware.controller import MAX_FPS  # noqa: E402
from .logs import LOGGER, setup_logging, log_exception  # noqa: E402
from .studio import Studio, load_env_file  # noqa: E402

DEFAULT_DATA = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share") / "mechanical-tv"

# Same palette as the web studio so both interfaces read as one product.
C = {
    "bg": "#101312", "panel": "#181d1b", "panel_alt": "#1f2522", "border": "#323d35",
    "text": "#e7eddf", "muted": "#99a898", "accent": "#d8ef87", "ink": "#1e2612",
    "danger": "#ff6b6b", "good": "#69db7c", "warn": "#ffd43b",
}

SOURCES = [
    ("hdmi", "Live HDMI Video (capture adapter)"),
    ("smpte_bars", "Test Pattern: SMPTE Grayscale Bars"),
    ("grayscale_ramp", "Test Pattern: Horizontal Gray Ramp"),
    ("vertical_ramp", "Test Pattern: Vertical Gray Ramp"),
    ("grid", "Test Pattern: Aperture Grid & Crosshair"),
    ("rotating_bar", "Test Pattern: Rotating Phase Sync Bar"),
    ("pulse_test", "Test Pattern: 1 Hz Photodiode Pulse Test"),
    ("checkerboard", "Test Pattern: 4x4 Checkerboard"),
    ("single_column", "Test Pattern: Sweeping Column"),
    ("library", "Selected Library Clip"),
]
SOURCE_BY_LABEL = {label: key for key, label in SOURCES}
SOURCE_LABEL = {key: label for key, label in SOURCES}

from .v4l2 import PREVIEW_H, PREVIEW_W, available as native_available

PREVIEW_HZ = 10        # preview and telemetry refresh rate
SRC_LABEL_FRAME = "SOURCE VIDEO · 32 × 25 GRAYSCALE"
SRC_LABEL_LIVE = "HDMI INPUT · LIVE PICTURE"
HUD_WIDTH = 26         # characters per telemetry value
LOG_LINES = 2000       # lines kept in the debug window
LIBRARY_REFRESH_S = 2  # library list refresh interval


def pgm(frame: bytes) -> bytes:
    """Binary PGM (P5) image of one 32 x 25 frame, the format Tk's PhotoImage reads natively."""
    if len(frame) != FRAME_PIXELS:
        frame = bytes(FRAME_PIXELS)
    return b"P5 %d %d 255\n" % (WIDTH, HEIGHT) + frame


def ppm(rgb: bytes) -> bytes:
    """Binary PPM (P6) image of one PREVIEW_W x PREVIEW_H RGB picture, also read natively by Tk."""
    return b"P6 %d %d 255\n" % (PREVIEW_W, PREVIEW_H) + rgb


def check_report(args) -> dict:
    """What `--check` prints: the environment the app would run in, without opening a window."""
    probe = HDMICapture(device=args.hdmi_device, fit=args.hdmi_fit, input_format=args.hdmi_format,
                        edid=args.hdmi_edid, backend=args.hdmi_backend, bridge=args.hdmi_bridge)
    tk_version = None
    tk_error = ""
    try:
        import tkinter
        tk_version = str(tkinter.TkVersion)
    except Exception as exc:  # python3-tk missing
        tk_error = f"{type(exc).__name__}: {exc}"
    hdmi_present = os.path.exists(args.hdmi_device)
    return {
        "version": __version__,
        "python": sys.version.split()[0],
        "tk": tk_version,
        "tk_error": tk_error,
        "display": os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY") or "",
        "data": str(args.data),
        "ffmpeg": shutil.which("ffmpeg"),
        "v4l2_ctl": shutil.which("v4l2-ctl"),
        "motor_driver": os.environ.get("MTV_MOTOR_DRIVER", "hat"),
        "hardware_pwm": bool(sorted(Path("/sys/class/pwm").glob("pwmchip*"))) if Path("/sys/class/pwm").exists() else False,
        "gpiochip": os.path.exists("/dev/gpiochip0"),
        "i2c": os.path.exists("/dev/i2c-1"),
        "hdmi_device": args.hdmi_device,
        "hdmi_device_exists": hdmi_present,
        "hdmi_bridge": probe.bridge if hdmi_present else "device missing",
        "hdmi_input_format": probe.input_format,
        "hdmi_backend": probe.backend,
        "native_v4l2": native_available()[0],
        "native_v4l2_note": native_available()[1],
        "hdmi_edid": probe.edid_path or None,
        "default_source": resolve_default_source(args),
        "mock_hardware": bool(args.mock_hardware),
        "fullscreen": bool(args.fullscreen),
    }


def resolve_default_source(args) -> str:
    if args.default_source != "auto":
        return args.default_source
    # Capture is never mocked, so a simulation run must not grab a real device another process may own.
    return "hdmi" if os.path.exists(args.hdmi_device) and not args.mock_hardware else "pattern"


def usable_data_dir(requested, log=print) -> str:
    """The requested data directory if this user can write it, else the per-user default.

    /etc/mechanical-tv/environment points MTV_DATA at the web service's private directory
    (/var/lib/mechanical-tv, owned by the service user); the desktop user cannot use it.
    """
    path = Path(requested)
    try:
        path.mkdir(parents=True, exist_ok=True)
        probe = path / ".write-test"
        probe.touch()
        probe.unlink()
        return str(path)
    except OSError as exc:
        fallback = DEFAULT_DATA
        if Path(requested) != fallback:
            log(f"Data directory {requested} is not writable ({exc.strerror or exc}); using {fallback}", flush=True)
        fallback.mkdir(parents=True, exist_ok=True)
        return str(fallback)


def build_studio(args, log=print):
    """Create the engine; if real hardware fails to initialise, report it and fall back to simulation."""
    options = dict(
        hdmi_device=args.hdmi_device,
        hdmi_options={
            "hdmi_bridge": args.hdmi_bridge, "hdmi_format": args.hdmi_format, "hdmi_edid": args.hdmi_edid,
            "hdmi_fit": args.hdmi_fit, "hdmi_backend": args.hdmi_backend,
        },
        motor_port=args.motor_port, opto_pin=args.opto_pin, led_pin=args.led_pin,
        use_spi_led=args.spi_led, default_source=resolve_default_source(args),
    )
    args.data = usable_data_dir(args.data, log)
    if args.mock_hardware:
        return Studio(args.data, force_mock=True, **options), ""
    try:
        return Studio(args.data, force_mock=False, **options), ""
    except Exception as exc:
        reason = f"{type(exc).__name__}: {exc}"
        log(f"Hardware initialisation failed ({reason}); running in simulation", flush=True)
        return Studio(args.data, force_mock=True, **options), reason


class Console:
    """The Tk operator console. All Tk calls happen on the main thread."""

    def __init__(self, root, studio: Studio, fullscreen: bool = False, hardware_note: str = "",
                 log_queue=None, log_path=None):
        import tkinter as tk
        from tkinter import ttk
        self.tk, self.ttk = tk, ttk
        self.root = root
        self.studio = studio
        self.log_queue = log_queue if log_queue is not None else queue.Queue()
        self.log_path = log_path
        self.error_count = 0
        self.realtime = studio.realtime
        self.pipeline = studio.pipeline
        self.hardware_note = hardware_note
        self.notices: "queue.Queue[str]" = queue.Queue()
        self.jobs: "queue.Queue" = queue.Queue()
        self.worker = threading.Thread(target=self._job_loop, daemon=True, name="console-actions")
        self.worker.start()
        self.selected_clip = None
        self.closing = False
        self._last_library = None
        self._last_library_at = 0.0
        self._zoom = 1
        self._pzoom = 1                      # integer zoom of the live-picture preview
        self._canvas_size = (1, 1)
        self._src_shown = None               # (w, h) currently placed in the left pane
        self._scale_busy = {}                # scale -> inf while pressed, else time of the last release
        self._source_changed_at = 0.0
        self._free_run_changed_at = 0.0
        self._fullscreen = False

        root.title(f"Mechanical TV {__version__}")
        root.configure(bg=C["bg"])
        root.minsize(1100, 700)
        root.protocol("WM_DELETE_WINDOW", self.quit)
        root.bind("<F11>", lambda e: self.set_fullscreen(not self._fullscreen))
        root.bind("<Escape>", lambda e: self.set_fullscreen(False))
        root.bind("<Control-q>", lambda e: self.quit())
        root.bind("<F12>", lambda e: self.debug.toggle())
        root.bind("<space>", self._space)
        root.report_callback_exception = self._tk_exception
        self._style()
        self._build()
        self.debug = DebugWindow(self, log_path)
        self.set_fullscreen(fullscreen)
        self._sync_calibration_widgets(self.pipeline)
        self.notice(f"Mechanical TV {__version__} · " + ("REALTIME HARDWARE" if studio.hardware_enabled else "SIMULATION"))
        if hardware_note:
            self.notice(f"Hardware unavailable, running in simulation: {hardware_note}")
        root.after(100, self._tick)

    # -- styling -------------------------------------------------------------

    def _style(self):
        ttk = self.ttk
        s = ttk.Style(self.root)
        s.theme_use("clam")
        base = ("DejaVu Sans", 11)
        self.font_base = base
        self.font_small = ("DejaVu Sans", 9)
        self.font_mono = ("DejaVu Sans Mono", 9)
        self.font_title = ("DejaVu Sans", 22, "bold")
        self.font_value = ("DejaVu Sans Mono", 13, "bold")
        s.configure(".", background=C["bg"], foreground=C["text"], font=base, borderwidth=0)
        s.configure("TFrame", background=C["bg"])
        s.configure("Panel.TFrame", background=C["panel"])
        s.configure("TLabel", background=C["bg"], foreground=C["text"])
        s.configure("Panel.TLabel", background=C["panel"], foreground=C["text"])
        s.configure("Muted.TLabel", background=C["panel"], foreground=C["muted"], font=self.font_mono)
        s.configure("Eyebrow.TLabel", background=C["bg"], foreground=C["muted"], font=self.font_mono)
        s.configure("Title.TLabel", background=C["bg"], foreground=C["text"], font=self.font_title)
        s.configure("Heading.TLabel", background=C["panel"], foreground=C["accent"], font=("DejaVu Sans", 12, "bold"))
        s.configure("Value.TLabel", background=C["panel"], foreground=C["text"], font=self.font_value)
        s.configure("Notice.TLabel", background=C["bg"], foreground=C["accent"], font=self.font_small)
        s.configure("TButton", background=C["panel_alt"], foreground=C["text"], bordercolor=C["border"],
                    lightcolor=C["panel_alt"], darkcolor=C["panel_alt"], padding=(14, 8), relief="flat")
        s.map("TButton", background=[("active", C["border"]), ("disabled", C["panel"])],
              foreground=[("disabled", C["muted"])])
        s.configure("Primary.TButton", background=C["accent"], foreground=C["ink"], lightcolor=C["accent"],
                    darkcolor=C["accent"], bordercolor=C["accent"], font=("DejaVu Sans", 11, "bold"))
        s.map("Primary.TButton", background=[("active", "#e9f7a8"), ("disabled", C["panel"])],
              foreground=[("disabled", C["muted"])])
        s.configure("Danger.TButton", background=C["panel_alt"], foreground=C["danger"], bordercolor=C["danger"])
        s.map("Danger.TButton", background=[("active", "#3a2222")])
        s.configure("TCheckbutton", background=C["panel"], foreground=C["text"], indicatorcolor=C["panel_alt"],
                    indicatorbackground=C["panel_alt"])
        s.map("TCheckbutton", background=[("active", C["panel"])], indicatorcolor=[("selected", C["accent"])],
              foreground=[("disabled", C["muted"])])
        s.configure("TCombobox", fieldbackground=C["panel_alt"], background=C["panel_alt"], foreground=C["text"],
                    arrowcolor=C["accent"], bordercolor=C["border"], lightcolor=C["panel_alt"], darkcolor=C["panel_alt"],
                    selectbackground=C["panel_alt"], selectforeground=C["text"], padding=6)
        s.map("TCombobox", fieldbackground=[("readonly", C["panel_alt"])], foreground=[("readonly", C["text"])])
        self.root.option_add("*TCombobox*Listbox.background", C["panel_alt"])
        self.root.option_add("*TCombobox*Listbox.foreground", C["text"])
        self.root.option_add("*TCombobox*Listbox.selectBackground", C["accent"])
        self.root.option_add("*TCombobox*Listbox.selectForeground", C["ink"])
        s.configure("Treeview", background=C["panel_alt"], fieldbackground=C["panel_alt"], foreground=C["text"],
                    bordercolor=C["border"], rowheight=26)
        s.configure("Treeview.Heading", background=C["panel"], foreground=C["muted"], font=self.font_mono, relief="flat")
        s.map("Treeview", background=[("selected", C["accent"])], foreground=[("selected", C["ink"])])
        s.configure("Vertical.TScrollbar", background=C["panel_alt"], troughcolor=C["panel"], arrowcolor=C["muted"],
                    bordercolor=C["panel"])

    def _scale(self, parent, from_, to, resolution, command):
        """A classic Tk scale (ttk's has no resolution) in the dark theme; `command` runs on release."""
        tk = self.tk
        w = tk.Scale(parent, from_=from_, to=to, resolution=resolution, orient="horizontal", showvalue=False,
                     bg=C["panel"], fg=C["text"], troughcolor=C["panel_alt"], activebackground=C["accent"],
                     highlightthickness=0, bd=0, sliderrelief="flat", sliderlength=22, length=220)
        # Tk scales never take the keyboard focus on a click, so "is the operator holding this
        # slider" must be tracked from the mouse buttons; the refresh tick leaves a busy slider alone.
        busy = self._scale_busy

        def pressed(event):
            busy[w] = float("inf")

        def released(event):
            busy[w] = time.monotonic()
            command(w.get())

        w.bind("<ButtonPress-1>", pressed)
        w.bind("<ButtonRelease-1>", released)
        w.bind("<KeyRelease>", released)
        return w

    def _pill(self, parent):
        tk = self.tk
        return tk.Label(parent, text="", font=self.font_mono, bg=C["bg"], fg=C["muted"], padx=12, pady=4,
                        highlightthickness=1, highlightbackground=C["border"])

    @staticmethod
    def _pill_color(pill, state):
        colors = {"locked": C["good"], "free-run": C["warn"], "ramping": C["warn"], "running": C["warn"],
                  "idle": C["muted"], "live": C["good"], "danger": C["danger"]}
        color = colors.get(state, C["muted"])
        pill.configure(fg=color, highlightbackground=color)

    # -- layout --------------------------------------------------------------

    def _build(self):
        tk, ttk = self.tk, self.ttk
        root = self.root
        root.columnconfigure(0, weight=3)
        root.columnconfigure(1, weight=2)
        root.rowconfigure(1, weight=1)

        # Header
        header = ttk.Frame(root, padding=(24, 16, 24, 8))
        header.grid(row=0, column=0, columnspan=2, sticky="ew")
        header.columnconfigure(1, weight=1)
        titles = ttk.Frame(header)
        titles.grid(row=0, column=0, sticky="w")
        ttk.Label(titles, text="EXPERIMENTAL MECHANICAL TELEVISION / 001", style="Eyebrow.TLabel").pack(anchor="w")
        ttk.Label(titles, text="Mechanical TV", style="Title.TLabel").pack(anchor="w")
        pills = ttk.Frame(header)
        pills.grid(row=0, column=2, sticky="e")
        self.pill_hw = self._pill(pills)
        self.pill_motor = self._pill(pills)
        self.pill_sync = self._pill(pills)
        self.pill_hdmi = self._pill(pills)
        self.pill_log = self._pill(pills)
        self.pill_log.configure(text="LOG", cursor="hand2")
        self.pill_log.bind("<Button-1>", lambda e: self.debug.toggle())
        for p in (self.pill_hw, self.pill_motor, self.pill_sync, self.pill_hdmi, self.pill_log):
            p.pack(side="left", padx=(0, 8))
        ttk.Button(pills, text="Debug log (F12)", command=lambda: self.debug.toggle()).pack(side="left", padx=(8, 0))
        ttk.Button(pills, text="Full screen (F11)", command=lambda: self.set_fullscreen(not self._fullscreen)).pack(side="left", padx=(8, 0))
        ttk.Button(pills, text="Quit", command=self.quit).pack(side="left", padx=(8, 0))

        # Left: previews + source
        left = ttk.Frame(root, padding=(24, 8, 12, 8))
        left.grid(row=1, column=0, sticky="nsew")
        left.columnconfigure(0, weight=1)
        left.columnconfigure(1, weight=1)
        left.rowconfigure(2, weight=1)

        src_row = ttk.Frame(left, style="Panel.TFrame", padding=12)
        src_row.grid(row=0, column=0, columnspan=2, sticky="ew", pady=(0, 12))
        src_row.columnconfigure(1, weight=1)
        ttk.Label(src_row, text="Video input", style="Heading.TLabel").grid(row=0, column=0, padx=(0, 12))
        self.source_var = tk.StringVar(value=SOURCE_LABEL["smpte_bars"])
        self.source_box = ttk.Combobox(src_row, textvariable=self.source_var, values=[l for _, l in SOURCES],
                                       state="readonly", width=44)
        self.source_box.grid(row=0, column=1, sticky="w")
        self.source_box.bind("<<ComboboxSelected>>", self._source_selected)
        self.source_status = ttk.Label(src_row, text="", style="Muted.TLabel")
        self.source_status.grid(row=0, column=2, sticky="e", padx=(12, 0))
        self.source_hint = ttk.Label(src_row, text="", style="Muted.TLabel", wraplength=760, justify="left")
        self.source_hint.grid(row=1, column=0, columnspan=3, sticky="w", pady=(8, 0))

        self.label_src = ttk.Label(left, text=SRC_LABEL_FRAME, style="Eyebrow.TLabel")
        self.label_src.grid(row=1, column=0, sticky="w")
        ttk.Label(left, text="MECHANICAL TV IMAGE · AS SENT TO THE DISK", style="Eyebrow.TLabel").grid(row=1, column=1, sticky="w", padx=(12, 0))
        self.canvas_src = tk.Canvas(left, bg="#000000", highlightthickness=1, highlightbackground=C["border"])
        self.canvas_src.grid(row=2, column=0, sticky="nsew", pady=(6, 0))
        self.canvas_out = tk.Canvas(left, bg="#000000", highlightthickness=1, highlightbackground=C["border"])
        self.canvas_out.grid(row=2, column=1, sticky="nsew", pady=(6, 0), padx=(12, 0))
        self.canvas_src.bind("<Configure>", self._canvas_resized)
        self.img_src_small = tk.PhotoImage(width=WIDTH, height=HEIGHT)
        self.img_preview_small = tk.PhotoImage(width=PREVIEW_W, height=PREVIEW_H)
        self._showing_preview = False
        self._painted = (None, None, 0)   # (frame, preview, zoom) last encoded into the left pane
        self.img_out_small = tk.PhotoImage(width=WIDTH, height=HEIGHT)
        # No fixed size: a PhotoImage created with width/height never grows, and these must follow the zoom.
        self.img_src = tk.PhotoImage()
        self.img_out = tk.PhotoImage()
        self.item_src = self.canvas_src.create_image(0, 0, anchor="nw", image=self.img_src)
        self.item_out = self.canvas_out.create_image(0, 0, anchor="nw", image=self.img_out)

        # HUD
        hud = ttk.Frame(left, style="Panel.TFrame", padding=12)
        hud.grid(row=3, column=0, columnspan=2, sticky="ew", pady=(12, 0))
        self.hud = {}
        items = [("target", "TARGET SPEED"), ("measured", "MEASURED SPEED"), ("lock", "SYNC LOCK"),
                 ("jitter", "OPTO JITTER"), ("duty", "LED DUTY CYCLE"), ("watchdog", "SAFETY WATCHDOG"),
                 ("capture", "CAPTURE"), ("frames", "FRAMES TO DISK"), ("temp", "CPU TEMPERATURE"),
                 ("motor", "MOTOR DRIVER"), ("sensor", "INDEX SENSOR"), ("led", "LED OUTPUT")]
        for i, (key, label) in enumerate(items):
            cell = ttk.Frame(hud, style="Panel.TFrame")
            cell.grid(row=i // 4, column=i % 4, sticky="w", padx=(0, 12), pady=(0, 8))
            ttk.Label(cell, text=label, style="Muted.TLabel").pack(anchor="w")
            self.hud[key] = ttk.Label(cell, text="—", style="Value.TLabel", width=HUD_WIDTH, anchor="w")
            self.hud[key].pack(anchor="w")
            hud.columnconfigure(i % 4, weight=1, uniform="hud")

        # Right: controls
        right = ttk.Frame(root, padding=(12, 8, 24, 8))
        right.grid(row=1, column=1, sticky="nsew")
        right.columnconfigure(0, weight=1)
        right.rowconfigure(3, weight=1)

        ctl = ttk.Frame(right, style="Panel.TFrame", padding=14)
        ctl.grid(row=0, column=0, sticky="ew", pady=(0, 12))
        ctl.columnconfigure(1, weight=1)
        ttk.Label(ctl, text="Motor & LED", style="Heading.TLabel").grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, 8))
        buttons = ttk.Frame(ctl, style="Panel.TFrame")
        buttons.grid(row=1, column=0, columnspan=3, sticky="w", pady=(0, 10))
        self.btn_start = ttk.Button(buttons, text="▶  Spin up motor", style="Primary.TButton", command=lambda: self.action("start"))
        self.btn_start.pack(side="left", padx=(0, 8))
        self.btn_stop = ttk.Button(buttons, text="■  Stop motor", command=lambda: self.action("stop"))
        self.btn_stop.pack(side="left", padx=(0, 8))
        ttk.Button(buttons, text="⚠  Emergency blank", style="Danger.TButton", command=lambda: self.action("emergency_blank")).pack(side="left")

        ttk.Label(ctl, text="Speed", style="Panel.TLabel").grid(row=2, column=0, sticky="w")
        self.speed = self._scale(ctl, 1.0, MAX_FPS, 0.1, lambda v: self.action("speed", fps=float(v)))
        self.speed.set(self.realtime.target_fps)
        self.speed.grid(row=2, column=1, sticky="ew", padx=10)
        self.speed_val = ttk.Label(ctl, text="", style="Muted.TLabel", width=20)
        self.speed_val.grid(row=2, column=2, sticky="w")
        self.speed.configure(command=lambda v: self.speed_val.configure(text=f"{float(v):.1f} FPS ({round(float(v) * 60)} RPM)"))
        self.speed_val.configure(text=f"{self.realtime.target_fps:.1f} FPS ({round(self.realtime.target_fps * 60)} RPM)")

        ttk.Label(ctl, text="LED brightness", style="Panel.TLabel").grid(row=3, column=0, sticky="w")
        self.bright = self._scale(ctl, 0, 100, 1, lambda v: self.action("calibration", brightness=float(v) / 100))
        self.bright.grid(row=3, column=1, sticky="ew", padx=10)
        self.bright_val = ttk.Label(ctl, text="", style="Muted.TLabel", width=20)
        self.bright_val.grid(row=3, column=2, sticky="w")
        self.bright.configure(command=lambda v: self.bright_val.configure(text=f"{int(float(v))}%"))

        self.free_run_var = tk.BooleanVar(value=self.realtime.free_run)
        self.free_run_box = ttk.Checkbutton(
            ctl, variable=self.free_run_var, command=self._free_run_clicked,
            text="Free-run output: ignore disk sync, clock the LED from an internal timer (LED fires with no disk spinning)")
        self.free_run_box.grid(row=4, column=0, columnspan=3, sticky="w", pady=(10, 0))

        cal = ttk.Frame(right, style="Panel.TFrame", padding=14)
        cal.grid(row=1, column=0, sticky="ew", pady=(0, 12))
        cal.columnconfigure(1, weight=1)
        ttk.Label(cal, text="Nipkow disc calibration", style="Heading.TLabel").grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, 8))
        ttk.Label(cal, text="Phase offset", style="Panel.TLabel").grid(row=1, column=0, sticky="w")
        self.phase = self._scale(cal, 0, 799, 1, lambda v: self.action("calibration", phase_offset=int(float(v))))
        self.phase.grid(row=1, column=1, sticky="ew", padx=10)
        self.phase_val = ttk.Label(cal, text="", style="Muted.TLabel", width=20)
        self.phase_val.grid(row=1, column=2, sticky="w")
        self.phase.configure(command=lambda v: self.phase_val.configure(text=f"{int(float(v))} px ({round(int(float(v)) * 360 / 800)}°)"))
        ttk.Label(cal, text="Gamma", style="Panel.TLabel").grid(row=2, column=0, sticky="w")
        self.gamma = self._scale(cal, 0.5, 3.0, 0.1, lambda v: self.action("calibration", gamma=float(v)))
        self.gamma.grid(row=2, column=1, sticky="ew", padx=10)
        self.gamma_val = ttk.Label(cal, text="", style="Muted.TLabel", width=20)
        self.gamma_val.grid(row=2, column=2, sticky="w")
        self.gamma.configure(command=lambda v: self.gamma_val.configure(text=f"{float(v):.1f}"))
        ttk.Label(cal, text="Contrast", style="Panel.TLabel").grid(row=3, column=0, sticky="w")
        self.contrast = self._scale(cal, 0.5, 2.5, 0.1, lambda v: self.action("calibration", contrast=float(v)))
        self.contrast.grid(row=3, column=1, sticky="ew", padx=10)
        self.contrast_val = ttk.Label(cal, text="", style="Muted.TLabel", width=20)
        self.contrast_val.grid(row=3, column=2, sticky="w")
        self.contrast.configure(command=lambda v: self.contrast_val.configure(text=f"{float(v):.1f}"))
        flips = ttk.Frame(cal, style="Panel.TFrame")
        flips.grid(row=4, column=0, columnspan=3, sticky="w", pady=(8, 0))
        self.inv_x = tk.BooleanVar(value=self.pipeline.invert_x)
        self.inv_y = tk.BooleanVar(value=self.pipeline.invert_y)
        ttk.Checkbutton(flips, text="Invert horizontal (flip X)", variable=self.inv_x,
                        command=lambda: self.action("calibration", invert_x=self.inv_x.get())).pack(side="left", padx=(0, 16))
        ttk.Checkbutton(flips, text="Invert vertical (flip Y)", variable=self.inv_y,
                        command=lambda: self.action("calibration", invert_y=self.inv_y.get())).pack(side="left")

        lib = ttk.Frame(right, style="Panel.TFrame", padding=14)
        lib.grid(row=3, column=0, sticky="nsew")
        lib.columnconfigure(0, weight=1)
        lib.rowconfigure(1, weight=1)
        ttk.Label(lib, text="Media library", style="Heading.TLabel").grid(row=0, column=0, sticky="w", pady=(0, 8))
        self.tree = ttk.Treeview(lib, columns=("name", "status", "length"), show="headings", selectmode="browse", height=6)
        self.tree.heading("name", text="NAME")
        self.tree.heading("status", text="STATUS")
        self.tree.heading("length", text="LENGTH")
        self.tree.column("name", width=220, anchor="w")
        self.tree.column("status", width=110, anchor="w")
        self.tree.column("length", width=70, anchor="e")
        self.tree.grid(row=1, column=0, sticky="nsew")
        scroll = ttk.Scrollbar(lib, orient="vertical", command=self.tree.yview)
        scroll.grid(row=1, column=1, sticky="ns")
        self.tree.configure(yscrollcommand=scroll.set)
        lib_buttons = ttk.Frame(lib, style="Panel.TFrame")
        lib_buttons.grid(row=2, column=0, columnspan=2, sticky="w", pady=(10, 0))
        ttk.Button(lib_buttons, text="Add video file…", command=self.add_file).pack(side="left", padx=(0, 8))
        ttk.Button(lib_buttons, text="Show on disk", style="Primary.TButton", command=self.select_clip).pack(side="left", padx=(0, 8))
        ttk.Button(lib_buttons, text="Delete", command=self.delete_clip).pack(side="left")
        self.fit_var = tk.StringVar(value="fit")
        fit_box = ttk.Frame(lib, style="Panel.TFrame")
        fit_box.grid(row=3, column=0, columnspan=2, sticky="w", pady=(8, 0))
        ttk.Label(fit_box, text="Import framing:", style="Muted.TLabel").pack(side="left", padx=(0, 8))
        ttk.Checkbutton(fit_box, text="Crop to fill (instead of letterbox)", variable=self.fit_var, onvalue="crop", offvalue="fit").pack(side="left")
        self.lib_status = ttk.Label(lib, text="", style="Muted.TLabel", wraplength=520, justify="left")
        self.lib_status.grid(row=4, column=0, columnspan=2, sticky="w", pady=(8, 0))

        # Footer
        footer = ttk.Frame(root, padding=(24, 4, 24, 10))
        footer.grid(row=2, column=0, columnspan=2, sticky="ew")
        footer.columnconfigure(0, weight=1)
        self.notice_label = ttk.Label(footer, text="", style="Notice.TLabel")
        self.notice_label.grid(row=0, column=0, sticky="w")
        self.footer_right = ttk.Label(footer, text="", style="Eyebrow.TLabel")
        self.footer_right.grid(row=0, column=1, sticky="e")

    # -- actions (worker thread) ------------------------------------------

    def action(self, name, **params):
        """Queue a hardware/engine call; the worker runs it and reports errors as notices."""
        self.jobs.put((name, params))

    def _job_loop(self):
        rt = self.realtime
        while True:
            name, params = self.jobs.get()
            if name is None:
                return
            LOGGER.info("action %s %s", name, {k: v for k, v in params.items()} if params else "")
            try:
                if name == "start":
                    rt.start()
                elif name == "stop":
                    rt.stop()
                elif name == "emergency_blank":
                    rt.emergency_blank()
                    self.notice("Emergency blank: LED off, motor stopped, free-run off.")
                elif name == "speed":
                    rt.set_target_fps(params["fps"])
                elif name == "free_run":
                    rt.set_free_run(params["enabled"])
                    self.notice("Free-run output ON: internal clock drives the LED; the disk sync and stall interlock are bypassed."
                                if params["enabled"] else "Free-run output off: the index sensor clocks the LED again.")
                elif name == "calibration":
                    rt.set_calibration(**params)
                elif name == "source":
                    mode = params["mode"]
                    if mode == "pattern":
                        rt.set_source_mode("pattern")
                        rt.set_pattern(params["pattern"])
                    elif mode == "library":
                        rt.set_source_mode("library", library_path=self.studio.clip_frames_path(params["id"]))
                    else:
                        rt.set_source_mode("hdmi")
                elif name == "import":
                    ident = self.studio.import_file(params["path"], fit=params["fit"])
                    self.notice(f"Queued {Path(params['path']).name} for conversion ({ident[:8]}).")
                elif name == "delete":
                    self.studio.library.delete(params["id"])
                    self.notice("Clip deleted.")
                elif name == "close":
                    self.studio.close()
            except Exception as exc:
                self.notice(f"{name}: {exc}")
                log_exception(f"action {name}", exc)
            finally:
                self.jobs.task_done()

    def notice(self, text):
        self.notices.put(text)

    def _tk_exception(self, exc_type, exc_value, tb):
        """Tk callback errors: log with traceback and show in the footer instead of dying silently."""
        exc = exc_value if isinstance(exc_value, BaseException) else exc_type(exc_value)
        log_exception("ui callback", exc)
        self.notice(f"UI error: {exc_type.__name__}: {exc_value}")

    # -- UI events -----------------------------------------------------------

    def _space(self, event=None):
        if isinstance(event.widget, (self.tk.Entry, self.ttk.Combobox)):
            return
        self.action("stop" if self.realtime.state != "idle" else "start")

    def _free_run_clicked(self):
        self._free_run_changed_at = time.monotonic()
        self.action("free_run", enabled=self.free_run_var.get())

    def _source_selected(self, event=None):
        key = SOURCE_BY_LABEL.get(self.source_var.get(), "smpte_bars")
        self._source_changed_at = time.monotonic()
        if key == "hdmi":
            self.action("source", mode="hdmi")
        elif key == "library":
            if not self.selected_clip:
                self.notice("Select a ready clip in the media library first.")
                return
            self.action("source", mode="library", id=self.selected_clip)
        else:
            self.action("source", mode="pattern", pattern=key)

    def add_file(self):
        from tkinter import filedialog
        path = filedialog.askopenfilename(
            parent=self.root, title="Add a video to the library",
            filetypes=[("Video files", "*.mp4 *.m4v *.mov *.mkv *.webm *.avi"), ("All files", "*")])
        if path:
            self.action("import", path=path, fit=self.fit_var.get())

    def _tree_selection(self):
        selection = self.tree.selection()
        return selection[0] if selection else None

    def select_clip(self):
        ident = self._tree_selection()
        if not ident:
            self.notice("Choose a clip in the list first.")
            return
        try:
            self.studio.clip_frames_path(ident)
        except ValueError as exc:
            self.notice(str(exc))
            return
        self.selected_clip = ident
        self.source_var.set(SOURCE_LABEL["library"])
        self._source_changed_at = time.monotonic()
        self.action("source", mode="library", id=ident)

    def delete_clip(self):
        from tkinter import messagebox
        ident = self._tree_selection()
        if not ident:
            self.notice("Choose a clip in the list first.")
            return
        name = self.tree.set(ident, "name")
        if not messagebox.askyesno("Delete clip", f"Delete “{name}” and its prepared files?", parent=self.root):
            return
        if self.selected_clip == ident:
            self.selected_clip = None
            if self.pipeline.source_mode == "library":
                self.source_var.set(SOURCE_LABEL["smpte_bars"])
                self.action("source", mode="pattern", pattern="smpte_bars")
        self.action("delete", id=ident)

    def set_fullscreen(self, enabled: bool):
        self._fullscreen = bool(enabled)
        try:
            self.root.attributes("-fullscreen", self._fullscreen)
        except self.tk.TclError:
            pass

    def quit(self):
        if self.closing:
            return
        self.closing = True
        LOGGER.info("console closing: stopping motor, blanking LED")
        self.notice("Shutting down: stopping motor and blanking LED…")
        self.root.update_idletasks()
        try:
            self.studio.close()
        finally:
            self.jobs.put((None, {}))
            self.root.destroy()

    # -- periodic refresh ------------------------------------------------------

    def _canvas_resized(self, event=None):
        cw = max(1, self.canvas_src.winfo_width())
        ch = max(1, self.canvas_src.winfo_height())
        self._canvas_size = (cw, ch)
        self._zoom = zoom = max(1, min(cw // WIDTH, ch // HEIGHT))
        self._pzoom = max(1, min(cw // PREVIEW_W, ch // PREVIEW_H))
        self._src_shown = None   # _paint re-centres the left pane for whichever picture it shows
        self.canvas_out.coords(self.item_out, (cw - WIDTH * zoom) // 2, (ch - HEIGHT * zoom) // 2)

    def _paint(self, frame: bytes, status: dict):
        zoom, pzoom = self._zoom, self._pzoom
        preview = self.pipeline.get_source_preview()
        # Sources hand out the same bytes object until a new frame lands, so identity tells us
        # whether the left pane needs re-encoding (the 240 KB preview costs ~5 ms per paint).
        last_frame, last_preview, last_zoom = self._painted
        if frame is not last_frame or preview is not last_preview or (zoom, pzoom) != last_zoom:
            self._painted = (frame, preview, (zoom, pzoom))
            if preview is not None:
                # Whole pixels at an integer zoom: Tk's "copy" cannot scale by a fraction (its
                # -subsample picks every nth pixel first), so the preview keeps its own zoom.
                self.img_preview_small.configure(data=ppm(preview))
                self.img_src.tk.call(self.img_src, "copy", self.img_preview_small, "-zoom", pzoom, pzoom, "-shrink")
                shown = (PREVIEW_W * pzoom, PREVIEW_H * pzoom)
            else:
                self.img_src_small.configure(data=pgm(frame))
                self.img_src.tk.call(self.img_src, "copy", self.img_src_small, "-zoom", zoom, zoom, "-shrink")
                shown = (WIDTH * zoom, HEIGHT * zoom)
            if shown != self._src_shown:
                self._src_shown = shown
                cw, ch = self._canvas_size
                self.canvas_src.coords(self.item_src, (cw - shown[0]) // 2, (ch - shown[1]) // 2)
        if (preview is not None) != self._showing_preview:
            self._showing_preview = preview is not None
            self.label_src.configure(text=SRC_LABEL_LIVE if self._showing_preview else SRC_LABEL_FRAME)
        stream = self.pipeline.serialize_nipkow_stream(frame)
        shown = self.pipeline.reconstruct_display_preview(stream)
        self.img_out_small.configure(data=pgm(shown))
        self.img_out.tk.call(self.img_out, "copy", self.img_out_small, "-zoom", zoom, zoom, "-shrink")

    def _tick(self):
        if self.closing:
            return
        try:
            while True:
                self.notice_label.configure(text=self.notices.get_nowait())
        except queue.Empty:
            pass
        try:
            status = self.realtime.get_status()
            frame = self.realtime.get_preview_frame()
            self._paint(frame, status)
            self._update_hud(status)
            now = time.monotonic()
            if now - self._last_library_at >= LIBRARY_REFRESH_S:
                self._last_library_at = now
                self._refresh_library()
        except Exception as exc:  # keep the console alive; report instead of dying silently
            self.notice_label.configure(text=f"UI error: {type(exc).__name__}: {exc}")
            log_exception("ui refresh", exc)
        self._drain_log()
        self.root.after(int(1000 / PREVIEW_HZ), self._tick)

    def _drain_log(self):
        drained = 0
        try:
            while drained < 200:
                record = self.log_queue.get_nowait()
                drained += 1
                if record.levelno >= logging.ERROR:
                    self.error_count += 1
                self.debug.append(record)
        except queue.Empty:
            pass
        if drained:
            if self.error_count:
                self.pill_log.configure(text=f"LOG: {self.error_count} ERROR{'S' if self.error_count != 1 else ''}")
                self._pill_color(self.pill_log, "danger")
            else:
                self.pill_log.configure(text="LOG")
                self._pill_color(self.pill_log, "idle")

    def _update_hud(self, rt: dict):
        state = rt["state"]
        locked = bool(rt["sync_locked"])
        free_run = bool(rt.get("free_run"))
        self.pill_hw.configure(text="REALTIME HARDWARE" if self.studio.hardware_enabled else "SIMULATION")
        self._pill_color(self.pill_hw, "live" if self.studio.hardware_enabled else "danger")
        self.pill_motor.configure(text=f"MOTOR: {state.upper()}")
        self._pill_color(self.pill_motor, state)
        if free_run:
            self.pill_sync.configure(text="SYNC: FREE-RUN")
            self._pill_color(self.pill_sync, "free-run")
        else:
            self.pill_sync.configure(text=f"SYNC: {'LOCKED' if locked else 'UNLOCKED'}")
            self._pill_color(self.pill_sync, "locked" if locked else ("idle" if state == "idle" else "ramping"))
        if self.free_run_var.get() != free_run and time.monotonic() - self._free_run_changed_at > 3.0:
            self.free_run_var.set(free_run)
        self.btn_start.state(["disabled"] if state != "idle" else ["!disabled"])
        self.btn_stop.state(["!disabled"] if state != "idle" else ["disabled"])

        hud = self.hud
        hud["target"].configure(text=f"{round(rt['target_rpm'])} RPM ({rt['target_fps']:.1f} FPS)")
        hud["measured"].configure(text=f"{rt['measured_rpm']:.1f} RPM")
        hud["lock"].configure(text="FREE-RUN CLOCK" if free_run else "LOCKED (±0.4%)" if locked else "STANDBY" if state == "idle" else "ACQUIRING")
        hud["jitter"].configure(text=f"{rt['jitter_ms']:.2f} ms")
        led = rt.get("led", {})
        hud["duty"].configure(text=f"{(led.get('average_duty_cycle') or 0) * 100:.1f}%")
        hud["watchdog"].configure(text="TRIPPED (OFF)" if led.get("safety_tripped") else "BYPASSED (FREE-RUN)" if free_run else "ARMED" if state == "idle" else "ACTIVE")
        hud["frames"].configure(text=str(rt["frame_counter"]))
        temp = self.studio.temperature_c()
        hud["temp"].configure(text=f"{temp:.1f} °C" if temp is not None else "n/a")
        motor = rt.get("motor", {})
        drv = motor.get("driver")
        drv = drv if isinstance(drv, dict) else {"driver": drv}
        name = str(drv.get("driver", "?")).replace("Driver", "").replace("Adafruit", "")
        hud["motor"].configure(text=self._fit(f"{name} · {drv.get('mode', '')} · {motor.get('current_rpm', 0):.0f} RPM".replace(" ·  ·", " ·")))
        sensor = rt.get("sensor", {})
        hud["sensor"].configure(text=self._fit(f"{sensor.get('backend', sensor.get('driver', '?'))} · GPIO {sensor.get('pin', '?')} · {sensor.get('pulse_count', 0)} pulses"))
        hud["led"].configure(text=self._fit(f"{led.get('backend', led.get('driver', '?'))} · {'BLANK' if led.get('is_blanked') else 'ON'} · {led.get('frames_rendered', 0)} frames"))

        pipe = rt.get("pipeline", {})
        self._sync_source_widgets(pipe)
        self._sync_calibration_widgets(self.pipeline)
        if self.speed.get() != rt["target_fps"] and not self._dragging(self.speed):
            self.speed.set(rt["target_fps"])
        self.footer_right.configure(text=f"v{__version__} · data {self.studio.root} · Esc leaves full screen · Ctrl+Q quits")

    SCALE_GRACE_S = 1.5   # after a release, give the worker time to apply the value before syncing back

    def _dragging(self, scale):
        # Mouse tracking from _scale(); no Tk focus lookup, which raised KeyError whenever a
        # Combobox list ("popdown", a Tcl-only toplevel) held the focus and aborted the refresh.
        last = self._scale_busy.get(scale)
        return last is not None and time.monotonic() - last < self.SCALE_GRACE_S

    def _sync_calibration_widgets(self, pipe):
        if not self._dragging(self.bright) and int(self.bright.get()) != round(pipe.brightness * 100):
            self.bright.set(round(pipe.brightness * 100))
            self.bright_val.configure(text=f"{round(pipe.brightness * 100)}%")
        if not self._dragging(self.phase) and int(self.phase.get()) != pipe.phase_offset:
            self.phase.set(pipe.phase_offset)
        if not self._dragging(self.gamma) and abs(float(self.gamma.get()) - pipe.gamma) > 0.05:
            self.gamma.set(pipe.gamma)
        if not self._dragging(self.contrast) and abs(float(self.contrast.get()) - pipe.contrast) > 0.05:
            self.contrast.set(pipe.contrast)
        if self.inv_x.get() != pipe.invert_x:
            self.inv_x.set(pipe.invert_x)
        if self.inv_y.get() != pipe.invert_y:
            self.inv_y.set(pipe.invert_y)

    def _sync_source_widgets(self, pipe: dict):
        details = pipe.get("source_details") or {}
        mode = pipe.get("source_mode")
        if time.monotonic() - self._source_changed_at > 2.0:
            wanted = "hdmi" if mode == "hdmi" else "library" if mode == "library" else details.get("pattern", "smpte_bars")
            if wanted in SOURCE_LABEL and self.source_var.get() != SOURCE_LABEL[wanted]:
                self.source_var.set(SOURCE_LABEL[wanted])
        if mode == "hdmi":
            where = f"HDMI {details.get('device', '')} · {(details.get('bridge') or '').upper()}"
            if details.get("backend"):
                where += f" · {details['backend']}"
            if details.get("signal"):
                fps = f" @ {details.get('input_fps', 0):.0f} FPS" if details.get("input_fps") else ""
                if details.get("picture"):
                    text, hint, color = f"{where} · {details.get('input_resolution') or '?'}{fps} · LIVE", "", "live"
                else:
                    text = f"{where} · {details.get('input_resolution') or '?'}{fps} · BLACK PICTURE (peak {details.get('frame_peak', 0)}/255)"
                    hint = ("Signal locked and frames are arriving, but every pixel is black: the device on the HDMI cable is "
                            "sending a black picture. Wake it, un-blank its second display, or move a window onto that display.")
                    color = "ramping"
            elif details.get("connected"):
                text, color = f"{where} · WAITING FOR FRAMES", "ramping"
                hint = details.get("error") or "Timings locked; waiting for the first frame from the capture backend."
            else:
                text, color = f"{where} · NO SIGNAL", "danger"
                hint = details.get("error") or "Waiting for an HDMI source. The source must show a second display."
            self.pill_hdmi.configure(text="HDMI: LIVE" if details.get("picture") else "HDMI: BLACK" if details.get("signal") else "HDMI: NO SIGNAL")
            self._pill_color(self.pill_hdmi, color)
        elif mode == "library":
            total = details.get("total_frames", 0)
            text = f"LIBRARY CLIP · frame {details.get('current_frame', 0)}/{total}" if total else "LIBRARY CLIP · nothing loaded"
            hint = ""
            self.pill_hdmi.configure(text="HDMI: IDLE")
            self._pill_color(self.pill_hdmi, "idle")
        else:
            text, hint = f"PROCEDURAL PATTERN · {details.get('pattern', '')}", ""
            self.pill_hdmi.configure(text="HDMI: IDLE")
            self._pill_color(self.pill_hdmi, "idle")
        self.source_status.configure(text=text)
        self.source_hint.configure(text=hint)
        hud_capture = text.split(" · ", 1)[-1] if mode == "hdmi" else text
        self.hud["capture"].configure(text=self._fit(hud_capture))

    @staticmethod
    def _fit(text: str) -> str:
        return text if len(text) <= HUD_WIDTH else text[:HUD_WIDTH - 1] + "…"

    def _refresh_library(self):
        items = self.studio.library.items()
        key = json.dumps(items, sort_keys=True, default=str)
        if key == self._last_library:
            return
        self._last_library = key
        selected = self._tree_selection()
        self.tree.delete(*self.tree.get_children())
        for item in items:
            length = f"{int(item['duration'] // 60)}:{int(item['duration'] % 60):02d}" if item.get("duration") else ""
            status = item["status"].upper() + (f" — {item['error']}" if item.get("error") else "")
            self.tree.insert("", "end", iid=item["id"], values=(item["name"], status, length))
        if selected and self.tree.exists(selected):
            self.tree.selection_set(selected)
        pending = self.studio.pending_jobs()
        self.lib_status.configure(text=(f"{len(items)} clip(s) · {pending} converting" if pending else f"{len(items)} clip(s)")
                                  if items else "Library is empty. Add an MP4/WebM/MOV file to convert it to 32 × 25 frames.")


class DebugWindow:
    """A separate window with the live log: every engine message, action and error, newest at the bottom.

    The same lines are in the log file (path shown in the window), so a fault survives a restart.
    """

    def __init__(self, console: "Console", log_path):
        tk, ttk = console.tk, console.ttk
        self.console = console
        self.win = tk.Toplevel(console.root)
        self.win.title("Mechanical TV — debug log")
        self.win.configure(bg=C["bg"])
        self.win.geometry("980x520")
        self.win.protocol("WM_DELETE_WINDOW", self.hide)
        self.win.bind("<F12>", lambda e: self.hide())
        self.win.bind("<Escape>", lambda e: self.hide())
        self.win.columnconfigure(0, weight=1)
        self.win.rowconfigure(1, weight=1)
        bar = ttk.Frame(self.win, padding=(12, 10, 12, 6))
        bar.grid(row=0, column=0, sticky="ew")
        bar.columnconfigure(3, weight=1)
        ttk.Button(bar, text="Clear view", command=self.clear).grid(row=0, column=0, padx=(0, 8))
        ttk.Button(bar, text="Mark", command=lambda: LOGGER.info("---- operator mark ----")).grid(row=0, column=1, padx=(0, 8))
        self.errors_only = tk.BooleanVar(value=False)
        chk = ttk.Checkbutton(bar, text="Errors only", variable=self.errors_only, command=self._refilter, style="Dark.TCheckbutton")
        chk.grid(row=0, column=2, padx=(0, 12))
        ttk.Label(bar, text=f"Log file: {log_path or 'unavailable'}", style="Eyebrow.TLabel").grid(row=0, column=3, sticky="e")
        self.text = tk.Text(self.win, bg="#0b0d0c", fg=C["text"], insertbackground=C["text"], font=console.font_mono,
                            wrap="word", state="disabled", highlightthickness=0, bd=0, padx=10, pady=8)
        self.text.grid(row=1, column=0, sticky="nsew")
        scroll = ttk.Scrollbar(self.win, orient="vertical", command=self.text.yview)
        scroll.grid(row=1, column=1, sticky="ns")
        self.text.configure(yscrollcommand=scroll.set)
        self.text.tag_configure("ERROR", foreground=C["danger"])
        self.text.tag_configure("CRITICAL", foreground=C["danger"])
        self.text.tag_configure("WARNING", foreground=C["warn"])
        self.text.tag_configure("INFO", foreground=C["text"])
        self.text.tag_configure("DEBUG", foreground=C["muted"])
        self.formatter = logging.Formatter("%(asctime)s %(levelname)-7s %(threadName)s: %(message)s", "%H:%M:%S")
        self.records = []
        self.visible = False
        self.win.withdraw()
        ttk.Style(console.root).configure("Dark.TCheckbutton", background=C["bg"], foreground=C["text"])

    def append(self, record):
        self.records.append(record)
        if len(self.records) > LOG_LINES:
            del self.records[: len(self.records) - LOG_LINES]
        if self.errors_only.get() and record.levelno < logging.ERROR:
            return
        self._write(record)

    def _write(self, record):
        at_end = self.text.yview()[1] >= 0.999
        self.text.configure(state="normal")
        self.text.insert("end", self.formatter.format(record) + "\n", record.levelname)
        lines = int(self.text.index("end-1c").split(".")[0])
        if lines > LOG_LINES:
            self.text.delete("1.0", f"{lines - LOG_LINES}.0")
        self.text.configure(state="disabled")
        if at_end:
            self.text.see("end")

    def _refilter(self):
        self.text.configure(state="normal")
        self.text.delete("1.0", "end")
        self.text.configure(state="disabled")
        for record in self.records:
            if not self.errors_only.get() or record.levelno >= logging.ERROR:
                self._write(record)

    def clear(self):
        self.records.clear()
        self._refilter()

    def show(self):
        self.visible = True
        self.win.deiconify()
        self.win.lift()
        self.text.see("end")

    def hide(self):
        self.visible = False
        self.win.withdraw()

    def toggle(self):
        self.hide() if self.visible else self.show()


def build_parser():
    pre = argparse.ArgumentParser(add_help=False)
    pre.add_argument("--env-file")
    pre_args, _ = pre.parse_known_args()
    if pre_args.env_file:
        load_env_file(pre_args.env_file)
    env = os.environ
    parser = argparse.ArgumentParser(description="Mechanical TV local operator console (runs on the Pi's own display)")
    parser.add_argument("--env-file", help="read MTV_* defaults from a KEY=value file, e.g. /etc/mechanical-tv/environment")
    parser.add_argument("--data", default=env.get("MTV_DATA") or str(DEFAULT_DATA),
                        help=f"library and settings directory (default: {DEFAULT_DATA})")
    parser.add_argument("--fullscreen", action="store_true", default=env.get("MTV_FULLSCREEN") == "1",
                        help="start full screen (F11 toggles, Esc leaves)")
    parser.add_argument("--check", action="store_true", help="print the environment report as JSON and exit without a window")
    parser.add_argument("--free-run", action="store_true", help="start with free-run output enabled (bench test without a disk)")
    parser.add_argument("--debug-window", action="store_true", default=env.get("MTV_DEBUG") == "1", help="open the debug log window at start (F12 toggles it)")
    parser.add_argument("--mock-hardware", action="store_true", default=env.get("MTV_MOCK_HARDWARE") == "1")
    parser.add_argument("--hdmi-device", default=env.get("MTV_HDMI_DEVICE", "/dev/video0"))
    parser.add_argument("--hdmi-bridge", choices=["auto", "csi", "usb"], default=env.get("MTV_HDMI_BRIDGE", "auto"))
    parser.add_argument("--hdmi-format", default=env.get("MTV_HDMI_FORMAT", "auto"))
    parser.add_argument("--hdmi-edid", default=env.get("MTV_HDMI_EDID", "auto"))
    parser.add_argument("--hdmi-fit", choices=["fit", "crop"], default=env.get("MTV_HDMI_FIT", "fit"))
    parser.add_argument("--hdmi-backend", choices=["auto", "v4l2", "ffmpeg", "opencv"], default=env.get("MTV_HDMI_BACKEND", "auto"),
                        help="auto: in-process V4L2 reader for a UYVY CSI bridge (lowest latency), else FFmpeg")
    parser.add_argument("--default-source", choices=["auto", "pattern", "hdmi"], default=env.get("MTV_DEFAULT_SOURCE", "auto"),
                        help="video input at start: auto picks hdmi when the capture device exists, else pattern")
    parser.add_argument("--motor-port", type=int, default=int(env.get("MTV_MOTOR_PORT", "1")))
    parser.add_argument("--opto-pin", type=int, default=int(env.get("MTV_OPTO_PIN", "4")))
    parser.add_argument("--led-pin", type=int, default=int(env.get("MTV_LED_PIN", "21")))
    parser.add_argument("--spi-led", action="store_true", default=env.get("MTV_SPI_LED") == "1")
    return parser


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.check:
        report = check_report(args)
        print(json.dumps(report, indent=2))
        return 0 if report["tk"] and report["ffmpeg"] else 1
    try:
        import tkinter as tk
    except ImportError as exc:
        raise SystemExit(f"tkinter is not installed ({exc}). On Raspberry Pi OS: sudo apt install python3-tk")
    try:
        root = tk.Tk()
    except tk.TclError as exc:
        raise SystemExit(f"No display: {exc}. Run from the Pi desktop, or use `python3 -m mechanical_tv.server` for the web interface.")
    args.data = usable_data_dir(args.data)
    log_queue = queue.Queue()
    log_path = None
    try:
        log_path = setup_logging(Path(args.data) / "logs", listener=log_queue)
    except OSError as exc:
        print(f"Log file unavailable: {exc}", flush=True)
    LOGGER.info("Mechanical TV %s local console starting (data %s)", __version__, args.data)
    studio, note = build_studio(args)
    if args.free_run:
        studio.realtime.set_free_run(True)
    console = Console(root, studio, fullscreen=args.fullscreen, hardware_note=note, log_queue=log_queue, log_path=log_path)
    if args.debug_window:
        console.debug.show()
    mode = "REALTIME HARDWARE" if studio.hardware_enabled else "SIMULATION"
    print(f"Mechanical TV {__version__}: local console — {mode}", flush=True)
    try:
        root.mainloop()
    except KeyboardInterrupt:
        pass
    finally:
        if not console.closing:
            console.closing = True
            studio.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
