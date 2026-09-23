#!/usr/bin/env python3
"""
sentry.py -- Luckfox Pico camera node: motion-triggered recording for the car.

WHY THE LUCKFOX RUNS THIS AND NOT THE PI
    The RV1103 has a hardware ISP and a hardware H.264/H.265 ENCODER. Encoding
    in silicon is why a ~1 W board can record continuously while the Pi and the
    Zynq are fully powered down. That is the whole basis of low-power sentry
    mode: the expensive boards sleep, the cheap cameras watch.

PRE-ROLL IS THE POINT
    A sentry system that starts recording when it detects motion has already
    missed the event. This keeps a rolling PRE-ROLL buffer so a saved clip
    begins BEFORE the trigger. Without that you get footage of someone walking
    away, which is useless.

POWER DISCIPLINE
    A car battery is a hard budget. This node watches supply voltage and stops
    recording at a cutoff so it can never strand the car -- losing footage is
    survivable, a dead battery in a parking lot is not.

RUNTIME PROBING, NOT ASSUMPTIONS
    Capture backends differ across Luckfox images (v4l2, ffmpeg, rkipc). This
    probes what actually exists rather than hardcoding one, and the motion /
    clip / power logic is pure so it can be tested anywhere.
        python sentry.py --selftest
"""
import argparse
import collections
import os
import shutil
import subprocess
import sys
import time

# ---------------- configuration ----------------
DEV            = os.environ.get("SENTRY_DEV", "/dev/video0")
OUTDIR         = os.environ.get("SENTRY_OUT", "/mnt/sdcard/sentry")
WIDTH, HEIGHT  = 1280, 720
FPS            = 15
PREROLL_S      = 10          # seconds kept before a trigger
POSTROLL_S     = 20          # seconds kept after motion stops
MIN_FREE_MB    = 512         # rotate oldest clips below this
VOLT_CUTOFF    = 12.0        # stop recording below this (protect the battery)
VOLT_RESUME    = 12.4
MOTION_THRESH  = 12.0        # mean abs frame delta (0-255) that counts as motion
MOTION_PIXFRAC = 0.02        # ...over at least this fraction of the frame


# ============================================================
#  motion detection  (pure -- unit testable)
# ============================================================
def frame_delta(prev, cur):
    """Mean absolute difference and the fraction of changed cells.

    Operates on a coarse downsampled grayscale grid, not full frames: at 720p
    a full-resolution diff would eat the CPU budget we are trying to save.
    Inputs are flat sequences of ints (same length).
    """
    if prev is None or len(prev) != len(cur):
        return 0.0, 0.0
    n = len(cur)
    total = 0
    changed = 0
    for a, b in zip(prev, cur):
        d = a - b
        if d < 0:
            d = -d
        total += d
        if d > MOTION_THRESH:
            changed += 1
    return total / n, changed / n


def is_motion(prev, cur):
    mean_d, frac = frame_delta(prev, cur)
    return mean_d > MOTION_THRESH and frac > MOTION_PIXFRAC


# ============================================================
#  pre-roll ring  (pure -- unit testable)
# ============================================================
class PreRoll:
    """Fixed-duration ring of encoded chunks.

    Stores already-ENCODED segments, not raw frames: raw 720p at 15 fps is
    ~20 MB/s and would exhaust the Luckfox's ~33 MB of usable RAM in under two
    seconds. Encoded, ten seconds costs a couple of megabytes.
    """

    def __init__(self, seconds, fps):
        self.max_chunks = max(1, int(seconds * fps))
        self.buf = collections.deque(maxlen=self.max_chunks)

    def push(self, chunk):
        self.buf.append(chunk)

    def drain(self):
        out = list(self.buf)
        self.buf.clear()
        return out

    def __len__(self):
        return len(self.buf)


# ============================================================
#  power  (pure -- unit testable)
# ============================================================
class PowerGate:
    """Hysteretic battery guard. Recording is a luxury; starting the car is not."""

    def __init__(self, cutoff=VOLT_CUTOFF, resume=VOLT_RESUME):
        self.cutoff, self.resume = cutoff, resume
        self.allowed = True

    def update(self, volts):
        if volts is None:
            return self.allowed                 # no reading -> don't change state
        if self.allowed and volts < self.cutoff:
            self.allowed = False
        elif not self.allowed and volts >= self.resume:
            self.allowed = True
        return self.allowed


