# geo_world package format (version 1)

A package is a folder a game ships under `assets/` (e.g. `assets/maps/jowhar/`), built offline by
`tools/geo_import.py` from a region folder and read by `scripts/geo_world/world.py`. The package
never changes at runtime; building, road and bridge conditions are kept beside it in the world dict.

## Making one

```bat
python -I tools\geo_import.py fetch <region dir>              :: Overpass + Terrarium tiles (internet)
python -I tools\geo_import.py build <region dir> <out dir>    :: the package (offline, ~10 s)
python -I tools\geo_import.py check <region dir>              :: two builds, identical bytes
python -I tools\geo_zones.py <out dir>                        :: zones.json (below), after every build
```

The region folder is committed with its downloaded sources, so a build is the same every time:

| File | What |
|---|---|
| `region.json` | `name`, `title`, centre `lat`/`lon`, `half` (metres from centre to edge), `cell` (terrain cell, m), `grid` (passability cell, m), `texel` (ground picture, m), `dem_zoom`, `dem_smooth` (3x3 box passes against DEM speckle), `steep_deg`, optional `palette` (`{"farmland": [r, g, b]}`), `seasons`, `vegetation`, `tree_kinds`, `homesteads`, `tall_grass`, `road_wear` (each replaces the importer's keys) |
| `overrides.json` | manual authoring over the sources (below) |
| `osm.json.gz` | the Overpass answer, written by `fetch` |
| `dem/z_x_y.png` | Terrarium elevation tiles, written by `fetch` |

Never edit the sources; fix data in `overrides.json`:

```json
{
  "buildings": {"b:w123": {"kind": "school", "levels": 2, "height": 7.5, "important": true, "remove": true}},
  "add_buildings": [{"id": "x:checkpoint1", "lonlat": [[45.50, 2.78], ...], "tags": {"building": "yes"}}],
  "roads": {"w456": {"remove": true, "kind": "track", "width": 4, "bridge": true}},
  "points": [{"id": "x:base_gate", "kind": "entry", "lonlat": [45.49, 2.76], "name": "", "civilian": false}],
  "remove_points": ["p:n789"],
  "walls": [{"id": "x:compound1", "lonlat": [[45.50, 2.78], [45.501, 2.78]], "height": 2.2}]
}
```

OSM has almost no compound walls, so walls come only from `walls` (height defaults to 2.2 m).
Trees are generated, not authored (`VEGETATION` and `TREE_KINDS` in the importer; region.json
`vegetation` replaces any key). Every `natural=tree` node is a tree. Orchards plant rows along their
long side with a few gaps. Everywhere else a density field (trees per hectare) is sampled
stratified with a minimum spacing: woods and scrub at full density that thins out over `fade`
metres past their edge (no hard stop where the green ends), riparian trees near water, yard trees
in town, a sparse scatter on open ground, each broken up by clumps and clearings (`clumps`). Each
area class mixes kinds (acacia, mango, neem, palm, banana, bush), each with its height, crown and
crown base range. Building, road, water and wall cells stay free. The same sources give the same
trees.

Homesteads (`HOMESTEADS`, region.json `homesteads`): most outlying small houses (few others close
by) get a compound hedge, thorn branches (zariba) or euphorbia (cactus), a wavy ring a few metres
out from the house, open toward the nearest road. Its cells carry WALL (closed to movement) except
the gate's, so a walker always gets into the yard; for sight it is leaves. A hedge that would cross a road, water, another building or hedge is not built.
Yard props (water tank, laundry line, charcoal sacks, cart, goats) stand round those houses on free
cells. Tall grass (`TALL_GRASS`, region.json `tall_grass`): patches over part of the grass, scrub
and wetland land, with sparse tufts in them; it hides a person lying down. The ground picture also
gets earth bunds round farmland plots, wheel ruts on unpaved roads (puddles in them in the wet
picture) and potholes on paved ones (`ROAD_WEAR`).

## Frame

Metres; x east, y south (screen-down at rotation 0, as the engine's ground), z up from
`frame.base_elevation` (the lowest DEM vertex, metres above sea level). The map is the square
`[-half, half]` on both axes round the centre. WGS84 lon/lat <-> world: `world.to_lonlat` /
`from_lonlat` (equirectangular with the ellipsoid's radii at the centre: centimetres over a few km).

## Files

| File | Content |
|---|---|
| `world.json` | everything below, UTF-8 JSON |
| `heights.bin` | float32 little-endian, `(columns + 1) * (rows + 1)` DEM vertex heights, row by row from the north-west corner |
| `grid.bin` | one byte per passability cell, row by row from the north-west corner, bits in `grid.bits` |
| `ground.png` | RGB ground picture over `ground.area`, **south row first** (as a render texture is stored, which is how `sprite_stack` `fx.draw_terrain` reads it): land use with ragged, fading edges, farmland rows, soil and dry grass patches, tone noise, darker under crowns |
| `ground_<season>.png` | the same picture for another season (`SEASONS` in the importer: palette changes, today `dry`) |

## world.json

| Key | Content |
|---|---|
| `format`, `version` | `"geo_world"`, `1` |
| `name`, `title` | region name, human title |
| `frame` | `lat`, `lon`, `half`, `base_elevation`, `axes`, `projection` |
| `terrain` | `cell`, `columns`, `rows`, `origin` (north-west corner), `file` |
| `grid` | `cell`, `columns`, `rows`, `origin`, `file`, `bits`: water 1, steep 2, road 4, bridge 8, building 16, rubble 32, wall 64 |
| `ground` | `file`, `area` [x, y, width, height], `seasons` {season: file} |
| `sources` | attribution, OSM timestamp, DEM name, `hash` (SHA-256 of every source file) |
| `nodes` | `{id: [x, y]}` road graph nodes: `n<osm node>` junctions and ends, `e<way>.<piece><a/b>` where a road leaves the map |
| `roads` | edges between nodes: `id` (`w<way>.<k>`), `a`, `b`, `kind` (OSM highway), `width`, `paved`, `bridge`, `tunnel`, `layer`, `oneway`, `name`, `length`, `points` |
| `buildings` | `id` (`b:w<way>`, `b:r<relation>.<k>`, override ids), `kind` (house, hut, commercial, industrial, worship, school, health, government), `important`, `levels`, `height`, `height_source` (height, levels, estimate, override), `name`, `footprint` (counter-clockwise on screen), `obb` [cx, cy, length, width, degrees], `boxes` (up to 4 obbs covering an L/U/T footprint; the static obstacle extents) |
| `points` | `id`, `kind` (settlement, market, worship, school, health, government, fuel, bridge, entry), `x`, `y`, `name`, `civilian`, `ref` (building or road id) |
| `walls` | `id`, `points` (a polyline in world metres), `height`; their cells carry WALL (closed to movement) |
| `trees` | `[x, y, height, crown radius, crown base, kind]` per tree, sorted; no grid bit (the trunk blocks sight up to the crown base, the crown is leaves: `foliage(w)`) |
| `hedges` | `points` (an open polyline: the gap is the gate), `height`, `kind` (zariba, cactus), `building` (the house it rings); their cells carry WALL |
| `props` | `[x, y, z, rotation, kind]` per yard prop (water_tank, laundry, charcoal, cart, goat_0..2) |
| `tall_grass` | one ring per tall-grass patch |
| `tufts` | `[x, y]` per tall-grass tuft, sorted |
| `fields` | one outer ring per farmland area (OSM `landuse=farmland`), for games that put farm life on them |
| `issues` | what the importer dropped, simplified, estimated or found suspicious (below) |

IDs come from OSM ids and overrides only, so the same sources give the same ids and a save game
can name them.

## zones.json (optional, beside world.json)

Named neighbourhoods for games that keep numbers per area (people, activity, fear, support). Built
offline from a finished package, never edited: `python -I tools\geo_zones.py <package dir>` writes
it, `... check <package dir>` asserts it against the package. Same package, same bytes.

```json
{"format": "geo_zones", "version": 1, "package": "jowhar", "source_hash": "<world.json sources.hash>",
 "cell": 50, "origin": [-2000, -2000], "columns": 80, "rows": 80, "grid": [0, 0, -1, 3, ...],
 "zones": [{"id": "z0", "name": "Biyasa", "x": 650.1, "y": 900.4, "buildings": ["b:w1", ...], "homes": 1419,
            "people": 8514, "markets": [[x, y]], "mosques": [[x, y]], "schools": [[x, y]]}]}
```

`grid`: one zone index (into `zones`, so `z7` is 7) per `cell`-metre square, row by row from the
north-west `origin`, `-1` = no zone. A square gets the zone of the nearest home within 150 m of its
centre. `buildings`: every building (any kind) whose centre is within 150 m of a home of the zone
(homes are in their own zone), each id in one zone only. `homes` counts `house` buildings; `people`
is `homes * 6` (household size, an estimate). `markets`, `mosques`, `schools`: point and building
positions of those kinds (a building within 25 m of a point of its kind is the same place). Names are
the OSM settlement name, else `<nearest named zone> <compass direction>` (unique). Seeds are
`settlement` points (homes within 700 m), leftover homes bin into 500 m squares, zones under 60
homes merge into the nearest.

## Validation (issues)

| Kind | Handling |
|---|---|
| `building_dropped` | not closed, degenerate, under 4 m2, or self-intersecting: left out |
| `building_simplified` | multipolygon building: outer rings kept, holes ignored |
| `building_type_unknown` | unknown `building=` value: treated as a house |
| `building_trimmed` | an OSM outline drawn over a street (it leaves the road under `BLOCKED_FREE` m free somewhere): the strip within `CLEAR_CORE` / 2 of the centre line is cut off (`_clear_streets`; footpaths and bridges are left alone) |
| `building_off_road` | such a building whose middle is past the road's centre line, or that would keep under `TRIM_KEEP` of itself: removed |
| `heights_estimated` | no `height` / `building:levels`: 1 storey of 3.2 m |
| `road_dropped` | way without geometry |
| `road_crossing_without_junction` | two roads cross on one level with no shared node: reported, not joined |
| `settlement_without_buildings` | a named place with no building within 300 m: OSM is missing them; add them in overrides |
| `steep_cells`, `dem` | terrain cells over `steep_deg`; a DEM range that looks broken |

## Runtime (world.py)

The runtime is C++ (`src/World.h`, `GeoWorld`, built into the engine); `world.py` wraps it, and its
checks are `TestGeoWorld` in the engine tests (`build\ninja\IYIEngineTests.exe`).

Mutable state: `set_condition(w, id, "intact" | "damaged" | "destroyed" | "cleared")` on a building or road ("cleared": a destroyed building's rubble taken away, its cells neither building nor rubble, nothing blocks sight).
A building's cells (`building_cells`) are those whose centres lie in its footprint, or for a hut
smaller than a cell the one under its centre. A destroyed building turns its cells from BUILDING to RUBBLE (cells a standing neighbour shares stay
BUILDING); a destroyed bridge drops its BRIDGE bits, leaving WATER; `road_open` is false for a
destroyed road. Each change bumps `w["revision"]` and is listed by `changes_since(w, revision)`, so
navigation and the renderer update only what changed. `blast(w, x, y, radius)`: trees within half
the radius snap (`w["tree_state"][index] = "broken"`, `BROKEN_TRUNK` of the trunk left, no crown),
the rest within it burn bare ("burnt", no leaves), and `w["scorches"]` gets `[x, y, radius / 3]`;
it bumps the revision without a change entry (no grid bit changes). `set_damage(w, id, amount)`
gives a building a damage 0..1 beside its condition (`w["damage"]`, read by `damage(w, id)`): how
far its stack crumbles, looks only, so it too bumps the revision without a change entry.
`add_mark(w, kind, x, y, length, width, rotation, strength, time)` appends a ground mark to
`w["marks"]` (`[kind index into MARKS ("crater", "track", "rubble", "blood"), x, y, length, width,
rotation, strength, time]`, looks only, no revision bump); `set_marks(w, marks)` replaces them and
bumps `w["marks_gen"]`, so a renderer redraws them all (between calls the list only grows).
`save_state` / `load_state` carry the conditions, damage, tree states, scorches and marks in a save game;
`set_trees(w, tree_state, scorches)` replaces the last two (`{}, []` starts a scenario on unharmed
trees) and redoes sight round every old and new scorch. Who decides what: navigation reads `flags` and the road graph, combat
calls `set_condition`, the game decides what may be rebuilt.

Line of sight: `blocker_heights(w)` is one float per grid cell, the top of whatever stands there (a
building's height, `RUBBLE_HEIGHT` for rubble, the highest wall or tree crown base (the trunk)
over the cell, 0 for open ground; walls and trees survive building changes);
`foliage(w)` is two float lists `(lows, highs)` per cell, the band that hides without blocking (a
sight line inside it is concealed; `SightGrid.set_foliage`): the leafy crowns over the cell,
widened over a hedge (ground to its height); where neither is, tall grass (ground to
`GRASS_HEIGHT`, so it hides a person lying down but not one standing). One band per cell, so a crown
over tall grass keeps the crown's. `sight_grid(w)` builds an `engine.SightGrid` from both and the
terrain (the cursor ray, who sees whom); after building changes and blasts, `update_sight(w, sight,
revision)` rewrites only the changed cells (the trunks and leaf bands round a blast, then `set_foliage`)
and returns the new revision. A game's native object with the same methods (a module holding its
own SightGrid) is updated the same way.
