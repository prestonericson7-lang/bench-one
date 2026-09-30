#!/usr/bin/env python3
"""stage.py -- gather every card's contents into D:/start/machine-cards/<card>/ with a SHA256SUMS file,
so each card is written in one step when it is in the reader (machine/cards/README.md). Copies only;
never writes a card. Sources: the card image in hardware/pz7020-starlite/linux/out/, the model files in
machine/zynq/out/models/ (staged by the image build).   python machine/cards/stage.py
"""
import hashlib
import os
import shutil
import sys

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DEST = os.environ.get("CARDS", "D:/start/machine-cards")
IMG_XZ = os.path.join(REPO, "hardware/pz7020-starlite/linux/out/pz7020-starlite-sd.img.xz")
IMG_SHA = os.path.join(REPO, "hardware/pz7020-starlite/linux/out/sd-image.sha256")
MODELS = os.path.join(REPO, "machine/zynq/out/models")
PLAN = {
    "zynq":   [IMG_XZ, IMG_SHA],
    "teensy": [os.path.join(MODELS, "qwen05b.gguf")],
    "stm32":  [os.path.join(MODELS, "qwen05b.gguf")],
}


def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main():
    missing = [p for ps in PLAN.values() for p in ps if not os.path.exists(p)]
    if missing:
        sys.exit("missing: " + ", ".join(missing))
    for card, srcs in PLAN.items():
        d = os.path.join(DEST, card)
        os.makedirs(d, exist_ok=True)
        sums = []
        for s in srcs:
            t = os.path.join(d, os.path.basename(s))
            if not (os.path.exists(t) and os.path.getsize(t) == os.path.getsize(s)):
                shutil.copyfile(s, t)
            sums.append(f"{sha256(t)}  {os.path.basename(t)}")
            print(f"{card}/{os.path.basename(t)}  {os.path.getsize(t):,} bytes")
        if card == "zynq":
            with open(os.path.join(d, "zynq-node-2.txt"), "w", newline="\n") as f:
                f.write("2\n")
            sums.append(f"{sha256(os.path.join(d, 'zynq-node-2.txt'))}  zynq-node-2.txt")
        with open(os.path.join(d, "SHA256SUMS"), "w", newline="\n") as f:
            f.write("\n".join(sums) + "\n")
    print(f"staged in {DEST}")


if __name__ == "__main__":
    main()