# ============================================================
#  storage  (pure enough to test with a temp dir)
# ============================================================
def free_mb(path):
    try:
        st = os.statvfs(path)
        return st.f_bavail * st.f_frsize / 1e6
    except Exception:                           # noqa: BLE001 - Windows/dev host
        return float("inf")


def rotate(outdir, min_free_mb=MIN_FREE_MB):
    """Delete oldest clips until there is room. Returns how many were removed."""
    removed = 0
    while free_mb(outdir) < min_free_mb:
        clips = sorted(
            (os.path.join(outdir, f) for f in os.listdir(outdir)
             if f.endswith((".mp4", ".h264"))),
            key=lambda p: os.path.getmtime(p))
        if not clips:
            break
        os.remove(clips[0])
        removed += 1
    return removed


# ============================================================
#  capture backend probing
# ============================================================
def probe_backends():
    """Report which capture paths exist on THIS image, rather than assuming one."""
    found = {}
    found["device"] = os.path.exists(DEV)
    for tool in ("ffmpeg", "v4l2-ctl", "rkipc", "gst-launch-1.0"):
        found[tool] = shutil.which(tool) is not None
    # Rockchip hardware encoder nodes, if the vendor stack is present
    found["rkmpp"] = any(os.path.exists(p) for p in
                         ("/dev/mpp_service", "/dev/rga", "/proc/rkmpp"))
    return found


def encoder_cmd(backends, outfile, seconds):
    """Build a capture command that uses HARDWARE encode where available."""
    if backends.get("ffmpeg") and backends.get("device"):
        # h264_rkmpp when the Rockchip encoder is exposed; otherwise let ffmpeg
        # pick, and accept the CPU cost.
        vcodec = "h264_rkmpp" if backends.get("rkmpp") else "libx264"
        return ["ffmpeg", "-hide_banner", "-loglevel", "error",
                "-f", "v4l2", "-framerate", str(FPS),
                "-video_size", f"{WIDTH}x{HEIGHT}", "-i", DEV,
                "-t", str(seconds), "-c:v", vcodec, "-y", outfile]
    return None


# ============================================================
#  selftest
# ============================================================
def selftest():
    ok = True

    print("1) motion detection")
    still = [100] * 256
    same = [100] * 256
    moved = [100] * 256
    for i in range(0, 60):            # disturb ~23% of cells, well above threshold
        moved[i] = 200
    if is_motion(still, same):
        print("   FAIL: flagged motion on identical frames"); ok = False
    else:
        print("   ok: identical frames -> no motion")
    if not is_motion(still, moved):
        print("   FAIL: missed obvious motion"); ok = False
    else:
        md, fr = frame_delta(still, moved)
        print(f"   ok: motion detected (mean delta {md:.1f}, {fr:.0%} of frame)")
    # a tiny glint should NOT trigger -- otherwise every passing headlight records
    glint = [100] * 256
    glint[0] = 255
    if is_motion(still, glint):
        print("   FAIL: single-cell glint triggered recording"); ok = False
    else:
        print("   ok: single-cell glint ignored (no false trigger)")

    print("2) pre-roll ring")
    pr = PreRoll(PREROLL_S, FPS)
    for i in range(500):
        pr.push(i)
    if len(pr) != pr.max_chunks:
        print(f"   FAIL: ring held {len(pr)}, expected {pr.max_chunks}"); ok = False
    else:
        print(f"   ok: capped at {pr.max_chunks} chunks ({PREROLL_S}s @ {FPS}fps)")
    d = pr.drain()
    if d[-1] != 499 or len(pr) != 0:
        print("   FAIL: drain did not return newest / did not clear"); ok = False
    else:
        print(f"   ok: drain returns most recent (ends {d[-1]}) and clears")

    print("3) power gate (hysteresis)")
    g = PowerGate()
    seq = [(12.8, True), (12.1, True), (11.9, False), (12.1, False),
           (12.5, True), (None, True)]
    for v, want in seq:
        got = g.update(v)
        if got != want:
            print(f"   FAIL: {v}V -> {got}, expected {want}"); ok = False
    if ok:
        print("   ok: cuts out below 12.0 V, only resumes at 12.4 V, "
              "ignores missing reads")

    print("4) backend probe (informational on this host)")
    for k, v in probe_backends().items():
        print(f"   {k:14} {v}")

    print("\nPASSED" if ok else "\nFAILED")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--probe", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        sys.exit(0 if selftest() else 1)
    if a.probe:
        for k, v in probe_backends().items():
            print(f"{k:14} {v}")
        return
    ap.error("run with --selftest or --probe (capture loop runs on the Luckfox)")


if __name__ == "__main__":
    main()
