#!/usr/bin/env python3
"""
intent.py -- turn a spoken sentence into a structured command.

DESIGN DECISION: this is a grammar, not a language model.

A driving command sits between you and a moving car. A small LLM on 4 GB would
be slower, hungrier, non-deterministic, and able to hallucinate a destination.
A grammar over a bounded vocabulary is instant, uses no RAM, and fails LOUDLY
(returns unknown) instead of confidently inventing something.

An LLM can be added later as a FALLBACK for free-form phrasing -- but never as
the thing standing between the driver and the route.

Also handles the reality of speech-to-text: whisper mis-hears brand names
constantly, so matching is fuzzy and tolerant of the common substitutions.
"""
import re
import sys
import unicodedata

# ---------------------------------------------------------------- normalising
_FILLER = re.compile(r"\b(uh|um|er|please|hey car|ok car|okay car)\b", re.I)
_PUNCT = re.compile(r"[^\w\s]")


def normalise(s):
    s = unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode()
    s = s.lower()
    s = _FILLER.sub(" ", s)
    s = _PUNCT.sub(" ", s)
    return re.sub(r"\s+", " ", s).strip()


# ---------------------------------------------------------------- the grammar
NAV_VERBS = r"(?:take me to|navigate to|drive to|directions to|route to|go to|find|where is|nearest|closest)"
SUPERLATIVE = r"(?:closest|nearest|next)"

# Category words the driver is likely to use, mapped to OSM-ish categories so we
# can fall back to a category search when a brand name isn't recognised.
CATEGORY_WORDS = {
    "gas": "amenity=fuel", "gas station": "amenity=fuel", "petrol": "amenity=fuel",
    "fuel": "amenity=fuel", "charger": "amenity=charging_station",
    "charging station": "amenity=charging_station", "supercharger": "amenity=charging_station",
    "food": "amenity=restaurant", "restaurant": "amenity=restaurant",
    "coffee": "amenity=cafe", "cafe": "amenity=cafe",
    "hospital": "amenity=hospital", "pharmacy": "amenity=pharmacy",
    "atm": "amenity=atm", "bank": "amenity=bank",
    "parking": "amenity=parking", "hotel": "tourism=hotel",
    "grocery": "shop=supermarket", "supermarket": "shop=supermarket",
    "store": None, "shop": None,
    "pet store": "shop=pet", "pet shop": "shop=pet",
    "hardware": "shop=hardware", "car wash": "amenity=car_wash",
}

# Whisper mis-hearings seen in the wild for common brands. Cheap insurance:
# a driver saying "Petco" should not fail because the model wrote "pet co".
PHONETIC_FIX = {
    "pet co": "petco", "pet-co": "petco", "pepco": "petco", "pet go": "petco",
    "pet smart": "petsmart", "pets mart": "petsmart",
    "star bucks": "starbucks", "mac donalds": "mcdonalds",
    "mcdonald s": "mcdonalds", "home depo": "home depot",
    "wal mart": "walmart", "seven eleven": "7 eleven",
}

CONTROL = {
    r"\b(stop|cancel|never ?mind|abort)\b": ("cancel", None),
    r"\b(go home|take me home|navigate home)\b": ("navigate_home", None),
    r"\b(where am i|current location)\b": ("where_am_i", None),
    r"\b(how far|distance|eta|how long)\b": ("eta", None),
    r"\b(volume up|louder)\b": ("volume_up", None),
    r"\b(volume down|quieter)\b": ("volume_down", None),
    r"\b(open the vent|vent open)\b": ("vent_open", None),
    r"\b(close the vent|vent closed?)\b": ("vent_close", None),
    r"\b(battery|state of charge|how much charge)\b": ("battery_status", None),
    r"\b(range|how far can i go)\b": ("range_status", None),
}


def _apply_phonetic(s):
    # Word-boundary matching, NOT naive substring replace. Without \b, the entry
    # "home depo" -> "home depot" fires on the already-correct "home depot" and
    # produces "home depott". Caught by the selftest.
    for wrong, right in PHONETIC_FIX.items():
        s = re.sub(r"\b" + re.escape(wrong) + r"\b", right, s)
    return s


def parse(utterance):
    """Return a dict. action == 'unknown' means: say so, do NOT guess."""
    raw = utterance
    s = _apply_phonetic(normalise(utterance))
    if not s:
        return {"action": "unknown", "raw": raw, "why": "empty"}

    for pat, (action, _) in CONTROL.items():
        if re.search(pat, s):
            return {"action": action, "raw": raw, "text": s}

    m = re.search(NAV_VERBS + r"\s+(?:the\s+)?(?:a\s+)?(.*)", s)
    if not m:
        return {"action": "unknown", "raw": raw, "text": s,
                "why": "no navigation verb"}

    tail = m.group(1).strip()
    nearest = bool(re.search(SUPERLATIVE, s))
    tail = re.sub(r"^(?:" + SUPERLATIVE + r")\s+", "", tail).strip()
    tail = re.sub(r"\b(store|shop)$", "", tail).strip() or tail

    if not tail:
        return {"action": "unknown", "raw": raw, "text": s,
                "why": "no destination"}

    # Category match wins only on an exact phrase; otherwise treat as a
    # name/brand and let the POI index do fuzzy matching.
    cat = CATEGORY_WORDS.get(tail)
    return {
        "action": "navigate",
        "target": tail,
        "category": cat,
        "nearest": nearest or cat is not None,
        "raw": raw,
        "text": s,
    }


# -------------------------------------------------------------------- selftest
CASES = [
    ("hey car take me to the closest Petco",   "navigate", "petco", True),
    ("take me to the nearest gas station",     "navigate", "gas station", True),
    ("navigate to Home Depot",                 "navigate", "home depot", False),
    ("drive to the closest pet co",            "navigate", "petco", True),   # whisper slip
    ("uh, find the nearest coffee",            "navigate", "coffee", True),
    ("where is the closest hospital",          "navigate", "hospital", True),
    ("go home",                                "navigate_home", None, None),
    ("cancel",                                 "cancel", None, None),
    ("how much charge do I have",              "battery_status", None, None),
    ("close the vent",                         "vent_close", None, None),
    ("what's the weather like",                "unknown", None, None),
    ("",                                       "unknown", None, None),
]


def selftest():
    bad = 0
    for text, want_action, want_target, want_near in CASES:
        got = parse(text)
        ok = got["action"] == want_action
        if ok and want_target is not None:
            ok = got.get("target") == want_target
        if ok and want_near is not None:
            ok = got.get("nearest") == want_near
        flag = "ok " if ok else "FAIL"
        if not ok:
            bad += 1
        print(f"  [{flag}] {text!r:45} -> {got['action']:<15} "
              f"target={got.get('target')!r} nearest={got.get('nearest')}")
    print(f"\n{len(CASES)-bad}/{len(CASES)} passed")
    return bad == 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        sys.exit(0 if selftest() else 1)
    for line in sys.stdin:
        print(parse(line.strip()))
