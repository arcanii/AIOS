#!/usr/bin/env python3
# gen_system_json.py - Generate aios_system.json from aios.system XML
# Usage: python3 tools/gen_system_json.py [input.system] [output.json]
# Single source of truth: aios.system is authoritative.

import sys, json
import xml.etree.ElementTree as ET

def parse_pd(elem):
    pd = {
        "name": elem.get("name"),
        "priority": int(elem.get("priority")),
        "program_image": None,
        "maps": [],
    }
    for attr in ["id", "budget", "period", "cpu", "stack_size"]:
        val = elem.get(attr)
        if val is not None:
            pd[attr] = int(val) if attr in ["budget", "period", "cpu", "id"] else val

    pi = elem.find("program_image")
    if pi is not None:
        pd["program_image"] = pi.get("path")

    for mp in elem.findall("map"):
        pd["maps"].append({
            "mr": mp.get("mr"),
            "vaddr": mp.get("vaddr"),
            "perms": mp.get("perms"),
            "cached": mp.get("cached"),
            "setvar_vaddr": mp.get("setvar_vaddr"),
        })

    irqs = []
    for irq_elem in elem.findall("irq"):
        irqs.append({"irq": int(irq_elem.get("irq")), "id": int(irq_elem.get("id"))})
    if irqs:
        pd["irqs"] = irqs

    children = []
    for child_pd in elem.findall("protection_domain"):
        children.append(parse_pd(child_pd))
    if children:
        pd["children"] = children

    return pd

def main():
    xml_path = sys.argv[1] if len(sys.argv) > 1 else "aios.system"
    json_path = sys.argv[2] if len(sys.argv) > 2 else "aios_system.json"

    tree = ET.parse(xml_path)
    root = tree.getroot()

    result = {"memory_regions": [], "protection_domains": [], "channels": []}

    for mr in root.findall(".//memory_region"):
        region = {"name": mr.get("name"), "size": mr.get("size")}
        if mr.get("phys_addr"):
            region["phys_addr"] = mr.get("phys_addr")
        result["memory_regions"].append(region)

    for pd_elem in root.findall("protection_domain"):
        result["protection_domains"].append(parse_pd(pd_elem))

    for ch in root.findall("channel"):
        channel = {"ends": []}
        for end in ch.findall("end"):
            e = {"pd": end.get("pd"), "id": int(end.get("id"))}
            if end.get("pp") == "true":
                e["pp"] = True
            channel["ends"].append(e)
        result["channels"].append(channel)

    with open(json_path, "w") as f:
        json.dump(result, f, indent=2)
        f.write("\n")

    print(f"Generated {json_path}: {len(result['memory_regions'])} regions, "
          f"{len(result['protection_domains'])} PDs, {len(result['channels'])} channels")

if __name__ == "__main__":
    main()
