#!/usr/bin/env python3
"""Deterministic formatting and idempotence regressions for lexical unary ops."""
import argparse
import importlib.util
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--formatter', required=True)
    args = ap.parse_args()
    spec = importlib.util.spec_from_file_location('asterfmt_test', args.formatter)
    if spec is None or spec.loader is None:
        raise RuntimeError('formatter module is not available')
    # Dataclasses use sys.modules for type resolution on some Python versions.
    import sys
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    cases = [
        ('if ! true {', 'if !true {'),
        ('if !true {', 'if !true {'),
        ('if !!ready {', 'if !!ready {'),
        ('while ! done {', 'while !done {'),
        ('if !(a == b) {', 'if !(a == b) {'),
        ('if a != b {', 'if a != b {'),
        ('if a && ! b {', 'if a && !b {'),
        ('return ! false;', 'return !false;'),
        ('return ~ value;', 'return ~value;'),
        ('if ! a {', 'if !a {'),
    ]
    for before, after in cases:
        formatted = module.format_source(before)
        if after not in formatted:
            raise AssertionError(f'{before!r} -> {formatted!r}, expected {after!r}')
        if module.format_source(formatted) != formatted:
            raise AssertionError(f'formatter is not idempotent: {before!r}')
    print(f'formatter: {len(cases)} unary and operator checks passed')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
