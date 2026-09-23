#!/usr/bin/env python3
"""
assistant.py -- the "hey car" pipeline, wired end to end.

    wake word (ESP32-S3)          -- external, arrives as a trigger
      -> clean audio (Zynq FPGA)  -- external, beamformed/AEC'd
      -> whisper.cpp  tiny.en     -- speech to text          [this file calls it]
      -> intent.py                -- grammar, deterministic  [this file calls it]
      -> poi index (SQLite R-tree)-- find the place          [this file calls it]
      -> Valhalla                 -- route to it             [this file calls it]
      -> Piper                    -- speak the answer        [this file calls it]

Every external tool is OPTIONAL at import time and probed at runtime, so this
runs and self-tests on a machine with none of them installed. In the car, missing
tools degrade to a spoken/printed error rather than a traceback -- a dead TTS
must never take down navigation.

    python assistant.py --selftest              # no deps, no data
    python assistant.py --say "take me to the closest Petco" --lat 40 --lon -75
"""
import argparse
import json
import os
import shutil
import sqlite3
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import intent as intent_mod
from build_poi_index import find_nearest, schema, add_poi

VALHALLA_URL = os.environ.get("VALHALLA_URL", "http://127.0.0.1:8002")
WHISPER_BIN = os.environ.get("WHISPER_BIN", "whisper-cli")
WHISPER_MODEL = os.environ.get("WHISPER_MODEL", "models/ggml-tiny.en.bin")
PIPER_BIN = os.environ.get("PIPER_BIN", "piper")
PIPER_VOICE = os.environ.get("PIPER_VOICE", "en_US-lessac-medium.onnx")
POI_DB = os.environ.get("POI_DB", "poi.db")


def have(binary):
    return shutil.which(binary) is not None


# ------------------------------------------------------------------ speech in
def transcribe(wav_path):
    """whisper.cpp -> text. tiny.en is the model this board can run in real time:
    'base' needs ~4 A76 threads and we only have 2 A76 cores."""
    if not have(WHISPER_BIN):
        return None, f"{WHISPER_BIN} not installed"
    try:
        r = subprocess.run(
            [WHISPER_BIN, "-m", WHISPER_MODEL, "-f", wav_path, "-nt", "-t", "4"],
            capture_output=True, text=True, timeout=30)
        return r.stdout.strip(), None
    except Exception as e:                       # noqa: BLE001 - never crash the car UI
        return None, f"whisper failed: {e}"


# ----------------------------------------------------------------- speech out
def speak(text):
    print(f"[say] {text}")
    if not have(PIPER_BIN):
        return False
    try:
        subprocess.run([PIPER_BIN, "-m", PIPER_VOICE, "-f", "/tmp/say.wav"],
                       input=text, text=True, capture_output=True, timeout=20)
        for p in ("aplay", "paplay"):
            if have(p):
                subprocess.run([p, "/tmp/say.wav"], capture_output=True, timeout=20)
                break
        return True
    except Exception:                            # noqa: BLE001
        return False


# --------------------------------------------------------------------- route
def route(from_lat, from_lon, to_lat, to_lon):
    """Ask Valhalla for a route. Chosen over OSRM/GraphHopper because its
    tile-based model loads only the region queried -- RAM is our scarce
    resource (4 GB), and disk (4.5 TB) is not."""
    import urllib.request
    req = {
        "locations": [{"lat": from_lat, "lon": from_lon},
                      {"lat": to_lat, "lon": to_lon}],
        "costing": "auto",
        "directions_options": {"units": "miles"},
    }
    try:
        url = f"{VALHALLA_URL}/route"
        data = json.dumps(req).encode()
        with urllib.request.urlopen(
                urllib.request.Request(url, data=data,
                                       headers={"Content-Type": "application/json"}),
                timeout=8) as r:
            js = json.load(r)
        summ = js["trip"]["summary"]
        return {"km": summ.get("length"), "min": summ.get("time", 0) / 60.0,
                "raw": js}, None
    except Exception as e:                       # noqa: BLE001
        return None, f"valhalla unavailable: {e}"


