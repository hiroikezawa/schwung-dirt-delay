#!/usr/bin/env python3
"""Embed module.json's contract in the DSP; runtime callbacks never read files."""
import json
import pathlib
import sys
root = pathlib.Path(__file__).resolve().parent.parent
caps = json.loads((root / "module.json").read_text())["capabilities"]
# The engine's key set (dirt_engine.h). The knob ORDER is module.json's alone:
# params are addressed by key, so the two orders need not agree.
keys = ["mix", "gain", "drive", "time", "feedback", "crush", "fx1", "fx2"]
assert sorted(p["key"] for p in caps["chain_params"]) == sorted(keys), "chain_params keys must match dirt_engine.h"
def literal(value):
    return json.dumps(json.dumps(value, separators=(",", ":")), ensure_ascii=True)
pathlib.Path(sys.argv[1]).write_text("\n".join([
    "/* Generated at build time from module.json; edit that instead. */",
    "static const char chain_params[] = " + literal(caps["chain_params"]) + ";",
    "static const char ui_hierarchy[] = " + literal(caps["ui_hierarchy"]) + ";",
]) + "\n")
