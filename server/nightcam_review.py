#!/usr/bin/env python3
"""
nightcam_review.py — Desktop review tool for nightcam data on the N100.

Browse Stack JPEGs and FF binary files captured by the meteor camera.

Keyboard shortcuts:
  Right/Down       Next file
  Left/Up          Previous file
  Page Down        Next night
  Page Up          Previous night
  Home / End       First / last file in night
  Tab              Toggle Stacks ↔ FF mode
  1–5              FF plane: max / ave / frame / std / detection
  t                Start timelapse (Stacks or FF mode)
  Escape           Cancel timelapse
  Delete           Delete current file (with confirm)
  Ctrl+d           Delete entire night directory (with confirm)
  F5               Reload
  q                Quit

Usage:
    python3 server/nightcam_review.py [--data-dir ~/RMS_data]
"""

import argparse
import glob
import os
import re
import shutil
import struct
import subprocess
import tempfile
import threading
import tkinter as tk
import tkinter.ttk as _tkttk
from collections import OrderedDict
from tkinter import messagebox, simpledialog

import numpy as np
import ttkbootstrap as ttk
from PIL import Image, ImageTk

# ---------------------------------------------------------------------------
# FF binary constants
# ---------------------------------------------------------------------------

FF_HEADER_FMT  = "<i8I"   # int32 + 8×uint32 = 36 bytes
FF_HEADER_SIZE = struct.calcsize(FF_HEADER_FMT)  # 36

PLANE_NAMES = {
	1: "maxpixel",
	2: "avepixel",
	3: "maxframe",
	4: "stdpixel",
	5: "detection",
}

# ---------------------------------------------------------------------------
# FFFile — parse one RMS FF binary file
# ---------------------------------------------------------------------------

class FFFile:
	"""Parse one RMS FF binary file (version 2, little-endian)."""

	def __init__(self, path: str):
		self.path = path
		with open(path, "rb") as fh:
			raw = fh.read()
		fields = struct.unpack_from(FF_HEADER_FMT, raw, 0)
		(self.version, self.nrows, self.ncols, self.nframes,
		 self.first, self.camno, self.decimation, self.interleave,
		 self.fps_milli) = fields
		if self.version != -1:
			raise ValueError(f"Unexpected FF version marker: {self.version}")
		P = int(self.nrows) * int(self.ncols)
		off = FF_HEADER_SIZE
		self._maxpixel = np.frombuffer(raw, dtype=np.uint8, count=P,
		                               offset=off).reshape(self.nrows, self.ncols)
		off += P
		self._maxframe = np.frombuffer(raw, dtype=np.uint8, count=P,
		                               offset=off).reshape(self.nrows, self.ncols)
		off += P
		self._avepixel = np.frombuffer(raw, dtype=np.uint8, count=P,
		                               offset=off).reshape(self.nrows, self.ncols)
		off += P
		self._stdpixel = np.frombuffer(raw, dtype=np.uint8, count=P,
		                               offset=off).reshape(self.nrows, self.ncols)

	def get_plane(self, plane_id: int) -> np.ndarray:
		"""Return uint8 array for the requested plane (1–5)."""
		if plane_id == 1:
			return self._maxpixel
		if plane_id == 2:
			return self._avepixel
		if plane_id == 3:
			return self._maxframe
		if plane_id == 4:
			return self._stdpixel
		if plane_id == 5:
			return self.detection_plane()
		raise ValueError(f"Invalid plane_id: {plane_id}")

	def detection_plane(self) -> np.ndarray:
		"""Amplified difference maxpixel − avepixel (×3, clipped 0–255)."""
		diff = self._maxpixel.astype(np.int16) - self._avepixel.astype(np.int16)
		return np.clip(diff * 3, 0, 255).astype(np.uint8)

	def to_pil_image(self, plane_id: int) -> Image.Image:
		return Image.fromarray(self.get_plane(plane_id), "L").convert("RGB")


# ---------------------------------------------------------------------------
# ThumbnailCache — simple OrderedDict LRU, max 200 entries
# ---------------------------------------------------------------------------

