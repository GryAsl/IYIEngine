"""Country locations, languages and regions, independent of the engine."""

COUNTRIES = {
    "us": {'lon': -98, 'lat': 39, 'lang': 'en', 'region': 'na'},
    "ca": {'lon': -106, 'lat': 56, 'lang': 'en', 'region': 'na'},
    "br": {'lon': -51, 'lat': -12, 'lang': 'pt', 'region': 'sa'},
    "gb": {'lon': -2, 'lat': 53, 'lang': 'en', 'region': 'eu'},
    "de": {'lon': 10, 'lat': 51, 'lang': 'de', 'region': 'eu'},
    "fr": {'lon': 2, 'lat': 46, 'lang': 'fr', 'region': 'eu'},
    "es": {'lon': -4, 'lat': 40, 'lang': 'es', 'region': 'eu'},
    "it": {'lon': 12, 'lat': 43, 'lang': 'it', 'region': 'eu'},
    "se": {'lon': 16, 'lat': 62, 'lang': 'sv', 'region': 'eu'},
    "tr": {'lon': 35, 'lat': 39, 'lang': 'tr', 'region': 'eu'},
    "in": {'lon': 79, 'lat': 22, 'lang': 'en', 'region': 'as'},
    "cn": {'lon': 104, 'lat': 35, 'lang': 'zh', 'region': 'as'},
    "kr": {'lon': 128, 'lat': 36, 'lang': 'ko', 'region': 'as'},
    "jp": {'lon': 138, 'lat': 37, 'lang': 'ja', 'region': 'as'},
    "au": {'lon': 134, 'lat': -25, 'lang': 'en', 'region': 'oc'},
}

REGIONS = ("na", "sa", "eu", "af", "as", "oc")


def map_point(country_id: str) -> tuple[float, float]:
    """Pin position on the flat map image, 0..1 (equirectangular, latitude 75N..55S)."""
    c = COUNTRIES[country_id]
    return (c["lon"] + 180.0) / 360.0, (75.0 - c["lat"]) / 130.0
