#!/usr/bin/env python3
"""system_validate.py
Cross-checks aios_system.json against aios.system (XML) for consistency.
Exits non-zero if divergences are found, so it can gate make system-check.
"""

import json, sys, re
from xml.etree import ElementTree as ET

JSON_PATH = "aios_system.json"
XML_PATH  = "aios.system"

errors = []

def check(cond, msg):
    if not cond:
        errors.append(msg)

with open(JSON_PATH) as f:
    jdata = json.load(f)

tree = ET.parse(XML_PATH)
root = tree.getroot()

json_mr_names = {r["name"] for r in jdata["memory_regions"]}
xml_mr_names  = {mr.get("name") for mr in root.findall("memory_region")}
for name in json_mr_names - xml_mr_names:
    check(False, f"memory_region '{name}' in JSON but not in XML")
for name in xml_mr_names - json_mr_names:
    check(False, f"memory_region '{name}' in XML but not in JSON")

def collect_xml_pds(root):
    result = {}
    for pd in root.findall(".//protection_domain"):
        name = pd.get("name")
        irqs = [(int(irq.get("irq")), int(irq.get("id"))) for irq in pd.findall("irq")]
        result[name] = {"irqs": irqs}
    return result

def collect_json_pds(jdata):
    result = {}
    def walk(pds):
        for pd in pds:
            irqs = [(i["irq"], i["id"]) for i in pd.get("irqs", [])]
            result[pd["name"]] = {"irqs": irqs}
            walk(pd.get("children", []))
    walk(jdata["protection_domains"])
    return result

xml_pds  = collect_xml_pds(root)
json_pds = collect_json_pds(jdata)

json_pd_names = set(json_pds.keys())
xml_pd_names  = set(xml_pds.keys())

for name in json_pd_names - xml_pd_names:
    check(False, f"PD '{name}' in JSON but not in XML")
for name in xml_pd_names - json_pd_names:
    check(False, f"PD '{name}' in XML but not in JSON")

for name in json_pd_names & xml_pd_names:
    j_irqs = sorted(json_pds[name]["irqs"])
    x_irqs = sorted(xml_pds[name]["irqs"])
    check(j_irqs == x_irqs,
          f"PD '{name}' IRQ mismatch: JSON={j_irqs} XML={x_irqs}")

if errors:
    print("system_validate: FAILED")
    for e in errors:
        print(f"  {e}")
    sys.exit(1)
else:
    print(f"system_validate: OK ({len(json_pd_names)} PDs, {len(json_mr_names)} memory regions checked)")
