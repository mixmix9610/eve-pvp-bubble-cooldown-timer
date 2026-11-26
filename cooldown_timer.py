import os
import sys
import time
import math
import threading
import configparser
import tkinter as tk
from tkinter import ttk, messagebox
from PIL import Image, ImageDraw, ImageTk
import numpy as np
import cv2
import pyautogui
import winsound
import pystray
from pystray import MenuItem as item
import keyboard

A_ScriptDir = os.path.dirname(os.path.abspath(__file__))
A_ScreenWidth, A_ScreenHeight = pyautogui.size()

TRIGGER_IMAGE_LIST = [
    os.path.join(A_ScriptDir, "trigger_1.png"),
    os.path.join(A_ScriptDir, "trigger_2.png"),
]
TRIGGER_IMAGES = []
SEARCH_REGION = None
settings_window = None
IMAGE_TOLERANCE = 70
COUNTDOWN_SEC = 120
CHECK_INTERVAL = 300
GLOBAL_UPDATE_INTERVAL = 100
BEEP_LAST_SECONDS = 5
FLASH_START_SECONDS = 30
PLAY_SOUND_WHEN_HIDDEN = False
TEMPLATE_MATCH_DEBUG = False

WINDOW_WIDTH = 100
WINDOW_HEIGHT = 130
ICON_SIZE = 96
FONT_SIZE = 24
BG_COLOR = "#FF00FF"
PROGRESS_COLOR = "#00FF00"
PROGRESS_BACK = "#222222"
FONT_COLOR = "#00FF00"
SHADOW_COLOR = "#003300"
FLASH_PROGRESS_COLOR = "#FF0000"
FLASH_FONT_COLOR = "#FFFF00"
FONT_NAME = "Digital-7"
CONFIG_FILE = os.path.join(A_ScriptDir, "CoolDownTimer.ini")
CACHED_ICON_STEPS = 36
ICON_BASE_IMAGE = None
ICON_IMAGE_CACHE = {}

