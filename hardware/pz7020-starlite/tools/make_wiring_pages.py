#!/usr/bin/env python3
"""make_wiring_pages.py -- system-wiring.svg as six 1920x1080 pictures, one screen each, no zooming needed.
Renders the SVG at 3x with headless Edge, crops the page boxes make_system_wiring_svg.py wrote to
system-wiring.pages.json, scales each to fill the screen under a page title, and saves
../wiring-png/<name>.png.   python make_system_wiring_svg.py > ../system-wiring.svg && python make_wiring_pages.py
"""
import json
import os
import subprocess
import tempfile

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.normpath(os.path.join(HERE, ".."))
SVG = os.path.join(TOP, "system-wiring.svg")
OUT = os.path.join(TOP, "wiring-png")
EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
SW, SH, TITLE_H, PAD, K = 1920, 1080, 84, 24, 3

meta = json.load(open(os.path.join(TOP, "system-wiring.pages.json")))
png = os.path.join(tempfile.gettempdir(), "system-wiring-3x.png")
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


def crop(b):
    return big.crop(tuple(int(v * k) for v in b))


for p in meta["pages"]:
    body = crop(p["box"])
    if "header" in p:                                  # the table's column titles on both table pages
        hd = crop(p["header"])
        both = Image.new("RGB", (body.size[0], hd.size[1] + body.size[1]), bg)
        both.paste(hd, (0, 0)); both.paste(body, (0, hd.size[1]))
        body = both
    aw, ah = SW - 2 * PAD, SH - TITLE_H - PAD
    s = min(aw / body.size[0], ah / body.size[1])
    body = body.resize((int(body.size[0] * s), int(body.size[1] * s)), Image.LANCZOS)
    page = Image.new("RGB", (SW, SH), bg)
    page.paste(body, ((SW - body.size[0]) // 2, TITLE_H + (ah - body.size[1]) // 2))
    ImageDraw.Draw(page).text((PAD + 8, 18), p["title"], fill="#f4f7fa", font=font)
    page.save(os.path.join(OUT, p["name"] + ".png"), optimize=True)
    print(f"{p['name']}.png  1920x1080  (drawing scaled x{s * k / K * K:.2f} of the 1x size)")
