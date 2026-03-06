#!/usr/bin/env python3
"""
receiver.py — HTTP server for the N100 Beelink that receives detection events,
FF binary files, and timelapse stack JPEGs from Wyze cameras.

Listens on 0.0.0.0:8765 (configurable via config.yaml).

Endpoints:
  POST /event  — log a JSON detection event (meteor or stack metadata with IVS)
  POST /ff     — save an FF binary file into the RMS CapturedFiles tree
  POST /stack  — save a timelapse stack JPEG into a dated directory

Usage:
  pip install flask pyyaml
  python3 receiver.py [--config config.yaml]
"""

import argparse
import io
import logging
import os
import subprocess
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path

import numpy as np
from flask import Flask, request, jsonify
from PIL import Image, ImageFilter
import yaml

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

DEFAULT_CONFIG = {
    "listen_host": "0.0.0.0",
    "listen_port": 8765,
    "rms_data_dir": os.path.expanduser("~/RMS_data"),
    "captured_subdir": "CapturedFiles",
    "stack_subdir": "Stacks",
    "raw_stack_subdir": "Stacks_raw",
    "rms_run_on_receive": False,          # set True to auto-trigger RMS per FF
    "rms_detect_script": "python3 -m RMS.DetectStarsAndMeteors",
    "log_level": "INFO",
    "stack_enhance": False,   # background-subtract + histogram-stretch stack JPEGs
    "stack_jpeg_quality": 92, # output JPEG quality after enhancement
    "save_raw_stack": True,   # save a copy of the unedited STACK JPEG
}


def load_config(path: str) -> dict:
    cfg = dict(DEFAULT_CONFIG)
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as fh:
            overrides = yaml.safe_load(fh) or {}
        cfg.update(overrides)
    # Expand ~ in path values that may come from config.yaml as raw strings.
    cfg["rms_data_dir"] = os.path.expanduser(cfg["rms_data_dir"])
    return cfg


# ---------------------------------------------------------------------------
# Night-directory helpers
# ---------------------------------------------------------------------------

def night_dir_name(dt: datetime) -> str:
    """
    Return an RMS-style night directory name for a given UTC datetime.
    Nights start at 12:00 UTC and end the following day at 11:59 UTC,
    so a recording at 02:00 on the 16th belongs to the night starting on the 15th.
    """
    if dt.hour < 12:
        night_start = dt.replace(hour=12, minute=0, second=0,
                                 microsecond=0) - timedelta(days=1)
    else:
        night_start = dt.replace(hour=12, minute=0, second=0, microsecond=0)
    return night_start.strftime("%Y%m%d_%H%M%S_000000")


def station_from_filename(filename: str) -> str:
    """
    Extract station ID from an FF or STACK filename.
    FF_XX0001_...bin  →  XX0001
    STACK_XX0001_...jpg  →  XX0001
    Falls back to 'unknown' if the filename doesn't match the pattern.
    """
    parts = filename.split("_")
    if len(parts) >= 2 and parts[1]:
        return parts[1]
    return "unknown"


def ensure_dir(path: str) -> None:
    os.makedirs(path, exist_ok=True)


# ---------------------------------------------------------------------------
# Stack JPEG enhancement
# ---------------------------------------------------------------------------

