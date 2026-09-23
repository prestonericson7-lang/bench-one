#!/usr/bin/env python3
"""
build_poi_index.py -- turn an OpenStreetMap extract into a fast offline POI index.

"take me to the closest Petco" needs two things the routing engine does NOT provide:
    1. find WHAT "Petco" is            -> name/brand lookup
    2. find the CLOSEST one to me      -> spatial query

Valhalla routes between coordinates; it does not search for places. This builds
the search half: a SQLite database with an R-tree spatial index.

WHY SQLITE + R-TREE rather than a search engine:
    * stdlib -- sqlite3 ships with Python, nothing to install on the car
    * the R-tree module makes "nearest within a bounding box" an indexed query
      instead of a scan over every POI in the state
    * a few hundred MB of POIs still answers in milliseconds on the Pi
    * RAM cost is ~nothing, which is the actual scarce resource on a 4 GB board

INPUT: an OSM PBF converted to newline-delimited JSON, or a .osm XML extract.
       Get regional extracts from Geofabrik.

USAGE:
    python build_poi_index.py --osm region.osm --out poi.db
    python build_poi_index.py --selftest        # no data needed
"""
import argparse
import math
import os
import sqlite3
import sys
import xml.etree.ElementTree as ET

# Tags that mark something a driver would actually ask to be taken to.
# Deliberately not "everything" -- a POI index full of benches and postboxes
# makes the nearest-match worse, not better.
USEFUL_KEYS = ("shop", "amenity", "tourism", "leisure", "healthcare",
               "office", "craft", "emergency")
SKIP_VALUES = {"bench", "waste_basket", "recycling", "bicycle_parking",
               "post_box", "telephone", "drinking_water", "shelter",
               "street_lamp", "surveillance", "hunting_stand"}


def schema(db):
    db.executescript("""
        PRAGMA journal_mode = WAL;
        CREATE TABLE IF NOT EXISTS poi (
            id      INTEGER PRIMARY KEY,
            name    TEXT,
            brand   TEXT,
            cat     TEXT,      -- e.g. shop=pet
            lat     REAL,
            lon     REAL
        );
        -- R-tree gives us an indexed bounding-box query instead of a full scan
        CREATE VIRTUAL TABLE IF NOT EXISTS poi_rtree USING rtree(
            id, min_lat, max_lat, min_lon, max_lon
        );
        CREATE INDEX IF NOT EXISTS idx_poi_name  ON poi(name COLLATE NOCASE);
        CREATE INDEX IF NOT EXISTS idx_poi_brand ON poi(brand COLLATE NOCASE);
        CREATE INDEX IF NOT EXISTS idx_poi_cat   ON poi(cat);
    """)


def add_poi(db, pid, name, brand, cat, lat, lon):
    db.execute("INSERT OR REPLACE INTO poi VALUES (?,?,?,?,?,?)",
               (pid, name, brand, cat, lat, lon))
    db.execute("INSERT OR REPLACE INTO poi_rtree VALUES (?,?,?,?,?)",
               (pid, lat, lat, lon, lon))


def ingest_osm_xml(path, db, limit=None):
    """Stream the XML -- a state extract will not fit in RAM if parsed whole."""
    n = 0
    ctx = ET.iterparse(path, events=("start", "end"))
    _, root = next(ctx)
    cur = None
    for ev, el in ctx:
        if ev == "start" and el.tag == "node":
            cur = {"id": int(el.get("id")), "lat": float(el.get("lat")),
                   "lon": float(el.get("lon")), "tags": {}}
        elif ev == "end" and el.tag == "tag" and cur is not None:
            cur["tags"][el.get("k")] = el.get("v")
        elif ev == "end" and el.tag == "node":
            if cur:
                t = cur["tags"]
                cat = None
                for k in USEFUL_KEYS:
                    if k in t and t[k] not in SKIP_VALUES:
                        cat = f"{k}={t[k]}"
                        break
                name = t.get("name")
                brand = t.get("brand") or t.get("operator")
                if cat and (name or brand):
                    add_poi(db, cur["id"], name, brand, cat, cur["lat"], cur["lon"])
                    n += 1
                    if n % 20000 == 0:
                        db.commit()
                        print(f"  {n:,} POIs...", file=sys.stderr)
                    if limit and n >= limit:
                        break
            cur = None
            root.clear()
    db.commit()
    return n


# ----------------------------------------------------------------- query side
def haversine_km(lat1, lon1, lat2, lon2):
    R = 6371.0088
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp = p2 - p1
    dl = math.radians(lon2 - lon1)
    a = math.sin(dp/2)**2 + math.cos(p1)*math.cos(p2)*math.sin(dl/2)**2
    return 2 * R * math.asin(math.sqrt(a))


