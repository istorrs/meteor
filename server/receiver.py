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
import logging
import os
import subprocess
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path

from flask import Flask, request, jsonify
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
    "rms_run_on_receive": False,          # set True to auto-trigger RMS per FF
    "rms_detect_script": "python3 -m RMS.DetectStarsAndMeteors",
    "log_level": "INFO",
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


def ensure_dir(path: str) -> None:
    os.makedirs(path, exist_ok=True)


# ---------------------------------------------------------------------------
# Flask application factory
# ---------------------------------------------------------------------------

def make_app(cfg: dict) -> Flask:
    app = Flask(__name__)

    captured_root = os.path.join(cfg["rms_data_dir"], cfg["captured_subdir"])
    stack_root    = os.path.join(cfg["rms_data_dir"], cfg["stack_subdir"])
    ensure_dir(captured_root)
    ensure_dir(stack_root)

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
        night_dir  = os.path.join(captured_root, night_dir_name(now))
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
        night_dir = os.path.join(stack_root, night_dir_name(now))
        ensure_dir(night_dir)
        dest_path = os.path.join(night_dir, filename)

        data = request.get_data()
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
