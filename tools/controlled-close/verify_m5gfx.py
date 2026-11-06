"""Read-only guard for the project's exact managed M5GFX version.

Accept only the current simple scalar manifest format; this is not a YAML
parser or a substitute for the IDF dependency solver.
"""
import argparse
import json
import re
from pathlib import Path

EXPECTED = "0.2.36"


def scalar(text, key, indent):
    matches = list(re.finditer(r"^([ \t]*)" + re.escape(key) + r":([^\r\n]*)$", text, re.M))
    if len(matches) != 1 or matches[0][1] != " " * indent:
        raise ValueError("missing, duplicate or misplaced " + key)
    value = re.fullmatch(r'''\s*(?:"([^"\r\n]*)"|'([^'\r\n]*)'|([^#\s]+))\s*(?:\#.*)?''', matches[0][2])
    if not value:
        raise ValueError("unsupported scalar format for " + key)
    return next(v for v in value.groups() if v is not None)


def verify(project):
    root = Path(project)
    main = (root / "main/idf_component.yml").read_text(encoding="utf-8")
    # The project manifest owns this direct, exact dependency. Do not permit
    # a nested key to masquerade as its replacement.
    if len(re.findall(r"^dependencies:\s*(?:#.*)?$", main, re.M)) != 1:
        raise ValueError("missing or ambiguous dependency mapping")
    if scalar(main, "m5stack/m5gfx", 2) != EXPECTED:
        raise ValueError("project M5GFX dependency is not pinned to " + EXPECTED)
    metadata = (root / "managed_components/m5stack__m5gfx/idf_component.yml").read_text(encoding="utf-8")
    if scalar(metadata, "version", 0) != EXPECTED:
        raise ValueError("prepared managed M5GFX version differs from " + EXPECTED)
    return {"status": "verified", "managed_m5gfx_version": EXPECTED,
            "read_only": True, "other_dependencies_verified": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = verify(args.project)
    except (OSError, UnicodeError, ValueError) as error:
        parser.exit(1, "M5GFX version guard failed: " + str(error) + "\n")
    print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
