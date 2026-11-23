"""Renders assets/game_kit/world_map.png from Natural Earth country shapes (public domain, see
assets/game_kit/CREDITS.md). Dev tool, Pillow:

    python tools/make_map.py

Equirectangular, latitude 75N..55S, the projection game_kit.world.map_point() uses.
Market countries get a warm fill; other land stays quiet.
"""

import json
import os

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SRC = os.path.join(ROOT, "tools", "third_party", "natural_earth", "ne_50m_admin_0_countries.geojson")
OUT = os.path.join(ROOT, "assets", "game_kit", "world_map.png")
W, H, SS = 2400, 867, 2          # output size, supersampling
TOP, BOTTOM = 75.0, -55.0
MARKETS = {"USA", "CAN", "BRA", "GBR", "DEU", "FRA", "ESP", "ITA", "SWE", "TUR", "IND", "CHN", "KOR", "JPN", "AUS"}

OCEAN_TOP, OCEAN_BOTTOM = (156, 199, 236), (118, 166, 218)
GRID = (142, 186, 228)
LAND, MARKET = (224, 227, 212), (250, 232, 192)
BORDER, COAST = (178, 172, 154), (120, 140, 150)
SHADOW = (40, 70, 120, 90)


def xy(lon, lat):
    return ((lon + 180.0) / 360.0 * W * SS, (TOP - lat) / (TOP - BOTTOM) * H * SS)


def rings(feature):
    geom = feature["geometry"]
    polys = geom["coordinates"] if geom["type"] == "MultiPolygon" else [geom["coordinates"]]
    for poly in polys:
        yield [xy(lon, lat) for lon, lat in poly[0]]  # outer ring; tiny lakes are dropped


def main():
    with open(SRC, encoding="utf-8") as f:
        features = [ft for ft in json.load(f)["features"] if ft["properties"]["ADM0_A3"] != "ATA"]
    size = (W * SS, H * SS)
    img = Image.new("RGB", size)
    d = ImageDraw.Draw(img)
    for y in range(size[1]):
        t = y / size[1]
        d.line([(0, y), (size[0], y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(OCEAN_TOP, OCEAN_BOTTOM)))
    for lon in range(-180, 181, 30):
        d.line([xy(lon, TOP), xy(lon, BOTTOM)], fill=GRID, width=SS)
    for lat in range(-45, 76, 15):
        d.line([xy(-180, lat), xy(180, lat)], fill=GRID, width=SS)
    shadow = Image.new("RGBA", size, (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadow)
    for ft in features:
        for ring in rings(ft):
            sd.polygon([(x + 3 * SS, y + 4 * SS) for x, y in ring], fill=SHADOW)
    shadow = shadow.filter(ImageFilter.GaussianBlur(4 * SS))
    img.paste(shadow, (0, 0), shadow)
    for ft in features:
        fill = MARKET if ft["properties"]["ADM0_A3"] in MARKETS else LAND
        for ring in rings(ft):
            d.polygon(ring, fill=fill, outline=BORDER, width=SS)
    coast = Image.new("L", size, 0)
    cd = ImageDraw.Draw(coast)
    for ft in features:
        for ring in rings(ft):
            cd.polygon(ring, fill=255)
    edge = coast.filter(ImageFilter.FIND_EDGES).filter(ImageFilter.MaxFilter(3))
    img.paste(Image.new("RGB", size, COAST), (0, 0), edge)
    img.resize((W, H), Image.LANCZOS).save(OUT)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
