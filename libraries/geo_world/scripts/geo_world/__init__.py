"""geo_world: real places (DEM + OpenStreetMap) as prebuilt world packages.

tools/geo_import.py builds a package offline from a region folder; world.py loads it in the game
and answers the world queries (height, roads, buildings, passability) and keeps the mutable
condition of buildings, roads and bridges. Package format: FORMAT.md.
"""
