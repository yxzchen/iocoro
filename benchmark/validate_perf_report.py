#!/usr/bin/env python3

import argparse
import json
import sys
from pathlib import Path

try:
    import jsonschema
except ImportError as exc:  # pragma: no cover - runtime dependency guard
    print("Missing dependency: jsonschema. Install python3-jsonschema.", file=sys.stderr)
    raise SystemExit(2) from exc


def load_json(path: Path) -> object:
    try:
        with path.open("r", encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        print(f"File not found: {path}", file=sys.stderr)
        raise SystemExit(2)
    except json.JSONDecodeError as exc:
        print(f"Invalid JSON in {path}: {exc}", file=sys.stderr)
        raise SystemExit(2)


def format_error(err: jsonschema.ValidationError) -> str:
    if err.absolute_path:
        ptr = "/" + "/".join(str(part) for part in err.absolute_path)
    else:
        ptr = "/"
    return f"{ptr}: {err.message}"


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate perf_report.json against schema")
    parser.add_argument(
        "--schema",
        default="benchmark/perf_report.schema.json",
        help="Path to JSON schema file",
    )
    parser.add_argument(
        "--report",
        default="benchmark/perf_report.json",
        help="Path to perf report JSON file",
    )
    args = parser.parse_args()

    schema_path = Path(args.schema)
    report_path = Path(args.report)
    schema = load_json(schema_path)
    report = load_json(report_path)

    validator = jsonschema.Draft202012Validator(schema)
    errors = sorted(validator.iter_errors(report), key=lambda e: list(e.absolute_path))
    if errors:
        print(f"Schema validation failed for {report_path}:", file=sys.stderr)
        for err in errors:
            print(f"  - {format_error(err)}", file=sys.stderr)
        return 1

    print(f"Schema validation passed: {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