class ThumbnailCache:
	def __init__(self, maxsize: int = 200):
		self._cache: OrderedDict = OrderedDict()
		self._maxsize = maxsize

	def get(self, key: str):
		if key in self._cache:
			self._cache.move_to_end(key)
			return self._cache[key]
		return None

	def put(self, key: str, value):
		if key in self._cache:
			self._cache.move_to_end(key)
		else:
			if len(self._cache) >= self._maxsize:
				self._cache.popitem(last=False)
			self._cache[key] = value


# ---------------------------------------------------------------------------
# TimelapseWorker — ffmpeg timelapse in a background thread
# ---------------------------------------------------------------------------

class TimelapseWorker(threading.Thread):
	"""Build a timelapse MP4 from a list of JPEG or FF binary files using ffmpeg."""

	def __init__(self, files: list, night_dir: str, night_name: str,
	             fps: int, root, on_done, on_progress, ff_plane: int = 0):
		super().__init__(daemon=True)
		self.files       = files
		self.night_dir   = night_dir
		self.night_name  = night_name
		self.fps         = fps
		self.root        = root
		self.on_done     = on_done
		self.on_progress = on_progress
		self.ff_plane    = ff_plane   # 0 = files are already JPEGs
		self._cancelled  = threading.Event()

	def cancel(self):
		self._cancelled.set()

	def run(self):
		filelist_path = None
		tmpdir        = None
		proc          = None
		try:
			jpeg_files = self.files

			# FF mode: decode each binary to a temporary JPEG first.
			if self.ff_plane:
				tmpdir = tempfile.mkdtemp(prefix="nightcam_tl_")
				jpeg_files = []
				total = len(self.files)
				for i, ff_path in enumerate(self.files):
					if self._cancelled.is_set():
						self.root.after(0, lambda: self.on_done(None, "Cancelled"))
						return
					ff  = FFFile(ff_path)
					img = ff.to_pil_image(self.ff_plane)
					out = os.path.join(tmpdir, f"frame_{i:05d}.jpg")
					img.save(out, "JPEG", quality=95)
					jpeg_files.append(out)
					pct = int((i + 1) / total * 100)
					self.root.after(0,
					                lambda p=pct, t=total: self.on_progress(
					                    f"Converting FF frames: {p}% ({t} total)"))

			# Write concat filelist
			with tempfile.NamedTemporaryFile(mode="w", suffix=".txt",
			                                delete=False) as fh:
				filelist_path = fh.name
				frame_dur = 1.0 / max(1, self.fps)
				for f in jpeg_files:
					fh.write(f"file '{f}'\n")
					fh.write(f"duration {frame_dur:.6f}\n")
				# The concat demuxer requires a trailing file entry
				# WITHOUT a duration line so the last frame is shown;
				# without it the last frame gets zero duration and
				# ffmpeg may error out or drop it.
				if jpeg_files:
					fh.write(f"file '{jpeg_files[-1]}'\n")

			output_path = os.path.join(
				self.night_dir,
				f"timelapse_{self.night_name.replace('/', '_')}.mp4"
			)
			cmd = [
				"ffmpeg", "-y", "-f", "concat", "-safe", "0",
				"-i", filelist_path,
				"-c:v", "libx264", "-preset", "fast",
				"-crf", "23", "-pix_fmt", "yuv420p",
				output_path,
			]
			proc = subprocess.Popen(
				cmd,
				stdout=subprocess.DEVNULL,
				stderr=subprocess.PIPE,
				text=True,
			)
			stderr_lines = []
			for line in proc.stderr:
				stderr_lines.append(line.rstrip())
				if self._cancelled.is_set():
					proc.terminate()
					self.root.after(0, lambda: self.on_done(None, "Cancelled"))
					return
				if "frame=" in line or "fps=" in line:
					stripped = line.strip()
					self.root.after(0,
					                lambda s=stripped: self.on_progress(s))
			proc.wait()
			if proc.returncode == 0:
				self.root.after(0, lambda: self.on_done(output_path, None))
			else:
				print("=== ffmpeg stderr ===", flush=True)
				for sl in stderr_lines:
					print(sl, flush=True)
				print("=== end ffmpeg stderr ===", flush=True)
				self.root.after(0,
				                lambda: self.on_done(
				                    None,
				                    f"ffmpeg error (code {proc.returncode})",
				                ))
		except FileNotFoundError:
			self.root.after(0, lambda: self.on_done(
				None, "ffmpeg not found — install with: apt install ffmpeg"
			))
		except Exception as exc:  # pylint: disable=broad-except
			msg = str(exc)
			self.root.after(0, lambda: self.on_done(None, msg))
		finally:
			if filelist_path:
				try:
					os.unlink(filelist_path)
				except OSError:
					pass
			if tmpdir:
				shutil.rmtree(tmpdir, ignore_errors=True)


