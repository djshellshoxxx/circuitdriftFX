#!/usr/bin/env python3
"""Validate the planned-feature documents, registry and supplied schema/example.
Standard library only. This validates the JSON Schema keywords actually used by
this package, not arbitrary third-party JSON Schema documents or audio code.
"""
import json
import math
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs/planned-features"

def need(condition, message):
    if not condition:
        raise AssertionError(message)

def pairs(items):
    result = {}
    for key, value in items:
        need(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result

def bad_constant(value):
    raise ValueError(f"non-finite JSON number: {value}")

def read_json(path):
    return json.loads(path.read_text(), object_pairs_hook=pairs,
                      parse_constant=bad_constant)

SUPPORTED = {"$schema", "title", "type", "additionalProperties", "required",
             "properties", "const", "minLength", "maxLength", "pattern",
             "minItems", "maxItems", "uniqueItems", "items", "minProperties",
             "maxProperties", "propertyNames", "minimum", "maximum", "enum"}

def schema_check(value, schema, path="$"):
    need(set(schema) <= SUPPORTED, f"unsupported schema keyword at {path}")
    types = {
        "object": lambda x: isinstance(x, dict),
        "array": lambda x: isinstance(x, list),
        "string": lambda x: isinstance(x, str),
        "number": lambda x: isinstance(x, (int, float)) and not isinstance(x, bool) and math.isfinite(x),
        "integer": lambda x: isinstance(x, int) and not isinstance(x, bool),
        "boolean": lambda x: isinstance(x, bool),
    }
    if "type" in schema:
        need(types[schema["type"]](value), f"type at {path}")
    if "const" in schema:
        need(value == schema["const"], f"const at {path}")
    if "enum" in schema:
        need(value in schema["enum"], f"enum at {path}")
    if isinstance(value, dict):
        need(set(schema.get("required", [])) <= set(value), f"required at {path}")
        need(len(value) >= schema.get("minProperties", 0), f"minProperties {path}")
        need(len(value) <= schema.get("maxProperties", math.inf), f"maxProperties {path}")
        props = schema.get("properties", {})
        for key, item in value.items():
            if "propertyNames" in schema:
                schema_check(key, schema["propertyNames"], path + ".<key>")
            if key in props:
                schema_check(item, props[key], path + "." + key)
            else:
                extra = schema.get("additionalProperties", True)
                need(extra is not False, f"additionalProperties {path}.{key}")
                if isinstance(extra, dict):
                    schema_check(item, extra, path + "." + key)
    if isinstance(value, list):
        need(len(value) >= schema.get("minItems", 0), f"minItems {path}")
        need(len(value) <= schema.get("maxItems", math.inf), f"maxItems {path}")
        if schema.get("uniqueItems"):
            need(len({json.dumps(x, sort_keys=True) for x in value}) == len(value), f"uniqueItems {path}")
        if "items" in schema:
            for i, item in enumerate(value):
                schema_check(item, schema["items"], f"{path}[{i}]")
    if isinstance(value, str):
        need(len(value) >= schema.get("minLength", 0), f"minLength {path}")
        need(len(value) <= schema.get("maxLength", math.inf), f"maxLength {path}")
        if "pattern" in schema:
            need(re.search(schema["pattern"], value) is not None, f"pattern {path}")
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        need(math.isfinite(value), f"finite {path}")
        need(value >= schema.get("minimum", -math.inf), f"minimum {path}")
        need(value <= schema.get("maximum", math.inf), f"maximum {path}")

register = (DOCS / "FEATURE_REGISTER.md").read_text()
acceptance = (DOCS / "ACCEPTANCE.md").read_text()
ids = re.findall(r"^\| (CD-F\d\d) \|", register, re.M)
expected = {f"CD-F{i:02}" for i in range(3, 26)}
need(len(ids) == 23 and set(ids) == expected, "feature coverage/duplicate")
tests = set(re.findall(r"^\| (T\d\d) \|", acceptance, re.M))
need(tests == {f"T{i:02}" for i in range(3, 26)}, "test coverage")
ux = re.findall(r"^\| (UX-\d\d) \|", acceptance, re.M)
need(len(ux) == 10 and len(set(ux)) == 10, "UX coverage")
print("PASS: 23 feature mappings, 23 acceptance IDs, 10 UX contracts")

registry = read_json(DOCS / "parameters.json")["parameters"]
need(len({p["id"] for p in registry}) == len(registry), "parameter string IDs")
need(len({p["hostId"] for p in registry}) == len(registry), "parameter host IDs")
for p in registry:
    need(p["featureId"] in expected, f'unknown feature {p["id"]}')
    need(1000 <= p["hostId"] < 2000, f'ID range {p["id"]}')
    need(p["min"] <= p["default"] <= p["max"], f'default {p["id"]}')
    need(p["smoothingMs"] >= 0, f'smoothing {p["id"]}')
    if p["type"] == "enum":
        need(len(p["values"]) == p["max"]-p["min"]+1, f'enum {p["id"]}')
    for u in (0, .25, .5, .75, 1):
        if p["mapping"] == "log":
            need(p["min"] > 0, "positive log minimum")
            x = p["min"] * (p["max"]/p["min"]) ** u
            inverse = math.log(x/p["min"])/math.log(p["max"]/p["min"])
        else:
            x = p["min"] + (p["max"]-p["min"]) * u
            inverse = (x-p["min"])/(p["max"]-p["min"])
        need(abs(inverse-u) < 1e-12, f'mapping roundtrip {p["id"]}')
print(f"PASS: {len(registry)} parameter IDs, defaults, enums and mapping roundtrips")

manifest_schema = read_json(DOCS / "schemas/device-manifest.schema.json")
manifest = read_json(DOCS / "examples/fixture-diode.manifest.json")
schema_check(manifest, manifest_schema)
need(manifest["provenance"]["kind"] == "synthetic-fixture", "example manifest must be labelled synthetic")
print("PASS: synthetic diode manifest example matches device-manifest schema")

schema = read_json(DOCS / "schemas/blueprint.schema.json")
example = read_json(DOCS / "examples/rc-blueprint.json")

def blueprint_check(b):
    schema_check(b, schema)
    need("0" in b["nodes"], "missing ground")
    components = {c["id"]: c for c in b["components"]}
    need(len(components) == len(b["components"]), "duplicate component ID")
    for c in b["components"]:
        need(set(c["pins"].values()) <= set(b["nodes"]), "dangling pin")
        # This example registry is synthetic; not the future production catalogue.
        fields = {"fixture.resistor": "resistanceOhm", "fixture.capacitor": "capacitanceF"}
        need(c["modelId"] in fields, "unknown fixture model")
        field = fields[c["modelId"]]
        need(set(c["parameters"]) == {field} and c["parameters"][field] > 0, "fixture units/positive value")
        need(set(c["pins"]) == {"p", "n"}, "fixture pin signature")
    need(set(b["ports"].values()) <= set(b["nodes"]), "dangling port")
    for binding in b["macroBindings"]:
        need(binding["component"] in components, "dangling binding")

blueprint_check(example)
for mutation in ("dangling", "duplicate", "wrongtype"):
    bad = json.loads(json.dumps(example))
    if mutation == "dangling":
        bad["components"][0]["pins"]["p"] = "unknown"
    elif mutation == "duplicate":
        bad["components"][1]["id"] = bad["components"][0]["id"]
    else:
        bad["components"][0]["parameters"]["resistanceOhm"] = "10000"
    try:
        blueprint_check(bad)
    except AssertionError:
        pass
    else:
        raise AssertionError("negative fixture accepted: " + mutation)
print("PASS: blueprint schema, synthetic fixture semantics and three invalid cases")

# Independent sanity checks of specified design equations, not DSP tests.
temp = 25.0
for _ in range(10000):
    temp = (2/.001*temp + 1 + .2*25) / (2/.001+.2)
expected_temp = 25 + 5*(1-math.exp(-1))
need(abs(temp-expected_temp) < .002, "thermal design tolerance")
for bpm in (60, 120, 180):
    duration = 60/bpm
    tau = duration/math.log(20)
    need(abs(math.exp(-duration/tau)-.05) < 1e-12, "95% recovery equation")
print("PASS: synthetic thermal and tempo-recovery formula checks")

count = 0
for path in ROOT.rglob("*.md"):
    if ".git" in path.parts:
        continue
    for target in re.findall(r"\[[^\]]*\]\(([^)]+)\)", path.read_text()):
        if target.startswith(("http:", "https:", "mailto:", "#", "sandbox:")):
            continue
        local = target.split("#", 1)[0]
        if not local:
            continue
        need((path.parent / local).exists(), f"missing link {path.relative_to(ROOT)} -> {local}")
        count += 1
print(f"PASS: {count} repository-local Markdown link targets")
print("DOCUMENT PACKAGE VALIDATED. No plugin/audio/GPU/host tests were run.")
