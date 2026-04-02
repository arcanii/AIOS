#!/usr/bin/env python3
# check_consistency.py - Validate PD consistency across build files
# Usage: python3 tools/check_consistency.py
# Returns exit 0 on success, 1 on mismatch

import os, re, sys, json
import xml.etree.ElementTree as ET

errors = 0

# -- Makefile PDs --
with open("Makefile", "r") as f:
    makefile = f.read()
m = re.search(r"^PDS\s*:=\s*(.+?)$", makefile, re.MULTILINE)
makefile_pds = set(m.group(1).split()) if m else set()

# -- aios.system PDs --
tree = ET.parse("aios.system")
root = tree.getroot()
xml_pds = set()
for pd in root.iter("protection_domain"):
    xml_pds.add(pd.get("name"))

# -- aios_system.json PDs --
with open("aios_system.json", "r") as f:
    jdata = json.loads(f.read())
json_pds = set()
def collect_pds(pd_list):
    for pd in pd_list:
        json_pds.add(pd["name"])
        if "children" in pd:
            collect_pds(pd["children"])
collect_pds(jdata.get("protection_domains", []))

def check_pair(name_a, set_a, name_b, set_b):
    global errors
    diff_ab = set_a - set_b
    diff_ba = set_b - set_a
    if diff_ab:
        print(f"  ERROR: In {name_a} but not {name_b}: {diff_ab}")
        errors += 1
    if diff_ba:
        print(f"  ERROR: In {name_b} but not {name_a}: {diff_ba}")
        errors += 1

print("PD Consistency Check")
print(f"  Makefile: {len(makefile_pds)} PDs")
print(f"  aios.system: {len(xml_pds)} PDs")
print(f"  aios_system.json: {len(json_pds)} PDs")

check_pair("Makefile", makefile_pds, "aios.system", xml_pds)
check_pair("aios.system", xml_pds, "aios_system.json", json_pds)

# -- Channel endpoint validation --
for i, ch in enumerate(jdata.get("channels", [])):
    for end in ch.get("ends", []):
        pd_name = end.get("pd")
        if pd_name not in json_pds:
            print(f"  ERROR: Channel {i} references unknown PD: {pd_name}")
            errors += 1

if errors == 0:
    print("  ALL CHECKS PASSED")
    sys.exit(0)
else:
    print(f"  {errors} error(s) found")
    sys.exit(1)
