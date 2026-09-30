#!/usr/bin/env python3
"""make_pages.py -- machine-wiring.svg as 1920x1080 pictures, one screen each, no zooming needed.
Same method as hardware/pz7020-starlite/tools/make_wiring_pages.py: render the SVG at 3x with headless
Edge, crop the page boxes the generator wrote to machine-wiring.pages.json, scale each to fill the
screen under a page title, save png/<name>.png.
    python make_machine_wiring_svg.py && python make_pages.py
"""
import json
import os
import subprocess
import tempfile

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
SVG = os.environ.get("SVG", os.path.join(HERE, "machine-wiring.svg"))
META_PATH = os.environ.get("PAGES", os.path.join(HERE, "machine-wiring.pages.json"))
OUT = os.environ.get("OUT", os.path.join(HERE, "png"))
EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
SW, SH, TITLE_H, PAD, K = 1920, 1080, 84, 24, 3

meta = json.load(open(META_PATH))
png = os.path.join(tempfile.gettempdir(), "machine-wiring-3x.png")
subprocess.run([EDGE, "--headless=new", "--disable-gpu", "--hide-scrollbars", f"--force-device-scale-factor={K}",
                f"--screenshot={png}", f"--window-size={meta['width']},{meta['height']}",
                "file:///" + SVG.replace("\\", "/")], check=True, capture_output=True)
big = Image.open(png).convert("RGB")
k = big.size[0] / meta["width"]
bg = meta["bg"]
try:
    font = ImageFont.truetype("arialbd.ttf", 46)
except OSError:
    font = ImageFont.load_default()
os.makedirs(OUT, exist_ok=True)
for old in os.listdir(OUT):
    if old.endswith(".png"):
        os.remove(os.path.join(OUT, old))
for p in meta["pages"]:
    body = big.crop(tuple(int(v * k) for v in p["box"]))
    aw, ah = SW - 2 * PAD, SH - TITLE_H - PAD
    s = min(aw / body.size[0], ah / body.size[1])
    body = body.resize((int(body.size[0] * s), int(body.size[1] * s)), Image.LANCZOS)
    page = Image.new("RGB", (SW, SH), bg)
    page.paste(body, ((SW - body.size[0]) // 2, TITLE_H + (ah - body.size[1]) // 2))
    ImageDraw.Draw(page).text((PAD + 8, 18), p["title"], fill="#f4f7fa", font=font)
    page.save(os.path.join(OUT, p["name"] + ".png"), optimize=True)
    print(f"{p['name']}.png  1920x1080  (drawing scaled x{s * k / K * K:.2f} of the 1x size)")
