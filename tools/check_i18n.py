"""Validate flat UTF-8 catalogs and generate immutable UTF-16 C++ resources."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import string
import sys


KEY = re.compile(r"[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+\Z")
PARAMETER = re.compile(r"[a-z][a-z0-9_]*\Z")


class CatalogError(ValueError):
    pass


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise CatalogError(f"Duplicate key: {key}")
        result[key] = value
    return result


def parameters(text: str) -> set[str]:
    result = set()
    try:
        for _, field, spec, conversion in string.Formatter().parse(text):
            if field is None:
                continue
            if not PARAMETER.fullmatch(field) or spec or conversion:
                raise CatalogError(f"Invalid named placeholder: {field}")
            result.add(field)
    except ValueError as error:
        raise CatalogError(f"Invalid placeholder syntax: {error}") from error
    return result


def read_catalog(path: Path) -> dict[str, str]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_object)
    except (OSError, UnicodeError, ValueError) as error:
        raise CatalogError(f"{path.name}: {error}") from error
    if not isinstance(data, dict) or not data:
        raise CatalogError(f"{path.name}: expected a nonempty flat object")
    for key, value in data.items():
        if not KEY.fullmatch(key):
            raise CatalogError(f"{path.name}: invalid key {key!r}")
        if not isinstance(value, str) or not value.strip() or "\0" in value:
            raise CatalogError(f"{path.name}: empty or invalid translation for {key}")
        try:
            value.encode("utf-16-le")
            parameters(value)
        except (UnicodeError, CatalogError) as error:
            raise CatalogError(f"{path.name}: {key}: {error}") from error
    identifiers = [key.replace(".", "_") for key in data]
    if len(set(identifiers)) != len(identifiers):
        raise CatalogError(f"{path.name}: generated C++ key collision")
    return data


def validate(directory: Path, allow_incomplete: bool = False):
    english = read_catalog(directory / "en-US.json")
    chinese = read_catalog(directory / "zh-CN.json")
    missing, extra = english.keys() - chinese.keys(), chinese.keys() - english.keys()
    if extra or (missing and not allow_incomplete):
        raise CatalogError(f"Catalog keys differ: missing={sorted(missing)}, extra={sorted(extra)}")
    for key in english.keys() & chinese.keys():
        if parameters(english[key]) != parameters(chinese[key]):
            raise CatalogError(f"Placeholder mismatch: {key}")
    if missing:
        print(f"Warning: {len(missing)} Chinese translations fall back to English", file=sys.stderr)
    return english, chinese


def wide(text: str) -> str:
    units = text.encode("utf-16-le")
    return 'L"' + "".join(f"\\x{units[i] | units[i + 1] << 8:04x}" for i in range(0, len(units), 2)) + '"'


def write_if_changed(path: Path, text: str):
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8", newline="\n")


def generate(output: Path, english: dict[str, str], chinese: dict[str, str]):
    output.mkdir(parents=True, exist_ok=True)
    keys = sorted(english)
    header = "// Generated from resources/i18n. Do not edit.\n#pragma once\n#include <cstddef>\n"
    header += "namespace cliprope::localization {\nenum class TextKey : std::size_t {\n"
    header += "".join(f"    {key.replace('.', '_')},\n" for key in keys)
    header += "    count\n};\n}\n"
    source = '// Generated from resources/i18n. Do not edit.\n#include "localization/localization.hpp"\n'
    source += "namespace cliprope::localization {\nnamespace {\nconstexpr Translation entries[]{\n"
    for key in keys:
        source += f'    {{"{key}", {wide(english[key])}, {wide(chinese.get(key, ""))}, {wide("[" + key + "]")}}},\n'
    source += "};\nstatic_assert(std::size(entries) == static_cast<std::size_t>(TextKey::count));\n}\n"
    source += "std::span<const Translation> builtin_catalog() noexcept { return entries; }\n}\n"
    write_if_changed(output / "translation_keys.hpp", header)
    write_if_changed(output / "translation_catalog.cpp", source)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resources", type=Path, default=Path(__file__).resolve().parents[1] / "resources/i18n")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--allow-incomplete", action="store_true")
    args = parser.parse_args()
    try:
        english, chinese = validate(args.resources, args.allow_incomplete)
        if args.output:
            generate(args.output, english, chinese)
        print(f"Validated {len(english)} English and {len(chinese)} Chinese translations")
        return 0
    except CatalogError as error:
        print(f"i18n validation failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