# ------------------------------------------------------------------ pipeline
def handle(text, lat, lon, db):
    cmd = intent_mod.parse(text)
    act = cmd["action"]

    if act == "unknown":
        speak("Sorry, I didn't catch a destination.")
        return cmd

    if act != "navigate":
        # Non-navigation commands are routed to the rest of the fleet over MQTT
        # in the real system; here we surface them so the caller can dispatch.
        speak({"cancel": "Cancelled.",
               "navigate_home": "Routing home.",
               "battery_status": "Checking battery.",
               "range_status": "Checking range.",
               "vent_open": "Opening the vent.",
               "vent_close": "Closing the vent.",
               "where_am_i": "Checking location.",
               "eta": "Checking arrival time."}.get(act, "Okay."))
        return cmd

    target = cmd["target"]
    # Category search when the phrase maps to one ("gas station" -> amenity=fuel);
    # otherwise treat it as a name/brand and let the index match fuzzily.
    hits = find_nearest(db, target, lat, lon, category=cmd.get("category"))
    if not hits and cmd.get("category"):
        hits = find_nearest(db, target, lat, lon)      # fall back to name match
    if not hits:
        speak(f"I couldn't find a {target} nearby.")
        cmd["result"] = None
        return cmd

    best = hits[0]
    cmd["poi"] = best
    r, err = route(lat, lon, best["lat"], best["lon"])
    if err:
        speak(f"Found {best['name']}, {best['km']:.1f} kilometres away, "
              f"but routing is unavailable.")
        cmd["route_error"] = err
        return cmd

    speak(f"Routing to {best['name']}, {r['km']:.1f} kilometres, "
          f"about {r['min']:.0f} minutes.")
    cmd["route"] = {"km": r["km"], "min": r["min"]}
    return cmd


# ------------------------------------------------------------------ selftest
def selftest():
    print("selftest: full chain with a synthetic POI index, no external tools\n")
    db = sqlite3.connect(":memory:")
    schema(db)
    add_poi(db, 1, "Petco", "Petco", "shop=pet", 40.0100, -75.0000)
    add_poi(db, 2, "Shell", "Shell", "amenity=fuel", 40.0030, -75.0000)
    db.commit()

    print("tool availability:")
    for b in (WHISPER_BIN, PIPER_BIN):
        print(f"  {b:12} {'found' if have(b) else 'NOT installed (degrades gracefully)'}")
    print()

    ok = True
    for utter, expect in [("hey car take me to the closest Petco", "navigate"),
                          ("take me to the nearest gas", "navigate"),
                          ("what is the weather", "unknown"),
                          ("cancel", "cancel")]:
        print(f"> {utter}")
        res = handle(utter, 40.0, -75.0, db)
        got = res["action"]
        mark = "ok" if got == expect else "FAIL"
        if got != expect:
            ok = False
        if res.get("poi"):
            print(f"  matched POI: {res['poi']['name']} @ {res['poi']['km']:.2f} km")
        print(f"  [{mark}] action={got}\n")

    print("PASSED" if ok else "FAILED")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--say"); ap.add_argument("--wav")
    ap.add_argument("--lat", type=float); ap.add_argument("--lon", type=float)
    ap.add_argument("--db", default=POI_DB)
    a = ap.parse_args()

    if a.selftest:
        sys.exit(0 if selftest() else 1)

    text = a.say
    if a.wav:
        text, err = transcribe(a.wav)
        if err:
            print(err, file=sys.stderr); sys.exit(1)
        print(f"heard: {text}")
    if not text:
        ap.error("need --say, --wav, or --selftest")
    if a.lat is None or a.lon is None:
        ap.error("need --lat/--lon (from GPS in the car)")

    db = sqlite3.connect(a.db)
    handle(text, a.lat, a.lon, db)


if __name__ == "__main__":
    main()