def _enhance_stack(data: bytes, quality: int = 92) -> bytes:
    """
    Improve a stack JPEG for star visibility:
      1. Background subtraction — large Gaussian blur models the sky-glow
         gradient and is subtracted, leaving stars on a near-flat background.
      2. Denoise — mild median filter removes JPEG compression artifacts
         that would otherwise be amplified by the stretch.
      3. Histogram stretch — 1st–99.5th percentile mapped to 0–255 so faint
         stars use the full dynamic range.
    """
    img = Image.open(io.BytesIO(data)).convert("L")

    # Denoise on the original uint8 image FIRST — before any float arithmetic.
    # This removes JPEG DCT block artifacts while the pixel values are still
    # clean integers.  radius=0.8 is intentionally gentle: the Wyze ISP already
    # applies its own NR which creates correlated 10–20 px noise blobs; a
    # stronger spatial filter reveals those blobs as visible texture rather than
    # reducing them.
    denoised = img.filter(ImageFilter.GaussianBlur(radius=0.8))
    arr = np.array(denoised, dtype=np.float32)

    # Background model: blur radius ~8% of width captures slow sky-glow
    # gradient without touching point sources (stars are 1–3 px wide).
    radius = max(20, img.width // 12)
    bg = np.array(denoised.filter(ImageFilter.GaussianBlur(radius=radius)),
                  dtype=np.float32)

    # Subtract background in float, re-centre at 32 so residuals land above black.
    sub = arr - bg + 32.0

    # Percentile stretch — entirely in float32, no intermediate uint8 conversion.
    lo = np.percentile(sub, 1.0)
    hi = np.percentile(sub, 99.5)
    if hi > lo:
        stretched = np.clip((sub - lo) / (hi - lo) * 255.0, 0, 255)
    else:
        stretched = np.zeros_like(sub)

    out = io.BytesIO()
    Image.fromarray(stretched.astype(np.uint8)).save(out, format="JPEG",
                                                     quality=quality)
    return out.getvalue()


# ---------------------------------------------------------------------------
# Flask application factory
# ---------------------------------------------------------------------------

def make_app(cfg: dict) -> Flask:
    app = Flask(__name__)

    captured_root = os.path.join(cfg["rms_data_dir"], cfg["captured_subdir"])
    stack_root    = os.path.join(cfg["rms_data_dir"], cfg["stack_subdir"])
    raw_stack_root = os.path.join(cfg["rms_data_dir"], cfg.get("raw_stack_subdir", "Stacks_raw"))
    ensure_dir(captured_root)
    ensure_dir(stack_root)
    if cfg.get("save_raw_stack", True):
        ensure_dir(raw_stack_root)

    # -----------------------------------------------------------------------
    # GET /time — return current UTC Unix timestamp for camera clock sync
    # -----------------------------------------------------------------------
    @app.route("/time", methods=["GET"])
    def get_time():
        return jsonify({"unix": time.time()})

    # -----------------------------------------------------------------------
    # POST /event — receive a JSON detection event
    # -----------------------------------------------------------------------
    @app.route("/event", methods=["POST"])
    def recv_event():
        try:
            evt = request.get_json(force=True, silent=True) or {}
        except Exception:
            evt = {}

        ts_ms    = evt.get("timestamp_ms", int(time.time() * 1000))
        cam      = evt.get("camera_id", "unknown")
        evt_type = evt.get("type", "unknown")

        if evt_type == "meteor":
            cand = evt.get("candidate", {})
            logging.info(
                "METEOR cam=%s ts_ms=%d rho=%s theta=%s votes=%s len=%s",
                cam, ts_ms,
                cand.get("rho", "?"), cand.get("theta", "?"),
                cand.get("votes", "?"), cand.get("length_px", "?"),
            )
        elif evt_type == "stack":
            logging.info(
                "STACK  cam=%s ts_ms=%d file=%s "
                "ivs_polls=%s active=%s total_rois=%s last=%s",
                cam, ts_ms,
                evt.get("filename", "?"),
                evt.get("ivs_polls", "?"),
                evt.get("ivs_active_polls", "?"),
                evt.get("ivs_total_rois", "?"),
                evt.get("ivs_last_rois", "?"),
            )
        else:
            logging.info("EVENT  cam=%s ts_ms=%d type=%s",
                         cam, ts_ms, evt_type)

        return jsonify({"status": "ok"}), 200

    # -----------------------------------------------------------------------
    # POST /ff — receive an FF binary file
    # -----------------------------------------------------------------------
    @app.route("/ff", methods=["POST"])
    def recv_ff():
        filename = request.headers.get("X-Filename", "")
        if not filename or "/" in filename or "\\" in filename:
            logging.warning("recv_ff: missing or unsafe X-Filename header")
            return jsonify({"status": "error", "msg": "bad filename"}), 400

        now        = datetime.now(tz=timezone.utc)
        station    = station_from_filename(filename)
        night_dir  = os.path.join(captured_root, station, night_dir_name(now))
        ensure_dir(night_dir)
        dest_path  = os.path.join(night_dir, filename)

        data = request.get_data()
        with open(dest_path, "wb") as fh:
            fh.write(data)

        logging.info("FF saved: %s (%d bytes)", dest_path, len(data))

        if cfg.get("rms_run_on_receive"):
            _trigger_rms(cfg, night_dir)

        return jsonify({"status": "ok", "path": dest_path}), 200

    # -----------------------------------------------------------------------
    # POST /stack — receive a timelapse stack JPEG from nightcam
    # -----------------------------------------------------------------------
    @app.route("/stack", methods=["POST"])
    def recv_stack():
        filename = request.headers.get("X-Filename", "")
        if not filename or "/" in filename or "\\" in filename:
            logging.warning("recv_stack: missing or unsafe X-Filename header")
            return jsonify({"status": "error", "msg": "bad filename"}), 400

        now       = datetime.now(tz=timezone.utc)
        station   = station_from_filename(filename)
        night_dir = os.path.join(stack_root, station, night_dir_name(now))
        ensure_dir(night_dir)
        dest_path = os.path.join(night_dir, filename)

        data = request.get_data()

        if cfg.get("save_raw_stack", True):
            raw_night_dir = os.path.join(raw_stack_root, station, night_dir_name(now))
            ensure_dir(raw_night_dir)
            raw_dest_path = os.path.join(raw_night_dir, filename)
            with open(raw_dest_path, "wb") as fh:
                fh.write(data)
            logging.info("RAW STACK saved: %s (%d bytes)", raw_dest_path, len(data))

        if cfg.get("stack_enhance", True):
            try:
                data = _enhance_stack(data, quality=cfg.get("stack_jpeg_quality", 92))
            except Exception as exc:  # pylint: disable=broad-except
                logging.warning("stack enhance failed, saving raw: %s", exc)

        with open(dest_path, "wb") as fh:
            fh.write(data)

        logging.info("STACK saved: %s (%d bytes)", dest_path, len(data))
        return jsonify({"status": "ok", "path": dest_path}), 200

    return app


# ---------------------------------------------------------------------------
# RMS trigger (optional, called after each FF file)
# ---------------------------------------------------------------------------

def _trigger_rms(cfg: dict, night_dir: str) -> None:
    cmd = f"{cfg['rms_detect_script']} {night_dir}"
    logging.info("Triggering RMS: %s", cmd)
    try:
        subprocess.Popen(cmd, shell=True)  # noqa: S602  (trusted internal LAN command)
    except OSError as exc:
        logging.warning("RMS trigger failed: %s", exc)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(description="Meteor FF receiver")
    parser.add_argument("--config", default="config.yaml",
                        help="path to config.yaml (default: config.yaml)")
    args = parser.parse_args()

    cfg = load_config(args.config)

    logging.basicConfig(
        level=getattr(logging, cfg["log_level"].upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(message)s",
    )

    app = make_app(cfg)
    logging.info("Starting receiver on %s:%s",
                 cfg["listen_host"], cfg["listen_port"])
    app.run(host=cfg["listen_host"], port=int(cfg["listen_port"]),
            threaded=True)


if __name__ == "__main__":
    main()
