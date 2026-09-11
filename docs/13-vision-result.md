# 13 — The dog test: what happened, honestly

Run on **24 real photographs** from Wikimedia Commons (free licences), 8 German Shepherd,
8 Labrador, 8 Husky. Two rejected for having two dogs in frame. The German Shepherd **skeleton**
held out as a test case.

## Result

| test | result |
|---|---|
| Name the breed of a photo it has never seen | **4 / 9** (chance is 3/9) |
| Skeleton: does it honestly say "I don't know"? | **No** — matched confidently at 6.3σ |
| Recall a fact it was told once (origin, role) | **6 / 6 perfect** |

## Three real bugs found and fixed in the encoder

1. **Magnitude gate set above the signal.** Threshold 12, mean magnitude 5–10 → 72% of every
   image discarded. Cells contributing: **17.7/64**.
2. **Two of eight orientation bins never fired.** The bin-mapping arithmetic could not produce
   bins 1 or 4.
3. **The killer: signed gradients were averaged over each cell.** Opposing edges cancel, which
   is why mean magnitude collapsed. Real HOG accumulates a magnitude-weighted *histogram* per
   pixel and never averages the raw signed gradient.

After fixing all three: cells contributing **48.2/64**, all **8/8** bins live.

## And it did not help

| encoding | same-breed vs different-breed separation |
|---|---|
| gradient histogram, before fixes | 59 bits of 4096 |
| gradient histogram, after fixes | **88 bits of 4096** |
| silhouette shape | **−33 bits** (same-breed slightly *farther*) |

Skeleton-to-live-dog by shape: **0 of the 5 nearest were German Shepherds**, against a chance
expectation of 1.6. Worse than guessing.

## The honest conclusion

**The bugs were not the problem. The approach is.**

An 8×8 grid of gradient statistics encodes pose, framing and background — not breed. Two
Shepherds in different poses encode less alike than a Shepherd and a Husky in the same pose.
Silhouette failed for a related reason: Otsu thresholding on a natural photograph segments
light from dark, not dog from grass.

Fine-grained visual classification is the one problem hand-designed features have never solved.
That is precisely why convolutional networks exist, and no amount of tuning this encoder closes
the gap.

## What this machine actually does well

Everything that was measured separately still holds, and none of it depends on vision:

- one-shot learning: **100 classes from one example each, 20% noise, 100% correct**
- abstraction: prototypes converge on categories never shown (0.001 at 60 instances)
- concept formation: 240 experiences of 12 unnamed categories → **exactly 12 concepts**
- fact binding: **6/6**, and it is the one part of the dog test that worked perfectly
- certainty: 26.8σ at 45% corruption across 11 nodes

## The path, if breed-level vision is wanted

Hand-designed features are out. Pretrained models are out by your rule. That leaves **learning
features from scratch**, which is what `bench_bitslice.c` — the ternary RBM built earlier in this
project — is for: an RBM learns edge and texture detectors from unlabelled images, and those
learned features replace the hand-designed encoder.

Two honest caveats: feature learning needs far more than 22 photographs, and even then
breed-level discrimination is not guaranteed. It is a real path, not a promise.

**Nothing else in the system is blocked by this.** Vision is one input among several, and the
74HC fabric's 152 simultaneous channels — which the machine reads perfectly — is the input this
hardware was actually built around.
