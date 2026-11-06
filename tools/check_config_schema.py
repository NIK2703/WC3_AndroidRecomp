#!/usr/bin/env python3
"""Validate config/*.json against config/schema/*.schema.json.

The checker deliberately implements the small JSON Schema subset used by the
WC3 configs (type/required/properties/items/enum/const/pattern/ref) instead of
depending on a third-party package.  Semantic rules stay in the owning tools.

Exit codes: 0 pass, 1 schema violation, 2 unavailable (schema dir missing).
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SCHEMA_FOR = {
    "build_upstream.json": "build_upstream.schema.json",
    "migration.json": "migration.schema.json",
}


def schema_name(config_name: str) -> str | None:
    if config_name.startswith("baseline.") and config_name.endswith(".json"):
        return "baseline.schema.json"
    explicit = SCHEMA_FOR.get(config_name)
    if explicit:
        return explicit
    suffix = ".1.27b.json"
    if config_name.endswith(suffix):
        return config_name[: -len(suffix)] + ".schema.json"
    return None


def pointer(document: object, fragment: str) -> object:
    if not fragment:
        return document
    if not fragment.startswith("/"):
        raise ValueError(f"unsupported JSON pointer fragment: {fragment}")
    node = document
    for raw in fragment[1:].split("/"):
        token = raw.replace("~1", "/").replace("~0", "~")
        if isinstance(node, list):
            node = node[int(token)]
        elif isinstance(node, dict):
            node = node[token]
        else:
            raise KeyError(token)
    return node


class Store:
    def __init__(self, directory: Path):
        self.directory = directory
        self.cache: dict[str, object] = {}

    def load(self, name: str) -> object:
        if name not in self.cache:
            path = self.directory / name
            self.cache[name] = json.loads(path.read_text(encoding="utf-8"))
        return self.cache[name]

    def resolve(self, ref: str, base: str) -> tuple[object, str]:
        if ref.startswith("#"):
            return pointer(self.load(base), ref[1:]), base
        name, _, fragment = ref.partition("#")
        return pointer(self.load(name), fragment), name


def type_ok(value: object, expected: str) -> bool:
    if expected == "object":
        return isinstance(value, dict)
    if expected == "array":
        return isinstance(value, list)
    if expected == "string":
        return isinstance(value, str)
    if expected == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if expected == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if expected == "boolean":
        return isinstance(value, bool)
    if expected == "null":
        return value is None
    return True


def validate(
    value: object,
    schema: object,
    path: str,
    base: str,
    store: Store,
    errors: list[str],
) -> None:
    if schema is True:
        return
    if schema is False:
        errors.append(f"{path}: schema is false")
        return
    if not isinstance(schema, dict):
        errors.append(f"{path}: malformed schema node")
        return

    ref = schema.get("$ref")
    if isinstance(ref, str):
        try:
            target, target_base = store.resolve(ref, base)
        except (KeyError, OSError, ValueError, IndexError) as exc:
            errors.append(f"{path}: cannot resolve {ref}: {exc}")
            return
        validate(value, target, path, target_base, store, errors)
        return

    if "const" in schema and value != schema["const"]:
        errors.append(f"{path}: expected {schema['const']!r}, got {value!r}")
        return
    if "enum" in schema and value not in schema["enum"]:
        errors.append(f"{path}: {value!r} is not one of {schema['enum']!r}")
        return

    expected = schema.get("type")
    if expected is not None:
        types = expected if isinstance(expected, list) else [expected]
        if not any(type_ok(value, item) for item in types):
            errors.append(
                f"{path}: expected {'/'.join(str(t) for t in types)}, "
                f"got {type(value).__name__}"
            )
            return

    for key in ("allOf", "anyOf", "oneOf"):
        branches = schema.get(key)
        if not isinstance(branches, list):
            continue
        branch_errors: list[list[str]] = []
        for branch in branches:
            local: list[str] = []
            validate(value, branch, path, base, store, local)
            branch_errors.append(local)
        if key == "allOf":
            for local in branch_errors:
                errors.extend(local)
        elif key == "anyOf":
            if all(local for local in branch_errors):
                errors.append(f"{path}: no anyOf branch matched")
        elif key == "oneOf":
            matched = sum(1 for local in branch_errors if not local)
            if matched != 1:
                errors.append(f"{path}: expected exactly one oneOf branch, "
                              f"matched {matched}")
        return

    if isinstance(value, str):
        min_len = schema.get("minLength")
        if isinstance(min_len, int) and len(value) < min_len:
            errors.append(f"{path}: shorter than {min_len} characters")
        pattern = schema.get("pattern")
        if isinstance(pattern, str) and re.search(pattern, value) is None:
            errors.append(f"{path}: does not match {pattern}")

    if isinstance(value, (int, float)) and not isinstance(value, bool):
        minimum = schema.get("minimum")
        maximum = schema.get("maximum")
        if isinstance(minimum, (int, float)) and value < minimum:
            errors.append(f"{path}: {value} is below minimum {minimum}")
        if isinstance(maximum, (int, float)) and value > maximum:
            errors.append(f"{path}: {value} is above maximum {maximum}")

    if isinstance(value, list):
        min_items = schema.get("minItems")
        max_items = schema.get("maxItems")
        if isinstance(min_items, int) and len(value) < min_items:
            errors.append(f"{path}: fewer than {min_items} items")
        if isinstance(max_items, int) and len(value) > max_items:
            errors.append(f"{path}: more than {max_items} items")
        items = schema.get("items")
        if isinstance(items, dict):
            for index, item in enumerate(value):
                validate(item, items, f"{path}[{index}]", base, store, errors)

    if isinstance(value, dict):
        required = schema.get("required")
        if isinstance(required, list):
            for key in required:
                if key not in value:
                    errors.append(f"{path}: missing required key {key!r}")
        properties = schema.get("properties")
        if isinstance(properties, dict):
            for key, child in properties.items():
                if key in value:
                    validate(value[key], child, f"{path}.{key}", base, store,
                             errors)
        extra = schema.get("additionalProperties", True)
        if extra is False and isinstance(properties, dict):
            for key in value:
                if key not in properties:
                    errors.append(f"{path}: unexpected key {key!r}")
        elif isinstance(extra, dict):
            known = properties if isinstance(properties, dict) else {}
            for key, child in value.items():
                if key not in known:
                    validate(child, extra, f"{path}.{key}", base, store, errors)


def check(root: Path) -> int:
    schema_dir = root / "config" / "schema"
    if not schema_dir.is_dir():
        print("config schema: unavailable (config/schema/ missing)")
        return 2

    store = Store(schema_dir)
    failures = 0
    checked = 0

    for schema_path in sorted(schema_dir.glob("*.json")):
        try:
            store.load(schema_path.name)
        except (OSError, json.JSONDecodeError) as exc:
            print(f"{schema_path.name}: invalid schema JSON: {exc}")
            failures += 1

    for config_path in sorted((root / "config").glob("*.json")):
        name = config_path.name
        target = schema_name(name)
        if target is None:
            print(f"{name}: no schema mapping")
            failures += 1
            continue
        if not (schema_dir / target).is_file():
            print(f"{name}: schema {target} is missing")
            failures += 1
            continue
        try:
            value = json.loads(config_path.read_text(encoding="utf-8"))
            schema = store.load(target)
        except (OSError, json.JSONDecodeError) as exc:
            print(f"{name}: cannot load: {exc}")
            failures += 1
            continue

        errors: list[str] = []
        validate(value, schema, "$", target, store, errors)
        checked += 1
        for error in errors:
            print(f"{name}: {error}")
        if errors:
            failures += 1

    if failures:
        print(f"config schema: {failures} file(s) failed")
        return 1
    print(f"config schema: OK ({checked} files)")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args(argv)
    return check(args.root.resolve())


if __name__ == "__main__":
    sys.exit(main())
