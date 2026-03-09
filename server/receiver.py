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
  pip install flask pyyaml requests geopy
  python3 receiver.py [--config config.yaml]
"""

import argparse
import io
import json
import logging
import os
import subprocess
import threading
import time
from datetime import datetime, timedelta, timezone

import numpy as np
import requests
import yaml
from flask import Flask, jsonify, request
from geopy.geocoders import Nominatim
from PIL import Image, ImageFilter

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
    "address": None,          # optional address to lookup flights relative to
    "opensky_radius_km": 16,  # search radius (~10 miles)
}


def load_config(path: str) -> dict:
    """Load config"""
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
    """Ensure dir"""
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
    """Make app"""
    # pylint: disable=too-many-statements
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
        except Exception:  # pylint: disable=broad-exception-caught
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
    # OpenSky Network Lookup (Background Worker)
    # -----------------------------------------------------------------------
    def _query_opensky_bg(ff_path: str, cfg: dict):
        """Query opensky"""
        # pylint: disable=too-many-locals,broad-exception-caught
        address = cfg.get("address")
        if not address:
            return

        try:
            geolocator = Nominatim(user_agent="meteor_station_receiver")
            location = geolocator.geocode(address)
            if not location:
                logging.error("opensky: could not geocode configured address: %s", address)
                return
            lat = location.latitude
            lon = location.longitude
        except Exception as exc:  # pylint: disable=broad-exception-caught
            logging.error("opensky: geocoding error: %s", exc)
            return

        # Parse FF filename: FF_<station>_YYYYMMDD_HHMMSS_mmm_000000.bin
        base = os.path.splitext(os.path.basename(ff_path))[0]
        parts = base.split("_")
        if len(parts) < 5 or parts[0] != "FF":
            return

        try:
            dt_str = parts[2] + parts[3]  # YYYYMMDDHHMMSS
            dt = datetime.strptime(dt_str, "%Y%m%d%H%M%S")
            dt = dt.replace(tzinfo=timezone.utc)
            unix_ts = int(dt.timestamp())
        except ValueError:
            logging.error("opensky: failed to parse timestamp from %s", base)
            return

        # Earth radius roughly 6371 km. 1 deg lat ~= 111 km.
        radius_km = cfg.get("opensky_radius_km", 16)
        lat_delta = radius_km / 111.0
        lon_delta = radius_km / (111.0 * np.cos(np.radians(lat)))

        lamin = lat - lat_delta
        lamax = lat + lat_delta
        lomin = lon - lon_delta
        lomax = lon + lon_delta

        url = (
            f"https://opensky-network.org/api/states/all?"
            f"time={unix_ts}&lamin={lamin}&lomin={lomin}&lamax={lamax}&lomax={lomax}"
        )

        try:
            resp = requests.get(url, timeout=10)
            resp.raise_for_status()
            data = resp.json()

            states = data.get("states")
            flights = []
            if states:
                for s in states:
                    flights.append({
                        "callsign": str(s[1]).strip() if s[1] else "UNKNOWN",
                        "origin_country": s[2],
                        "longitude": s[5],
                        "latitude": s[6],
                        "altitude_m": s[7],
                        "velocity_ms": s[9],
                        "heading_deg": s[10],
                    })

            out_path = os.path.splitext(ff_path)[0] + "_flights.json"
            with open(out_path, "w", encoding="utf-8") as f:
                json.dump({
                    "timestamp": unix_ts,
                    "target_lat": lat,
                    "target_lon": lon,
                    "radius_km": radius_km,
                    "flights_found": len(flights),
                    "flights": flights
                }, f, indent=2)

            logging.info("opensky: saved %d flights for %s", len(flights), base)

        except requests.exceptions.RequestException as e:
            logging.warning("opensky: query failed for %s: %s", base, e)

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

        if cfg.get("address") is not None:
            threading.Thread(target=_query_opensky_bg, args=(dest_path, cfg), daemon=True).start()

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
            except Exception as exc:  # pylint: disable=broad-except,broad-exception-caught
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
        with subprocess.Popen(cmd, shell=True) as _:  # noqa: S602
            pass
    except OSError as exc:
        logging.warning("RMS trigger failed: %s", exc)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    """Main"""
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