# ---------------------------------------------------------------------------
# Night directory pattern
# ---------------------------------------------------------------------------

NIGHT_RE = re.compile(r"^\d{8}_\d{6}_\d{6}$")

# ---------------------------------------------------------------------------
# NightcamReview — main application class
# ---------------------------------------------------------------------------

class NightcamReview:
	DARK_BG    = "#1a1a1a"
	LEFT_MIN_W = 280

	def __init__(self, root: ttk.Window, data_dir: str):
		self.root       = root
		self.data_dir   = data_dir
		self.stacks_root = os.path.join(data_dir, "Stacks")
		self.ff_root     = os.path.join(data_dir, "CapturedFiles")

		self._mode           = "stacks"
		self._ff_plane       = 1
		self._current_path   = None
		self._current_pil    = None
		self._canvas_img_ref = None   # prevents GC of ImageTk.PhotoImage
		self._nights_list    = []
		self._files_list     = []
		self._tl_worker      = None
		self._cache          = ThumbnailCache()

		self._build_ui()
		self._bind_keys()
		# Reflect initial mode in toolbar
		self._btn_stacks.configure(bootstyle="primary")
		self._btn_ff.configure(bootstyle="secondary")
		self._reload()

	# -----------------------------------------------------------------------
	# UI construction
	# -----------------------------------------------------------------------

	def _build_ui(self):
		self.root.title("Nightcam Review")
		self.root.configure(bg=self.DARK_BG)

		# -- Toolbar ---------------------------------------------------------
		tb = ttk.Frame(self.root, bootstyle="dark")
		tb.pack(side=tk.TOP, fill=tk.X)

		self._btn_stacks = ttk.Button(
			tb, text="Stacks [Tab]", bootstyle="secondary",
			command=lambda: self._switch_mode("stacks"),
		)
		self._btn_stacks.pack(side=tk.LEFT, padx=2, pady=2)

		self._btn_ff = ttk.Button(
			tb, text="FF Files [Tab]", bootstyle="secondary",
			command=lambda: self._switch_mode("ff"),
		)
		self._btn_ff.pack(side=tk.LEFT, padx=2, pady=2)

		ttk.Button(
			tb, text="Reload [F5]", bootstyle="info-outline",
			command=self._reload,
		).pack(side=tk.LEFT, padx=2, pady=2)

		self._btn_tl = ttk.Button(
			tb, text="Timelapse [t]", bootstyle="success-outline",
			command=self._start_timelapse,
		)
		self._btn_tl.pack(side=tk.LEFT, padx=2, pady=2)

		ttk.Button(
			tb, text="Delete [Del]", bootstyle="danger-outline",
			command=self._delete_current,
		).pack(side=tk.LEFT, padx=2, pady=2)

		ttk.Button(
			tb, text="Del Night [Ctrl+d]", bootstyle="danger",
			command=self._delete_night,
		).pack(side=tk.LEFT, padx=2, pady=2)

		# -- Main paned window (horizontal) ----------------------------------
		self._paned = _tkttk.PanedWindow(self.root, orient=tk.HORIZONTAL)
		self._paned.pack(fill=tk.BOTH, expand=True)

		# Left panel
		left = ttk.Frame(self._paned, width=self.LEFT_MIN_W)
		left.pack_propagate(False)
		self._paned.add(left, weight=0)

		# Night dirs listbox — fixed height; no PanedWindow because ttk
		# PanedWindow starts with the sash at 0 making the top pane invisible.
		nights_frame = _tkttk.LabelFrame(left, text="Night")
		nights_frame.pack(side=tk.TOP, fill=tk.X, padx=2, pady=(4, 2))

		nights_scroll = ttk.Scrollbar(nights_frame, orient=tk.VERTICAL)
		self._nights_lb = tk.Listbox(
			nights_frame,
			height=8,
			bg="#222", fg="#ccc", selectbackground="#446",
			yscrollcommand=nights_scroll.set,
			font=("Monospace", 9),
			exportselection=False,
		)
		nights_scroll.config(command=self._nights_lb.yview)
		nights_scroll.pack(side=tk.RIGHT, fill=tk.Y)
		self._nights_lb.pack(fill=tk.BOTH, expand=True)
		self._nights_lb.bind("<<ListboxSelect>>", self._on_night_select)

		# Files listbox — fills remaining left-panel space
		files_frame = _tkttk.LabelFrame(left, text="Files")
		files_frame.pack(side=tk.TOP, fill=tk.BOTH, expand=True,
		                 padx=2, pady=(2, 4))

		files_scroll = ttk.Scrollbar(files_frame, orient=tk.VERTICAL)
		self._files_lb = tk.Listbox(
			files_frame,
			bg="#222", fg="#ccc", selectbackground="#446",
			yscrollcommand=files_scroll.set,
			font=("Monospace", 8),
			exportselection=False,
		)
		files_scroll.config(command=self._files_lb.yview)
		files_scroll.pack(side=tk.RIGHT, fill=tk.Y)
		self._files_lb.pack(fill=tk.BOTH, expand=True)
		self._files_lb.bind("<<ListboxSelect>>", self._on_file_select)

		# Canvas
		self._canvas = tk.Canvas(self._paned, bg=self.DARK_BG,
		                         highlightthickness=0)
		self._paned.add(self._canvas, weight=1)
		self._canvas.bind("<Configure>", self._on_canvas_resize)

		# Status bar
		self._status_var = tk.StringVar(value="Ready")
		tk.Label(
			self.root, textvariable=self._status_var,
			font=("Monospace", 9), fg="#dddddd", bg="#2a2a2a", anchor=tk.W,
		).pack(side=tk.BOTTOM, fill=tk.X, padx=4, pady=2)

	# -----------------------------------------------------------------------
	# Key bindings
	# -----------------------------------------------------------------------

	def _bind_keys(self):
		r = self.root
		r.bind("<Right>",         lambda e: self._step_file(+1))
		r.bind("<Down>",          lambda e: self._step_file(+1))
		r.bind("<Left>",          lambda e: self._step_file(-1))
		r.bind("<Up>",            lambda e: self._step_file(-1))
		r.bind("<Next>",          lambda e: self._step_night(+1))   # Page Down
		r.bind("<Control-Right>", lambda e: self._step_night(+1))
		r.bind("<Prior>",         lambda e: self._step_night(-1))   # Page Up
		r.bind("<Control-Left>",  lambda e: self._step_night(-1))
		r.bind("<Home>",          lambda e: self._jump_file(0))
		r.bind("<End>",           lambda e: self._jump_file(-1))
		r.bind("<Tab>",           lambda e: self._toggle_mode())
		r.bind("1",               lambda e: self._set_ff_plane(1))
		r.bind("2",               lambda e: self._set_ff_plane(2))
		r.bind("3",               lambda e: self._set_ff_plane(3))
		r.bind("4",               lambda e: self._set_ff_plane(4))
		r.bind("5",               lambda e: self._set_ff_plane(5))
		r.bind("t",               lambda e: self._start_timelapse())
		r.bind("<Escape>",        lambda e: self._cancel_timelapse())
		r.bind("<Delete>",        lambda e: self._delete_current())
		r.bind("<Control-d>",     lambda e: self._delete_night())
		r.bind("<F5>",            lambda e: self._reload())
		r.bind("q",               lambda e: self.root.destroy())

	# -----------------------------------------------------------------------
	# Mode switching
	# -----------------------------------------------------------------------

	def _switch_mode(self, mode: str):
		if mode == self._mode:
			return
		self._mode = mode
		self._current_path = None
		self._current_pil  = None
		self._canvas.delete("all")
		if mode == "stacks":
			self._btn_stacks.configure(bootstyle="primary")
			self._btn_ff.configure(bootstyle="secondary")
		else:
			self._btn_stacks.configure(bootstyle="secondary")
			self._btn_ff.configure(bootstyle="primary")
		self._scan_nights()

	def _toggle_mode(self):
		self._switch_mode("ff" if self._mode == "stacks" else "stacks")

	# -----------------------------------------------------------------------
	# Directory scanning
	# -----------------------------------------------------------------------

	def _scan_nights(self):
		"""
		Scan for night directories under the mode root, supporting both:
		  - New layout: <root>/<station>/<night>/   (multi-camera)
		  - Old layout: <root>/<night>/             (single-camera, backward compat)
		Entries in _nights_list are stored as 'station/night' or plain 'night'.
		"""
		root_dir = self.stacks_root if self._mode == "stacks" else self.ff_root
		try:
			entries = os.listdir(root_dir)
		except OSError:
			entries = []

		nights = []
		for entry in entries:
			if NIGHT_RE.match(entry):
				# Old flat layout
				nights.append(entry)
			else:
				# Possible station directory — scan inside for night dirs
				sub = os.path.join(root_dir, entry)
				if not os.path.isdir(sub):
					continue
				try:
					for night in os.listdir(sub):
						if NIGHT_RE.match(night):
							nights.append(f"{entry}/{night}")
				except OSError:
					pass

		nights = sorted(nights, reverse=True)
		self._nights_list = nights
		self._nights_lb.delete(0, tk.END)
		for n in nights:
			# Show 'STATION  /  NIGHT' or plain 'NIGHT'
			label = n.replace("/", "  /  ") if "/" in n else n
			self._nights_lb.insert(tk.END, label)

		if nights:
			self._nights_lb.selection_set(0)
			self._nights_lb.activate(0)
			self._load_file_list(nights[0])
		else:
			self._files_lb.delete(0, tk.END)
			self._files_list = []
			self._status_var.set(f"No night dirs in {root_dir}")

	def _night_dir_path(self, night_key: str) -> str:
		"""Resolve a nights-list key to an absolute directory path."""
		root_dir = self.stacks_root if self._mode == "stacks" else self.ff_root
		return os.path.join(root_dir, *night_key.split("/"))

	def _load_file_list(self, night_key: str):
		night_dir = self._night_dir_path(night_key)
		if self._mode == "stacks":
			pattern = os.path.join(night_dir, "STACK_*.jpg")
		else:
			pattern = os.path.join(night_dir, "FF_*.bin")

		files = sorted(glob.glob(pattern), reverse=True)
		self._files_list = files

		self._files_lb.delete(0, tk.END)
		for f in files:
			self._files_lb.insert(tk.END, os.path.basename(f))

		if files:
			self._files_lb.selection_set(0)
			self._files_lb.activate(0)
			self._display_file(files[0])
		else:
			self._canvas.delete("all")
			self._current_path = None
			self._current_pil  = None
			self._status_var.set(f"No files in {night_dir}")

	# -----------------------------------------------------------------------
	# File display
	# -----------------------------------------------------------------------

	def _display_file(self, path: str):
		self._current_path = path
		fname = os.path.basename(path)
		try:
			if self._mode == "stacks":
				img = Image.open(path).convert("RGB")
				self._current_pil = img
				w, h = img.size
				self._fit_to_canvas(img)
				self._status_var.set(
					f"{fname}  |  {w}×{h}  |  {self._file_index_str()}"
				)
			else:
				ff = FFFile(path)
				self._current_pil = ff.to_pil_image(self._ff_plane)
				self._fit_to_canvas(self._current_pil)
				plane_name = PLANE_NAMES.get(self._ff_plane, "?")
				self._status_var.set(
					f"{fname}  |  {ff.ncols}×{ff.nrows}  |  "
					f"{self._file_index_str()}  |  "
					f"plane: {plane_name} [{self._ff_plane}]"
				)
		except Exception as exc:  # pylint: disable=broad-except
			self._canvas.delete("all")
			self._current_pil = None
			self._status_var.set(f"Error loading {fname}: {exc}")

	def _fit_to_canvas(self, img: Image.Image):
		if img is None:
			return
		cw = self._canvas.winfo_width()
		ch = self._canvas.winfo_height()
		if cw < 2 or ch < 2:
			# Canvas not yet laid out; retry after layout pass
			self.root.after(50, lambda: self._fit_to_canvas(img))
			return
		iw, ih = img.size
		scale = min(cw / iw, ch / ih)
		new_w = max(1, int(iw * scale))
		new_h = max(1, int(ih * scale))
		resized = img.resize((new_w, new_h), Image.LANCZOS)
		photo = ImageTk.PhotoImage(resized)
		self._canvas_img_ref = photo   # keep reference — prevents GC
		self._canvas.delete("all")
		self._canvas.create_image(cw // 2, ch // 2, anchor=tk.CENTER,
		                          image=photo)

	def _on_canvas_resize(self, _event):
		if self._current_pil is not None:
			self._fit_to_canvas(self._current_pil)

	# -----------------------------------------------------------------------
	# Navigation helpers
	# -----------------------------------------------------------------------

	def _file_index_str(self) -> str:
		if not self._files_list or self._current_path is None:
			return ""
		try:
			idx = self._files_list.index(self._current_path)
			return f"{idx + 1}/{len(self._files_list)}"
		except ValueError:
			return ""

	def _current_file_index(self) -> int:
		if self._current_path is None:
			return -1
		try:
			return self._files_list.index(self._current_path)
		except ValueError:
			return -1

	def _current_night_index(self) -> int:
		sel = self._nights_lb.curselection()
		return sel[0] if sel else -1

	def _step_file(self, delta: int):
		idx = self._current_file_index()
		if idx < 0 and self._files_list:
			idx = 0
		else:
			idx += delta
		self._jump_file(idx)

	def _jump_file(self, idx: int):
		if not self._files_list:
			return
		if idx < 0:
			idx = len(self._files_list) - 1
		idx = max(0, min(idx, len(self._files_list) - 1))
		self._files_lb.selection_clear(0, tk.END)
		self._files_lb.selection_set(idx)
		self._files_lb.activate(idx)
		self._files_lb.see(idx)
		self._display_file(self._files_list[idx])

	def _step_night(self, delta: int):
		idx = self._current_night_index()
		if idx < 0 and self._nights_list:
			idx = 0
		else:
			idx += delta
		idx = max(0, min(idx, len(self._nights_list) - 1))
		if 0 <= idx < len(self._nights_list):
			self._nights_lb.selection_clear(0, tk.END)
			self._nights_lb.selection_set(idx)
			self._nights_lb.activate(idx)
			self._nights_lb.see(idx)
			self._load_file_list(self._nights_list[idx])

	# -----------------------------------------------------------------------
	# Listbox event handlers
	# -----------------------------------------------------------------------

	def _on_night_select(self, _event):
		sel = self._nights_lb.curselection()
		if sel:
			self._load_file_list(self._nights_list[sel[0]])

	def _on_file_select(self, _event):
		sel = self._files_lb.curselection()
		if sel and sel[0] < len(self._files_list):
			self._display_file(self._files_list[sel[0]])

	# -----------------------------------------------------------------------
	# FF plane switching
	# -----------------------------------------------------------------------

	def _set_ff_plane(self, plane: int):
		if self._mode != "ff":
			return
		self._ff_plane = plane
		if self._current_path:
			self._display_file(self._current_path)

	# -----------------------------------------------------------------------
	# Delete operations
	# -----------------------------------------------------------------------

	def _delete_current(self):
		if not self._current_path:
			return
		fname = os.path.basename(self._current_path)
		if not messagebox.askyesno("Delete", f"Delete {fname}?",
		                           icon="warning", default="no"):
			return
		idx = self._current_file_index()
		try:
			os.unlink(self._current_path)
		except OSError as exc:
			messagebox.showerror("Delete failed", str(exc))
			return
		self._files_list.pop(idx)
		self._files_lb.delete(idx)
		self._current_path = None
		self._current_pil  = None
		if self._files_list:
			new_idx = min(idx, len(self._files_list) - 1)
			self._files_lb.selection_set(new_idx)
			self._files_lb.activate(new_idx)
			self._display_file(self._files_list[new_idx])
		else:
			self._canvas.delete("all")
			self._status_var.set("No files remaining")

	def _delete_night(self):
		night_idx = self._current_night_index()
		if night_idx < 0:
			return
		night_name = self._nights_list[night_idx]
		night_dir = self._night_dir_path(night_name)
		if not messagebox.askyesno(
			"Delete Night",
			f"Delete entire night directory?\n\n{night_dir}",
			icon="warning", default="no",
		):
			return
		try:
			shutil.rmtree(night_dir)
		except OSError as exc:
			messagebox.showerror("Delete failed", str(exc))
			return
		self._nights_list.pop(night_idx)
		self._nights_lb.delete(night_idx)
		self._files_list = []
		self._files_lb.delete(0, tk.END)
		self._canvas.delete("all")
		self._current_path = None
		self._current_pil  = None
		if self._nights_list:
			new_idx = min(night_idx, len(self._nights_list) - 1)
			self._nights_lb.selection_set(new_idx)
			self._nights_lb.activate(new_idx)
			self._load_file_list(self._nights_list[new_idx])
		else:
			self._status_var.set("No nights remaining")

	# -----------------------------------------------------------------------
	# Timelapse
	# -----------------------------------------------------------------------

	def _start_timelapse(self):
		if self._tl_worker and self._tl_worker.is_alive():
			self._status_var.set(
				"Timelapse already running — press Escape to cancel"
			)
			return
		if not self._files_list:
			self._status_var.set("No files to create timelapse")
			return

		# Capture night selection before the dialog steals focus (X11
		# exportselection would clear curselection when the dialog's Entry
		# takes keyboard ownership, returning night_idx = -1 silently).
		night_idx = self._current_night_index()
		if night_idx < 0:
			self._status_var.set("No night selected")
			return
		night_name = self._nights_list[night_idx]
		night_dir  = self._night_dir_path(night_name)

		self.root.lift()
		self.root.focus_force()
		fps = simpledialog.askinteger(
			"Timelapse FPS", "Frames per second:",
			initialvalue=10, minvalue=1, maxvalue=60,
			parent=self.root,
		)
		if fps is None:
			return

		if self._mode == "ff":
			plane_name = PLANE_NAMES.get(self._ff_plane, "?")
			self._status_var.set(
				f"Converting FF plane '{plane_name}' → timelapse at {fps} fps …"
			)
		else:
			self._status_var.set(f"Building timelapse at {fps} fps …")
		self._btn_tl.configure(state=tk.DISABLED)

		self._tl_worker = TimelapseWorker(
			files=list(reversed(self._files_list)),
			night_dir=night_dir,
			night_name=night_name,
			fps=fps,
			root=self.root,
			ff_plane=self._ff_plane if self._mode == "ff" else 0,
			on_done=self._on_timelapse_done,
			on_progress=self._on_timelapse_progress,
		)
		self._tl_worker.start()

	def _cancel_timelapse(self):
		if self._tl_worker and self._tl_worker.is_alive():
			self._tl_worker.cancel()
			self._status_var.set("Cancelling timelapse …")

	def _on_timelapse_done(self, output_path, error):
		self._btn_tl.configure(state=tk.NORMAL)
		self.root.lift()
		self.root.focus_force()
		if error:
			self._status_var.set(f"Timelapse failed: {error}")
			messagebox.showerror("Timelapse", f"Failed:\n{error}")
		else:
			self._status_var.set(f"Timelapse saved: {output_path}")
			messagebox.showinfo("Timelapse", f"Saved:\n{output_path}")

	def _on_timelapse_progress(self, line: str):
		self._status_var.set(f"ffmpeg: {line}")

	# -----------------------------------------------------------------------
	# Reload
	# -----------------------------------------------------------------------

	def _reload(self):
		old_night_idx = self._current_night_index()
		old_night = (
			self._nights_list[old_night_idx]
			if old_night_idx >= 0 and old_night_idx < len(self._nights_list)
			else None
		)
		old_file = self._current_path

		self._scan_nights()

		# Restore previous night selection if still present
		if old_night and old_night in self._nights_list:
			idx = self._nights_list.index(old_night)
			self._nights_lb.selection_clear(0, tk.END)
			self._nights_lb.selection_set(idx)
			self._nights_lb.activate(idx)
			self._load_file_list(old_night)
			# Restore file selection within the night
			if old_file and old_file in self._files_list:
				fidx = self._files_list.index(old_file)
				self._files_lb.selection_clear(0, tk.END)
				self._files_lb.selection_set(fidx)
				self._files_lb.activate(fidx)
				self._display_file(old_file)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
	parser = argparse.ArgumentParser(
		description="Nightcam desktop review tool",
		formatter_class=argparse.ArgumentDefaultsHelpFormatter,
	)
	parser.add_argument(
		"--data-dir",
		default=os.path.expanduser("~/RMS_data"),
		help="RMS data root directory",
	)
	args = parser.parse_args()

	root = ttk.Window(themename="darkly")
	root.geometry("1280x800")
	NightcamReview(root, args.data_dir)
	root.mainloop()


if __name__ == "__main__":
	main()
