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
    # mk_sd_image.sh writes sd-image.sha256 and THEN the .xz; a checksum file newer than the .xz means the
    # last build skipped compression (NOXZ) and the .xz is an older image that this checksum does not
    # describe (2026-10-05: that pair would have been staged together). write_sd.py would refuse it at
    # write time; refusing here keeps a mismatched pair off the staging drive.
    if os.path.getmtime(IMG_SHA) > os.path.getmtime(IMG_XZ):
        sys.exit(f"{IMG_XZ} is older than {IMG_SHA}: the last build did not compress its image -- xz it first")
    for card, srcs in PLAN.items():
        d = os.path.join(DEST, card)
        os.makedirs(d, exist_ok=True)
        sums = []
        for s in srcs:
            t = os.path.join(d, os.path.basename(s))
            # copy unless the staged file is the same size AND at least as new as the source (a same-size
            # newer source -- a rebuilt 100-byte checksum file -- must be copied; the first version skipped it)
            same = os.path.exists(t) and os.path.getsize(t) == os.path.getsize(s) and os.path.getmtime(t) >= os.path.getmtime(s)
            if not same:
                shutil.copyfile(s, t)
                shutil.copystat(s, t)
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