def get_base_icon():
    global ICON_BASE_IMAGE
    if ICON_BASE_IMAGE is not None:
        return ICON_BASE_IMAGE
    # 尝试从工作目录加载背景图，记录调试信息以便定位为何加载失败
    icon_path = os.path.join(A_ScriptDir, "paopao.jpg")
    print(f"[DEBUG] trying to load base icon: {icon_path}")
    image = None
    if os.path.exists(icon_path):
        try:
            image = Image.open(icon_path)
            print(f"[DEBUG] opened paopao.jpg mode={image.mode} size={image.size}")
            image = image.convert("RGBA")
            try:
                resample_filter = Image.Resampling.LANCZOS
            except Exception:
                resample_filter = getattr(Image, 'LANCZOS', Image.BICUBIC)
            image = image.resize((ICON_SIZE, ICON_SIZE), resample=resample_filter)
        except Exception as e:
            print(f"[ERROR] failed to load paopao.jpg: {e}")
            image = None
    if image is None:
        icon_path2 = os.path.join(A_ScriptDir, "paopao.png")
        print(f"[DEBUG] trying fallback icon: {icon_path2}")
        if os.path.exists(icon_path2):
            try:
                image = Image.open(icon_path2)
                print(f"[DEBUG] opened paopao.png mode={image.mode} size={image.size}")
                image = image.convert("RGBA")
                try:
                    resample_filter = Image.Resampling.LANCZOS
                except Exception:
                    resample_filter = getattr(Image, 'LANCZOS', Image.BICUBIC)
                image = image.resize((ICON_SIZE, ICON_SIZE), resample=resample_filter)
            except Exception as e:
                print(f"[ERROR] failed to load paopao.png: {e}")
                image = None
    if image is None:
        image = Image.new("RGBA", (ICON_SIZE, ICON_SIZE), (0, 0, 0, 0))
        draw = ImageDraw.Draw(image)
        draw.ellipse((0, 0, ICON_SIZE - 1, ICON_SIZE - 1), fill=(40, 40, 40, 255))
        draw.text((ICON_SIZE // 3, ICON_SIZE // 4), "?", fill=(255, 255, 255, 255))
    # 裁剪为圆形：把圆外区域设为透明
    mask = Image.new("L", (ICON_SIZE, ICON_SIZE), 0)
    mask_draw = ImageDraw.Draw(mask)
    mask_draw.ellipse((0, 0, ICON_SIZE, ICON_SIZE), fill=255)
    image.putalpha(mask)
    ICON_BASE_IMAGE = image
    return ICON_BASE_IMAGE

def ensure_icon_frames():
    if ICON_IMAGE_CACHE:
        return
    base_icon = get_base_icon()
    # 生成圆形基准遮罩（用于限制绿色遮罩只在圆内生效）
    circle_mask = Image.new("L", (ICON_SIZE, ICON_SIZE), 0)
    md = ImageDraw.Draw(circle_mask)
    md.ellipse((0, 0, ICON_SIZE, ICON_SIZE), fill=255)
    for step in range(CACHED_ICON_STEPS + 1):
        progress_ratio = step / CACHED_ICON_STEPS
        # 仅在圆内绘制半透明绿色扇形
        overlay = Image.new("RGBA", (ICON_SIZE, ICON_SIZE), (0, 0, 0, 0))
        draw_overlay = ImageDraw.Draw(overlay)
        if progress_ratio > 0:
            end_angle = -90 - int(360 * progress_ratio)
            draw_overlay.pieslice((0, 0, ICON_SIZE, ICON_SIZE), start=-90, end=end_angle, fill=(0, 255, 0, 100))
        # 限制 overlay 到圆形内部
        transparent = Image.new("RGBA", (ICON_SIZE, ICON_SIZE), (0, 0, 0, 0))
        overlay_masked = Image.composite(overlay, transparent, circle_mask)
        # 在基准图上合成 overlay
        combined = Image.alpha_composite(base_icon, overlay_masked)
        # 把透明背景区域用 BG_COLOR 填充，这样 Tk 的 -transparentcolor 会把这些像素变为透明
        hexc = BG_COLOR.lstrip('#')
        try:
            r = int(hexc[0:2], 16)
            g = int(hexc[2:4], 16)
            b = int(hexc[4:6], 16)
        except Exception:
            r, g, b = 255, 0, 255
        bg = Image.new("RGBA", (ICON_SIZE, ICON_SIZE), (r, g, b, 255))
        final = Image.alpha_composite(bg, combined)
        final_rgb = final.convert("RGB")
        ICON_IMAGE_CACHE[step] = ImageTk.PhotoImage(final_rgb)

def get_icon_frame(progress_step):
    ensure_icon_frames()
    return ICON_IMAGE_CACHE.get(progress_step)

timers = []
timer_counter = 0
triggered_images = set()
TRIGGER_STATE = {}
TRIGGER_INITIALIZED = False
g_sound_playing = False
g_last_sound_time = 0
g_last_tick_time = None
g_tick_count = 0
tray_icon = None
root = None


def play_sound(freq, duration):
    global g_sound_playing, g_last_sound_time
    now = int(time.time() * 1000)
    if now - g_last_sound_time < 200:
        return
    if g_sound_playing:
        return
    g_sound_playing = True
    g_last_sound_time = now
    try:
        winsound.Beep(freq, duration)
    except RuntimeError:
        pass
    g_sound_playing = False


def load_timer_pos(timer_id):
    config = configparser.ConfigParser()
    if not os.path.exists(CONFIG_FILE):
        return None
    config.read(CONFIG_FILE, encoding="utf-8")
    section = "TimerPos"
    key_x = f"Timer{timer_id}_X"
    key_y = f"Timer{timer_id}_Y"
    if section in config and key_x in config[section] and key_y in config[section]:
        try:
            return int(config[section][key_x]), int(config[section][key_y])
        except ValueError:
            return None
    return None


def save_timer_pos(timer_id, x, y):
    config = configparser.ConfigParser()
    if os.path.exists(CONFIG_FILE):
        config.read(CONFIG_FILE, encoding="utf-8")
    if "TimerPos" not in config:
        config["TimerPos"] = {}
    config["TimerPos"][f"Timer{timer_id}_X"] = str(x)
    config["TimerPos"][f"Timer{timer_id}_Y"] = str(y)
    with open(CONFIG_FILE, "w", encoding="utf-8") as f:
        config.write(f)


def load_search_region():
    global SEARCH_REGION
    config = configparser.ConfigParser()
    if not os.path.exists(CONFIG_FILE):
        SEARCH_REGION = None
        return
    config.read(CONFIG_FILE, encoding="utf-8")
    if "SearchRegion" in config:
        try:
            left = int(config["SearchRegion"].get("Left", 0))
            top = int(config["SearchRegion"].get("Top", 0))
            width = int(config["SearchRegion"].get("Width", A_ScreenWidth))
            height = int(config["SearchRegion"].get("Height", A_ScreenHeight))
            if width > 0 and height > 0:
                SEARCH_REGION = (left, top, width, height)
                return
        except ValueError:
            pass
    SEARCH_REGION = None


def save_search_region(left, top, width, height):
    config = configparser.ConfigParser()
    if os.path.exists(CONFIG_FILE):
        config.read(CONFIG_FILE, encoding="utf-8")
    if "SearchRegion" not in config:
        config["SearchRegion"] = {}
    config["SearchRegion"]["Left"] = str(left)
    config["SearchRegion"]["Top"] = str(top)
    config["SearchRegion"]["Width"] = str(width)
    config["SearchRegion"]["Height"] = str(height)
    with open(CONFIG_FILE, "w", encoding="utf-8") as f:
        config.write(f)


def open_region_settings():
    global settings_window, SEARCH_REGION
    if settings_window is not None and settings_window.winfo_exists():
        settings_window.deiconify()
        settings_window.lift()
        return

    settings_window = tk.Toplevel(root)
    settings_window.title("设置识别区域")
    settings_window.geometry("360x180")
    settings_window.attributes("-topmost", True)
    settings_window.resizable(False, False)

    def format_region(region):
        if not region:
            return "当前: 全屏"
        left, top, width, height = region
        return f"当前: {left},{top}  宽度={width}  高度={height}"

    def update_region_label():
        region_label.config(text=format_region(SEARCH_REGION))

    def start_region_selection():
        settings_window.withdraw()
        overlay = tk.Toplevel(root)
        overlay.attributes("-fullscreen", True)
        overlay.attributes("-topmost", True)
        overlay.overrideredirect(True)
        overlay.attributes("-alpha", 0.25)
        overlay.configure(bg="black")

        canvas = tk.Canvas(overlay, bg="black", highlightthickness=0, cursor="cross")
        canvas.pack(fill="both", expand=True)

        selection = {"x1": None, "y1": None, "rect": None}

        def on_press(event):
            selection["x1"] = event.x
            selection["y1"] = event.y
            selection["rect"] = canvas.create_rectangle(event.x, event.y, event.x, event.y, outline="red", width=3)

        def on_drag(event):
            if selection["rect"] is not None:
                canvas.coords(selection["rect"], selection["x1"], selection["y1"], event.x, event.y)

        def on_release(event):
            global SEARCH_REGION
            x1 = min(selection["x1"], event.x)
            y1 = min(selection["y1"], event.y)
            x2 = max(selection["x1"], event.x)
            y2 = max(selection["y1"], event.y)
            width = max(1, x2 - x1)
            height = max(1, y2 - y1)
            SEARCH_REGION = (x1, y1, width, height)
            save_search_region(x1, y1, width, height)
            print(f"[INFO] saved search region: {SEARCH_REGION}")
            overlay.destroy()
            settings_window.deiconify()
            update_region_label()

        def on_cancel(event=None):
            overlay.destroy()
            settings_window.deiconify()

        canvas.bind("<ButtonPress-1>", on_press)
        canvas.bind("<B1-Motion>", on_drag)
        canvas.bind("<ButtonRelease-1>", on_release)
        overlay.bind("<Escape>", on_cancel)
        overlay.focus_set()
        overlay.grab_set()

    def reset_to_fullscreen():
        global SEARCH_REGION
        SEARCH_REGION = None
        save_search_region(0, 0, A_ScreenWidth, A_ScreenHeight)
        update_region_label()
        print("[INFO] reset search region to full screen")

    region_label = tk.Label(settings_window, text=format_region(SEARCH_REGION), anchor="w")
    region_label.place(x=12, y=20, width=336, height=24)

    tk.Label(settings_window, text="请点击“框选区域”，然后在屏幕上拖动框选要识别的目标区域。按 Esc 取消。", fg="#444444", wraplength=336, justify="left").place(x=12, y=52)

    select_button = tk.Button(settings_window, text="框选区域", width=12, command=start_region_selection)
    select_button.place(x=40, y=110)
    fullscreen_button = tk.Button(settings_window, text="全屏识别", width=12, command=reset_to_fullscreen)
    fullscreen_button.place(x=180, y=110)
    close_button = tk.Button(settings_window, text="关闭", width=12, command=settings_window.destroy)
    close_button.place(x=110, y=150)


class Timer:
    def __init__(self, timer_id, root_window):
        self.id = timer_id
        self.root = root_window
        self.active = False
        self.remain = 0.0
        self.flash_active = False
        self.flash_state = False
        self.x = 0
        self.y = 0
        self._drag_start = None
        self._window_start = (0, 0)
        self._last_move_time = 0.0

        timers.append(self)
        self.calculate_position()
        self.create_gui()
        self.hide()

    def calculate_position(self):
        pos = load_timer_pos(self.id)
        if pos:
            self.x, self.y = pos
            return

        occupied = [(t.y, WINDOW_HEIGHT) for t in timers if t is not self and t.y]
        step = WINDOW_HEIGHT + 4
        found = False
        candidate_y = 8
        attempts = 0
        while not found and attempts < 30:
            overlap = any((candidate_y + WINDOW_HEIGHT > oy) and (candidate_y < oy + h) for oy, h in occupied)
            if not overlap:
                found = True
                self.y = candidate_y
            else:
                candidate_y += step
                if candidate_y + WINDOW_HEIGHT > A_ScreenHeight:
                    candidate_y = 8
                attempts += 1

        if not found:
            self.y = max(0, A_ScreenHeight - WINDOW_HEIGHT - 8)

        self.x = max(0, A_ScreenWidth - WINDOW_WIDTH - 8)

    def create_gui(self):
        self.win = tk.Toplevel(self.root)
        self.win.overrideredirect(True)
        self.win.attributes("-topmost", True)
        try:
            # Windows: -transparentcolor 与整体 alpha 通常不能同时作用；
            # 保持 alpha=1.0 以便 -transparentcolor 生效
            self.win.attributes("-alpha", 1.0)
            self.win.attributes("-transparentcolor", BG_COLOR)
        except Exception:
            pass
        self.win.configure(bg=BG_COLOR)
        self.win.geometry(f"{WINDOW_WIDTH}x{WINDOW_HEIGHT}+{self.x}+{self.y}")

        self.display_canvas = tk.Canvas(self.win, width=WINDOW_WIDTH, height=WINDOW_HEIGHT,
                                        bd=0, highlightthickness=0, relief="flat", bg=BG_COLOR)
        self.display_canvas.place(x=0, y=0)

        self.icon_item = self.display_canvas.create_image(WINDOW_WIDTH // 2, 8 + ICON_SIZE // 2, image=None)
        text_y = ICON_SIZE + 12 + (WINDOW_HEIGHT - ICON_SIZE - 16) // 2
        self.count_text_item = self.display_canvas.create_text(
            WINDOW_WIDTH // 2,
            text_y,
            text="待机",
            font=(FONT_NAME, 14),
            fill=FONT_COLOR,
        )

        self.base_icon = get_base_icon()
        self.icon_image = None
        self.current_progress_step = None
        self.last_display_seconds = None
        self.last_display_text = None
        self.draw_icon(1.0)

        self.display_canvas.configure(cursor="fleur")
        self.display_canvas.bind("<ButtonPress-1>", self._start_move)
        self.display_canvas.bind("<B1-Motion>", self._on_move)
        self.display_canvas.bind("<ButtonRelease-1>", self._save_position)

    def load_skill_icon(self):
        return get_base_icon()

    def draw_icon(self, progress_ratio):
        try:
            progress_step = int(progress_ratio * CACHED_ICON_STEPS)
            if progress_step == self.current_progress_step:
                return
            self.current_progress_step = progress_step
            self.icon_image = get_icon_frame(progress_step)
            self.display_canvas.itemconfig(self.icon_item, image=self.icon_image)
        except Exception as e:
            print(f"[WARN] draw_icon failed: {e}")

    def _start_move(self, event):
        self._drag_start = (event.x_root, event.y_root)
        self._window_start = (self.x, self.y)

    def _on_move(self, event):
        if self._drag_start:
            now = time.time()
            if now - self._last_move_time < 0.032:
                return
            self._last_move_time = now
            dx = event.x_root - self._drag_start[0]
            dy = event.y_root - self._drag_start[1]
            self.x = self._window_start[0] + dx
            self.y = self._window_start[1] + dy
            self.x = max(0, min(self.x, A_ScreenWidth - WINDOW_WIDTH))
            self.y = max(0, min(self.y, A_ScreenHeight - WINDOW_HEIGHT))
            self.win.geometry(f"{WINDOW_WIDTH}x{WINDOW_HEIGHT}+{self.x}+{self.y}")

    def _save_position(self, event=None):
        save_timer_pos(self.id, self.x, self.y)

    def show(self):
        self.win.deiconify()
        self.update_display()
        if self.flash_active:
            self.flash_state = False
            self.apply_flash_state()

    def hide(self):
        self.win.withdraw()

    def toggle(self):
        if self.win.state() == "withdrawn":
            self.show()
        else:
            self.hide()

    def start(self):
        if self.active:
            return
        self.active = True
        self.remain = COUNTDOWN_SEC
        self.flash_active = False
        self.flash_state = False
        self.show()
        self.update_display()

    def update_display(self):
        if self.remain > 0:
            seconds = math.ceil(self.remain)
            if seconds != self.last_display_seconds:
                self.display_canvas.itemconfig(self.count_text_item, text=str(seconds))
                self.last_display_seconds = seconds
                self.last_display_text = None
            self.draw_icon(self.remain / COUNTDOWN_SEC)
        else:
            if self.last_display_text != "就绪":
                self.display_canvas.itemconfig(self.count_text_item, text="就绪")
                self.last_display_text = "就绪"
                self.last_display_seconds = None
            self.draw_icon(0.0)

    def apply_flash_state(self):
        color = FLASH_FONT_COLOR if self.flash_state else FONT_COLOR
        self.display_canvas.itemconfig(self.count_text_item, fill=color)

    def reset(self):
        self.active = False
        self.remain = 0.0
        self.flash_active = False
        self.flash_state = False
        self.update_display()
        self.hide()


def create_new_timer():
    global timer_counter
    timer_counter += 1
    t = Timer(timer_counter, root)
    t.start()
    return t


def load_trigger_images():
    global TRIGGER_IMAGES
    TRIGGER_IMAGES = []
    for image_path in TRIGGER_IMAGE_LIST:
        if not os.path.exists(image_path):
            print(f"[WARN] trigger image not found: {image_path}")
            continue
        try:
            image_obj = Image.open(image_path).convert("RGB")
            TRIGGER_IMAGES.append((image_path, image_obj))
            print(f"[INFO] loaded trigger image: {image_path}")
        except Exception as e:
            print(f"[WARN] failed to open trigger image {image_path}: {e}")
    if not TRIGGER_IMAGES:
        print("[WARN] no trigger images loaded; check files and folder path.")


def image_to_gray_array(image_obj):
    try:
        return cv2.cvtColor(np.array(image_obj), cv2.COLOR_RGB2GRAY)
    except Exception:
        return np.array(image_obj.convert("L"), dtype=np.uint8)


def find_image_with_opencv(image_obj, region=None, threshold=0.55):
    try:
        screenshot = pyautogui.screenshot(region=region) if region else pyautogui.screenshot()
        screen_gray = cv2.cvtColor(np.array(screenshot), cv2.COLOR_RGB2GRAY)
        template = image_to_gray_array(image_obj)
        if screen_gray.shape[0] < template.shape[0] or screen_gray.shape[1] < template.shape[1]:
            return None

        if template.shape[0] <= 16 and template.shape[1] <= 16:
            return find_image_with_opencv_small(image_obj, region=region)

        candidates = []
        candidates.append((screen_gray, template, threshold))

        if template.shape[0] > 8 and template.shape[1] > 8:
            screen_edges = cv2.Canny(screen_gray, 50, 150)
            template_edges = cv2.Canny(template, 50, 150)
            candidates.append((screen_edges, template_edges, min(threshold + 0.1, 0.6)))

        for target_img, target_template, thresh in candidates:
            methods = [
                (cv2.TM_CCOEFF_NORMED, thresh),
                (cv2.TM_CCORR_NORMED, max(thresh - 0.15, 0.35)),
                (cv2.TM_SQDIFF_NORMED, 1.0 - max(thresh, 0.4)),
            ]
            for method, match_thresh in methods:
                result = cv2.matchTemplate(target_img, target_template, method)
                min_val, max_val, min_loc, max_loc = cv2.minMaxLoc(result)
                if method == cv2.TM_SQDIFF_NORMED:
                    matched = min_val <= match_thresh
                    match_loc = min_loc
                else:
                    matched = max_val >= match_thresh
                    match_loc = max_loc
                if matched:
                    left = match_loc[0] + (region[0] if region else 0)
                    top = match_loc[1] + (region[1] if region else 0)
                    return left, top, template.shape[1], template.shape[0]
    except Exception as e:
        print(f"[DEBUG] OpenCV match failed: {e}")
    return None


def find_image_with_opencv_small(image_obj, region=None):
    try:
        screenshot = pyautogui.screenshot(region=region) if region else pyautogui.screenshot()
        screen_gray = cv2.cvtColor(np.array(screenshot), cv2.COLOR_RGB2GRAY)
        screen_color = np.array(screenshot)
        template_gray = image_to_gray_array(image_obj)
        if screen_gray.shape[0] < template_gray.shape[0] or screen_gray.shape[1] < template_gray.shape[1]:
            return None

        screen_gray = cv2.GaussianBlur(screen_gray, (3, 3), 0)
        template_gray = cv2.GaussianBlur(template_gray, (3, 3), 0)

        variants = [template_gray]
        if template_gray.shape[0] >= 6 and template_gray.shape[1] >= 6:
            variants.append(cv2.resize(template_gray, None, fx=2.0, fy=2.0, interpolation=cv2.INTER_CUBIC))

        for tmpl in variants:
            if screen_gray.shape[0] < tmpl.shape[0] or screen_gray.shape[1] < tmpl.shape[1]:
                continue
            for method, thresh in [
                (cv2.TM_CCOEFF_NORMED, 0.45),
                (cv2.TM_CCORR_NORMED, 0.42),
                (cv2.TM_SQDIFF_NORMED, 0.55),
            ]:
                result = cv2.matchTemplate(screen_gray, tmpl, method)
                min_val, max_val, min_loc, max_loc = cv2.minMaxLoc(result)
                if method == cv2.TM_SQDIFF_NORMED:
                    matched = min_val <= thresh
                    match_loc = min_loc
                    score = min_val
                else:
                    matched = max_val >= thresh
                    match_loc = max_loc
                    score = max_val
                if TEMPLATE_MATCH_DEBUG:
                    print(f"[DEBUG] small match method={method} thresh={thresh} score={score} loc={match_loc}")
                if matched:
                    left = match_loc[0] + (region[0] if region else 0)
                    top = match_loc[1] + (region[1] if region else 0)
                    return left, top, tmpl.shape[1], tmpl.shape[0]

        if image_obj.mode == "RGB" and region is not None:
            template_color = np.array(image_obj)
            for channel in range(3):
                screen_chan = screen_color[:, :, channel]
                tmpl_chan = template_color[:, :, channel]
                if screen_chan.shape[0] < tmpl_chan.shape[0] or screen_chan.shape[1] < tmpl_chan.shape[1]:
                    continue
                result = cv2.matchTemplate(screen_chan, tmpl_chan, cv2.TM_CCOEFF_NORMED)
                _, max_val, _, max_loc = cv2.minMaxLoc(result)
                if TEMPLATE_MATCH_DEBUG:
                    print(f"[DEBUG] color channel {channel} score={max_val} loc={max_loc}")
                if max_val >= 0.42:
                    left = max_loc[0] + region[0]
                    top = max_loc[1] + region[1]
                    return left, top, tmpl_chan.shape[1], tmpl_chan.shape[0]
    except Exception as e:
        print(f"[DEBUG] OpenCV tiny match failed: {e}")
    return None


def check_image():
    global TRIGGER_INITIALIZED, TRIGGER_STATE
    grid_size = 20
    if IMAGE_TOLERANCE >= 70:
        confidence_values = [0.6, 0.5, 0.4]
    else:
        confidence_values = [0.75, 0.65, 0.55]

    current_state = {}
    region_arg = SEARCH_REGION if SEARCH_REGION else None
    for image_path, image_obj in TRIGGER_IMAGES:
        found = None
        image_area = image_obj.width * image_obj.height
        thresholds = [0.7, 0.6, 0.55, 0.5] if image_area >= 64 else [0.6, 0.55, 0.5, 0.45]

        for confidence in confidence_values:
            try:
                if region_arg:
                    found = pyautogui.locateOnScreen(image_obj, confidence=confidence, grayscale=True, region=region_arg)
                else:
                    found = pyautogui.locateOnScreen(image_obj, confidence=confidence, grayscale=True)
            except Exception:
                try:
                    if region_arg:
                        found = pyautogui.locateOnScreen(image_obj, region=region_arg)
                    else:
                        found = pyautogui.locateOnScreen(image_obj)
                except Exception:
                    found = None
            if found:
                print(f"[DEBUG] matched {image_path} at {found.left},{found.top} confidence={confidence}")
                break

        if not found:
            for threshold in thresholds:
                found_rect = find_image_with_opencv(image_obj, region=region_arg, threshold=threshold)
                if found_rect:
                    left, top, width, height = found_rect
                    found = type("Match", (), {"left": left, "top": top, "width": width, "height": height})()
                    print(f"[DEBUG] opencv matched {image_path} at {left},{top} threshold={threshold}")
                    break

        current_state[image_path] = bool(found)
        if found:
            previous_visible = TRIGGER_STATE.get(image_path, False)
            if TRIGGER_INITIALIZED and not previous_visible:
                grid_x = (found.left // grid_size) * grid_size
                grid_y = (found.top // grid_size) * grid_size
                key = f"{image_path}|{grid_x}|{grid_y}"
                if key not in triggered_images:
                    triggered_images.add(key)
                    create_new_timer()

    TRIGGER_STATE.update(current_state)
    if not TRIGGER_INITIALIZED:
        TRIGGER_INITIALIZED = True

    root.after(CHECK_INTERVAL, check_image)


def global_tick():
    global g_last_tick_time, g_tick_count
    now = time.time()
    if g_last_tick_time is None:
        g_last_tick_time = now

    elapsed = now - g_last_tick_time
    if elapsed >= 0.1:
        delta = elapsed
        g_last_tick_time = now
        g_tick_count += 1
        for t in list(timers):
            if not t.active or t.remain <= 0:
                continue
            previous_seconds = math.ceil(t.remain)
            t.remain = max(0.0, t.remain - delta)
            visible = t.win.state() != "withdrawn"
            int_remain = math.ceil(t.remain)
            if 0 < int_remain <= FLASH_START_SECONDS:
                if not t.flash_active:
                    t.flash_active = True
                    t.flash_state = False
                if (visible or PLAY_SOUND_WHEN_HIDDEN) and int_remain % 2 == 0 and g_tick_count % 10 == 0:
                    play_sound(800, 100)
            elif t.flash_active:
                t.flash_active = False
                t.flash_state = False
                t.apply_flash_state()
            if 0 < int_remain <= BEEP_LAST_SECONDS and (visible or PLAY_SOUND_WHEN_HIDDEN):
                play_sound(1000, 150)
            if t.remain <= 0:
                t.active = False
                t.flash_active = False
                t.last_display_text = None
                t.last_display_seconds = None
                if visible or PLAY_SOUND_WHEN_HIDDEN:
                    play_sound(880, 300)
                t.win.after(2000, t.hide)
                t.update_display()
            elif int_remain != previous_seconds:
                t.update_display()
        if g_tick_count % 6 == 0:
            for t in timers:
                if t.flash_active and t.active:
                    t.flash_state = not t.flash_state
                    t.apply_flash_state()
    root.after(GLOBAL_UPDATE_INTERVAL, global_tick)


def reset_all_timers():
    global timers, triggered_images
    for t in timers:
        t.reset()
    timers = []
    triggered_images.clear()


def show_all_timers():
    for t in timers:
        t.show()


def hide_all_timers():
    for t in timers:
        t.hide()


def toggle_all_timers():
    for t in timers:
        t.toggle()


def on_exit(icon=None, item=None):
    for t in timers:
        save_timer_pos(t.id, t.x, t.y)
    if icon:
        icon.stop()
    root.quit()


def create_tray_icon():
    global tray_icon
    image = Image.new("RGB", (64, 64), BG_COLOR)
    draw = ImageDraw.Draw(image)
    draw.rectangle((8, 8, 56, 56), outline=PROGRESS_COLOR, width=4)
    tray_icon = pystray.Icon(
        "CoolDownTimer",
        image,
        "多计时器",
        menu=(
            item("显示所有", lambda icon, item: show_all_timers()),
            item("隐藏所有", lambda icon, item: hide_all_timers()),
            item("重置所有", lambda icon, item: reset_all_timers()),
            item("设置识别区域", lambda icon, item: root.after(0, open_region_settings)),
            item("退出", on_exit),
        ),
    )
    tray_icon.run()


def bind_hotkeys():
    keyboard.add_hotkey("f2", lambda: create_new_timer())
    keyboard.add_hotkey("f3", lambda: reset_all_timers())
    keyboard.add_hotkey("f4", lambda: toggle_all_timers())


def main():
    global root
    root = tk.Tk()
    root.withdraw()
    load_search_region()
    load_trigger_images()
    bind_hotkeys()

    tray_thread = threading.Thread(target=create_tray_icon, daemon=True)
    tray_thread.start()
    print("多计时器已启动，检测图片中... F2=新建 F3=重置 F4=切换显隐")

    root.after(CHECK_INTERVAL, check_image)
    root.after(GLOBAL_UPDATE_INTERVAL, global_tick)
    root.protocol("WM_DELETE_WINDOW", on_exit)
    root.mainloop()


if __name__ == "__main__":
    main()