def find_nearest(db, query, lat, lon, radius_km=40.0, limit=5, category=None):
    """Nearest POI matching `query` by name/brand, OR by `category` when given.

    Category search matters: "take me to the nearest gas station" has no brand
    to match on -- the driver wants ANY fuel station. Without this branch that
    request finds nothing even when one is 200 m away.

    The R-tree pre-filters by bounding box so we only haversine a handful of
    rows. Widening search: if nothing is found close by, expand rather than
    return nothing -- "closest Petco" may legitimately be 30 km away.
    """
    out = []
    r = radius_km
    while r <= 400 and not out:
        dlat = r / 111.0
        dlon = r / (111.0 * max(math.cos(math.radians(lat)), 0.01))
        if category:
            rows = db.execute("""
                SELECT p.id, p.name, p.brand, p.cat, p.lat, p.lon
                FROM poi_rtree t JOIN poi p ON p.id = t.id
                WHERE t.min_lat >= ? AND t.max_lat <= ?
                  AND t.min_lon >= ? AND t.max_lon <= ?
                  AND p.cat = ?
            """, (lat - dlat, lat + dlat, lon - dlon, lon + dlon,
                  category)).fetchall()
        else:
            rows = db.execute("""
                SELECT p.id, p.name, p.brand, p.cat, p.lat, p.lon
                FROM poi_rtree t JOIN poi p ON p.id = t.id
                WHERE t.min_lat >= ? AND t.max_lat <= ?
                  AND t.min_lon >= ? AND t.max_lon <= ?
                  AND (p.name LIKE ? OR p.brand LIKE ?)
            """, (lat - dlat, lat + dlat, lon - dlon, lon + dlon,
                  f"%{query}%", f"%{query}%")).fetchall()
        out = sorted(
            ({"id": i, "name": nm, "brand": br, "cat": c, "lat": la, "lon": lo,
              "km": haversine_km(lat, lon, la, lo)}
             for i, nm, br, c, la, lo in rows),
            key=lambda d: d["km"])[:limit]
        r *= 2
    return out


# -------------------------------------------------------------------- selftest
def selftest():
    print("selftest: building an in-memory index with known POIs...")
    db = sqlite3.connect(":memory:")
    schema(db)
    # A fake city block. Distances chosen so the ordering is unambiguous.
    pts = [
        (1, "Petco", "Petco", "shop=pet",      40.0100, -75.0000),   # ~1.1 km N
        (2, "Petco Supply", "Petco", "shop=pet", 40.0500, -75.0000), # ~5.6 km N
        (3, "PetSmart", "PetSmart", "shop=pet", 40.0020, -75.0000),  # ~0.2 km N
        (4, "Joe Coffee", None, "amenity=cafe", 40.0010, -75.0000),
    ]
    for p in pts:
        add_poi(db, *p)
    db.commit()

    here = (40.0000, -75.0000)
    res = find_nearest(db, "Petco", *here)
    print("  query 'Petco' from 40.0,-75.0 ->")
    for r in res:
        print(f"    {r['name']:<14} {r['cat']:<12} {r['km']:6.2f} km")
    assert res, "no results"
    assert res[0]["id"] == 1, f"expected nearest Petco id=1, got {res[0]['id']}"
    assert res[0]["km"] < res[1]["km"], "results not sorted by distance"
    # PetSmart is physically closer but must NOT match a 'Petco' query
    assert all("PetSmart" not in (r["name"] or "") for r in res), \
        "brand matching leaked a different brand"
    print("  OK: nearest-first ordering correct, no cross-brand leak")

    res2 = find_nearest(db, "coffee", *here)
    print(f"  query 'coffee' -> {len(res2)} hit(s): "
          f"{res2[0]['name'] if res2 else '-'}")
    print("selftest PASSED")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--osm", help=".osm XML extract")
    ap.add_argument("--out", default="poi.db")
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--query"); ap.add_argument("--lat", type=float)
    ap.add_argument("--lon", type=float)
    a = ap.parse_args()

    if a.selftest:
        selftest(); return

    if a.query:
        db = sqlite3.connect(a.out)
        for r in find_nearest(db, a.query, a.lat, a.lon):
            print(f"{r['km']:7.2f} km  {r['name']}  [{r['cat']}]  {r['lat']},{r['lon']}")
        return

    if not a.osm:
        ap.error("need --osm, --query, or --selftest")
    if os.path.exists(a.out):
        os.remove(a.out)
    db = sqlite3.connect(a.out)
    schema(db)
    n = ingest_osm_xml(a.osm, db, a.limit)
    db.execute("ANALYZE"); db.commit()
    print(f"indexed {n:,} POIs -> {a.out} "
          f"({os.path.getsize(a.out)/1e6:.1f} MB)")


if __name__ == "__main__":
    main()
