#!/usr/bin/env python3
"""
AIOS System Map Tool

Converts aios.system (Microkit XML) to/from JSON,
and generates a visual memory map as PNG.

Usage:
    python3 tools/sysmap.py xml2json [aios.system] [output.json]
    python3 tools/sysmap.py json2xml [input.json]  [aios.system]
    python3 tools/sysmap.py visualize [aios.system] [output.png]
    python3 tools/sysmap.py all       [aios.system]              # does all three
"""

import sys, json, os, re
from xml.etree import ElementTree as ET
from collections import OrderedDict

# ═══════════════════════════════════════════════════════════
# XML → JSON
# ═══════════════════════════════════════════════════════════

def parse_int(s):
    """Parse hex or decimal string to int."""
    if s is None:
        return None
    s = s.strip()
    if s.startswith('0x') or s.startswith('0X'):
        return int(s, 16)
    return int(s)

def parse_memory_regions(root):
    regions = []
    for mr in root.findall('memory_region'):
        r = {'name': mr.get('name'), 'size': mr.get('size')}
        if mr.get('phys_addr'):
            r['phys_addr'] = mr.get('phys_addr')
        regions.append(r)
    return regions

def parse_map(el):
    m = {
        'mr': el.get('mr'),
        'vaddr': el.get('vaddr'),
        'perms': el.get('perms'),
        'cached': el.get('cached', 'true'),
    }
    if el.get('setvar_vaddr'):
        m['setvar_vaddr'] = el.get('setvar_vaddr')
    return m

def parse_pd(pd_el):
    pd = OrderedDict()
    pd['name'] = pd_el.get('name')
    pd['priority'] = int(pd_el.get('priority'))
    if pd_el.get('id') is not None:
        pd['id'] = int(pd_el.get('id'))
    if pd_el.get('budget'):
        pd['budget'] = int(pd_el.get('budget'))
    if pd_el.get('period'):
        pd['period'] = int(pd_el.get('period'))
    if pd_el.get('cpu') is not None:
        pd['cpu'] = int(pd_el.get('cpu'))
    if pd_el.get('stack_size'):
        pd['stack_size'] = pd_el.get('stack_size')

    prog = pd_el.find('program_image')
    if prog is not None:
        pd['program_image'] = prog.get('path')

    maps = []
    for m in pd_el.findall('map'):
        maps.append(parse_map(m))
    if maps:
        pd['maps'] = maps

    irqs = []
    for irq in pd_el.findall('irq'):
        irqs.append({'irq': int(irq.get('irq')), 'id': int(irq.get('id'))})
    if irqs:
        pd['irqs'] = irqs

    children = []
    for child_pd in pd_el.findall('protection_domain'):
        children.append(parse_pd(child_pd))
    if children:
        pd['children'] = children

    return pd

def parse_channels(root):
    channels = []
    for ch in root.findall('channel'):
        ends = []
        for end in ch.findall('end'):
            e = {'pd': end.get('pd'), 'id': int(end.get('id'))}
            if end.get('pp'):
                e['pp'] = end.get('pp') == 'true'
            ends.append(e)
        channels.append({'ends': ends})
    return channels

def xml_to_json(xml_path):
    tree = ET.parse(xml_path)
    root = tree.getroot()

    system = OrderedDict()
    system['memory_regions'] = parse_memory_regions(root)

    pds = []
    for pd in root.findall('protection_domain'):
        pds.append(parse_pd(pd))
    system['protection_domains'] = pds

    system['channels'] = parse_channels(root)

    return system

# ═══════════════════════════════════════════════════════════
# JSON → XML
# ═══════════════════════════════════════════════════════════

def indent(level):
    return '    ' * level

def emit_map(m, level):
    parts = [f'{indent(level)}<map mr="{m["mr"]}" vaddr="{m["vaddr"]}" perms="{m["perms"]}" cached="{m["cached"]}"']
    if 'setvar_vaddr' in m:
        parts[0] += f'\n{indent(level)}     setvar_vaddr="{m["setvar_vaddr"]}"'
    parts[0] += ' />'
    return parts[0]

def emit_pd(pd, level):
    lines = []
    attrs = f'name="{pd["name"]}" priority="{pd["priority"]}"'
    if 'id' in pd:
        attrs += f' id="{pd["id"]}"'
    if 'budget' in pd:
        attrs += f' budget="{pd["budget"]}" period="{pd["period"]}"'
    if 'cpu' in pd:
        attrs += f' cpu="{pd["cpu"]}"'
    if 'stack_size' in pd:
        attrs += f' stack_size="{pd["stack_size"]}"'

    lines.append(f'{indent(level)}<protection_domain {attrs}>')

    if 'program_image' in pd:
        lines.append(f'{indent(level+1)}<program_image path="{pd["program_image"]}" />')

    for m in pd.get('maps', []):
        lines.append(emit_map(m, level+1))

    for irq in pd.get('irqs', []):
        lines.append(f'{indent(level+1)}<irq irq="{irq["irq"]}" id="{irq["id"]}" />')

    for child in pd.get('children', []):
        lines.append('')
        lines.extend(emit_pd(child, level+1).split('\n'))

    lines.append(f'{indent(level)}</protection_domain>')
    return '\n'.join(lines)

def json_to_xml(system):
    lines = ['<?xml version="1.0" encoding="UTF-8"?>', '<system>']

    # Memory regions
    lines.append(f'{indent(1)}<!-- ── Memory regions ── -->')
    for mr in system['memory_regions']:
        attrs = f'name="{mr["name"]}" size="{mr["size"]}"'
        if 'phys_addr' in mr:
            attrs += f' phys_addr="{mr["phys_addr"]}"'
        lines.append(f'{indent(1)}<memory_region {attrs} />')
    lines.append('')

    # Protection domains
    lines.append(f'{indent(1)}<!-- ── Protection domains ── -->')
    for pd in system['protection_domains']:
        lines.append(emit_pd(pd, 1))
        lines.append('')

    # Channels
    lines.append(f'{indent(1)}<!-- ── Channels ── -->')
    for ch in system['channels']:
        lines.append(f'{indent(1)}<channel>')
        for end in ch['ends']:
            attrs = f'pd="{end["pd"]}" id="{end["id"]}"'
            if end.get('pp'):
                attrs += ' pp="true"'
            lines.append(f'{indent(2)}<end {attrs} />')
        lines.append(f'{indent(1)}</channel>')
    lines.append('')

    lines.append('</system>')
    return '\n'.join(lines)

# ═══════════════════════════════════════════════════════════
# Visualize — memory map as PNG
# ═══════════════════════════════════════════════════════════

# Color palette for PDs
PD_COLORS = {
    'serial_driver': '#FF6B6B',
    'blk_driver':    '#4ECDC4',
    'fs_server':     '#45B7D1',
    'orchestrator':  '#96CEB4',
    'llm_server':    '#FFEAA7',
    'echo_server':   '#DDA0DD',
    'auth_server':   '#F39C12',
    'net_driver':    '#3498DB',
    'net_server':    '#2ECC71',
}
SBX_COLOR = '#C0C0C0'
SHARED_COLOR = '#FFD93D'
PHYS_COLOR = '#E8E8E8'

def hex_to_rgb(h):
    h = h.lstrip('#')
    return tuple(int(h[i:i+2], 16) for i in (0, 2, 4))

def visualize(system, output_path):
    """Generate a PNG memory map using only built-in libraries (no matplotlib/pillow required)."""

    # Collect all memory mappings per PD
    pd_maps = {}

    def collect_pd(pd, parent=None):
        name = pd['name']
        color = PD_COLORS.get(name, SBX_COLOR)
        if name not in pd_maps:
            pd_maps[name] = {'color': color, 'priority': pd['priority'], 'maps': [], 'parent': parent}
        for m in pd.get('maps', []):
            mr_size = None
            for mr in system['memory_regions']:
                if mr['name'] == m['mr']:
                    mr_size = parse_int(mr['size'])
                    break
            pd_maps[name]['maps'].append({
                'mr': m['mr'],
                'vaddr': parse_int(m['vaddr']),
                'size': mr_size or 0x1000,
                'perms': m['perms'],
                'cached': m['cached'],
                'setvar': m.get('setvar_vaddr', ''),
            })
        for child in pd.get('children', []):
            collect_pd(child, parent=name)

    for pd in system['protection_domains']:
        collect_pd(pd)

    # Build channel map
    channel_map = []
    for ch in system['channels']:
        if len(ch['ends']) == 2:
            channel_map.append({
                'pd1': ch['ends'][0]['pd'], 'id1': ch['ends'][0]['id'],
                'pd2': ch['ends'][1]['pd'], 'id2': ch['ends'][1]['id'],
                'pp': ch['ends'][0].get('pp', False) or ch['ends'][1].get('pp', False),
            })

    # Generate SVG (universally viewable, no dependencies)
    svg_path = output_path.replace('.png', '.svg') if output_path.endswith('.png') else output_path + '.svg'

    # Layout parameters
    col_width = 320
    row_height = 28
    header_height = 40
    padding = 20
    map_bar_height = 22

    # Sort PDs: orchestrator first, then by priority descending
    pd_names = sorted(pd_maps.keys(), key=lambda n: (0 if n == 'orchestrator' else 1, -pd_maps[n]['priority']))

    # Calculate dimensions
    max_maps = max(len(pd_maps[n]['maps']) for n in pd_names) if pd_names else 1
    num_cols = min(len(pd_names), 4)
    num_rows = (len(pd_names) + num_cols - 1) // num_cols

    total_width = padding * 2 + num_cols * (col_width + padding)
    # Each PD card: header + maps + padding
    card_heights = {}
    for name in pd_names:
        n_maps = len(pd_maps[name]['maps'])
        card_heights[name] = header_height + max(n_maps, 1) * row_height + 20

    max_card_per_row = []
    for r in range(num_rows):
        row_pds = pd_names[r*num_cols:(r+1)*num_cols]
        max_card_per_row.append(max(card_heights[n] for n in row_pds) if row_pds else 100)

    # Memory regions summary height
    mr_section_height = header_height + len(system['memory_regions']) * 20 + 40

    # Channel section height
    ch_section_height = header_height + len(channel_map) * 20 + 40

    total_height = padding + 60 + mr_section_height + sum(max_card_per_row) + num_rows * padding + ch_section_height + padding

    svg_lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{total_width}" height="{total_height}" '
        f'font-family="Consolas, Monaco, monospace" font-size="12">',
        f'<rect width="{total_width}" height="{total_height}" fill="#1a1a2e"/>',
        # Title
        f'<text x="{total_width//2}" y="35" text-anchor="middle" font-size="20" font-weight="bold" fill="#e0e0e0">'
        f'AIOS System Memory Map</text>',
    ]

    y_offset = 60

    # ── Memory Regions Section ──
    svg_lines.append(f'<text x="{padding}" y="{y_offset + 20}" font-size="16" font-weight="bold" fill="#96CEB4">'
                     f'Memory Regions ({len(system["memory_regions"])})</text>')
    y_offset += 35

    for i, mr in enumerate(system['memory_regions']):
        y = y_offset + i * 20
        size = parse_int(mr['size'])
        size_str = f'{size:#x}' if size else '?'
        if size >= 0x100000:
            human = f'{size // 0x100000} MiB'
        elif size >= 0x400:
            human = f'{size // 0x400} KiB'
        else:
            human = f'{size} B'
        phys = f' @ {mr["phys_addr"]}' if 'phys_addr' in mr else ''
        fill = '#FFD93D' if 'phys_addr' in mr else '#a0a0c0'
        svg_lines.append(
            f'<text x="{padding + 10}" y="{y + 14}" fill="{fill}" font-size="11">'
            f'{mr["name"]:24s}  {size_str:>12s}  ({human}){phys}</text>'
        )

    y_offset += len(system['memory_regions']) * 20 + 30

    # ── PD Cards ──
    for r in range(num_rows):
        row_pds = pd_names[r*num_cols:(r+1)*num_cols]
        for c, name in enumerate(row_pds):
            pd = pd_maps[name]
            x = padding + c * (col_width + padding)
            y = y_offset
            ch = card_heights[name]

            color = pd['color']
            rgb = hex_to_rgb(color)
            bg = f'rgb({rgb[0]//4 + 20},{rgb[1]//4 + 20},{rgb[2]//4 + 20})'

            # Card background
            svg_lines.append(f'<rect x="{x}" y="{y}" width="{col_width}" height="{ch}" '
                           f'rx="8" fill="{bg}" stroke="{color}" stroke-width="2"/>')

            # Header
            parent_str = f' (child of {pd["parent"]})' if pd.get('parent') else ''
            svg_lines.append(f'<text x="{x+10}" y="{y+22}" font-size="14" font-weight="bold" fill="{color}">'
                           f'{name}</text>')
            svg_lines.append(f'<text x="{x+col_width-10}" y="{y+22}" text-anchor="end" font-size="11" fill="#808080">'
                           f'prio {pd["priority"]}{parent_str}</text>')

            # Maps
            for mi, m in enumerate(pd['maps']):
                my = y + header_height + mi * row_height
                size = m['size']
                if size >= 0x100000:
                    sz_str = f'{size // 0x100000}M'
                elif size >= 0x400:
                    sz_str = f'{size // 0x400}K'
                else:
                    sz_str = f'{size}B'

                # Bar
                bar_width = min(max(size / 0x100000 * 10, 20), col_width - 130)
                perm_color = '#FF6B6B' if 'x' in m['perms'] else ('#4ECDC4' if 'w' in m['perms'] else '#45B7D1')
                svg_lines.append(f'<rect x="{x+8}" y="{my+2}" width="{bar_width:.0f}" height="{map_bar_height}" '
                               f'rx="3" fill="{perm_color}" opacity="0.4"/>')

                svg_lines.append(f'<text x="{x+12}" y="{my+16}" font-size="10" fill="#e0e0e0">'
                               f'{m["mr"]}</text>')
                svg_lines.append(f'<text x="{x+col_width-10}" y="{my+16}" text-anchor="end" font-size="10" fill="#a0a0a0">'
                               f'{m["vaddr"]:#010x} {m["perms"]} {sz_str}</text>')

            if not pd['maps']:
                svg_lines.append(f'<text x="{x+10}" y="{y + header_height + 16}" font-size="11" fill="#606060">'
                               f'(no memory maps)</text>')

        y_offset += max_card_per_row[r] + padding

    # ── Channels Section ──
    svg_lines.append(f'<text x="{padding}" y="{y_offset + 20}" font-size="16" font-weight="bold" fill="#96CEB4">'
                     f'Channels ({len(channel_map)})</text>')
    y_offset += 35

    for i, ch in enumerate(channel_map):
        y = y_offset + i * 20
        pp_str = ' (PPC)' if ch['pp'] else ''
        svg_lines.append(
            f'<text x="{padding + 10}" y="{y + 14}" fill="#a0a0c0" font-size="11">'
            f'CH {ch["id1"]:>2d}: {ch["pd1"]:20s} ↔ {ch["pd2"]:20s}{pp_str}</text>'
        )

    svg_lines.append('</svg>')

    with open(svg_path, 'w') as f:
        f.write('\n'.join(svg_lines))
    print(f"  SVG written to {svg_path}")

    # Try to convert to PNG if cairosvg or rsvg-convert available
    png_path = output_path if output_path.endswith('.png') else output_path.replace('.svg', '.png')
    try:
        import subprocess
        # Try rsvg-convert first (common on macOS via brew)
        r = subprocess.run(['rsvg-convert', '-o', png_path, svg_path],
                          capture_output=True, timeout=10)
        if r.returncode == 0:
            print(f"  PNG written to {png_path}")
            return
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass

    try:
        import cairosvg
        cairosvg.svg2png(url=svg_path, write_to=png_path)
        print(f"  PNG written to {png_path}")
        return
    except ImportError:
        pass

    print(f"  (PNG conversion skipped — install rsvg-convert or cairosvg for PNG output)")
    print(f"  Open {svg_path} in any browser to view the memory map")


# ═══════════════════════════════════════════════════════════
# CLI
# ═══════════════════════════════════════════════════════════

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    cmd = sys.argv[1]
    xml_file = sys.argv[2] if len(sys.argv) > 2 else 'aios.system'

    if cmd == 'xml2json':
        out = sys.argv[3] if len(sys.argv) > 3 else 'aios_system.json'
        system = xml_to_json(xml_file)
        with open(out, 'w') as f:
            json.dump(system, f, indent=2)
        print(f"  JSON written to {out}")
        print(f"  {len(system['memory_regions'])} memory regions")
        print(f"  {len(system['protection_domains'])} protection domains")
        print(f"  {len(system['channels'])} channels")

    elif cmd == 'json2xml':
        json_file = sys.argv[2] if len(sys.argv) > 2 else 'aios_system.json'
        out = sys.argv[3] if len(sys.argv) > 3 else 'aios.system'
        with open(json_file) as f:
            system = json.load(f)
        xml = json_to_xml(system)
        with open(out, 'w') as f:
            f.write(xml)
        print(f"  XML written to {out}")

    elif cmd == 'visualize':
        out = sys.argv[3] if len(sys.argv) > 3 else 'docs/sysmap.png'
        system = xml_to_json(xml_file)
        visualize(system, out)

    elif cmd == 'all':
        print("═══ XML → JSON ═══")
        system = xml_to_json(xml_file)
        json_out = 'aios_system.json'
        with open(json_out, 'w') as f:
            json.dump(system, f, indent=2)
        print(f"  JSON: {json_out}")

        print("\n═══ Visualize ═══")
        visualize(system, 'docs/sysmap.png')

        print("\n═══ JSON → XML (roundtrip test) ═══")
        xml_rt = json_to_xml(system)
        with open('aios_system_roundtrip.xml', 'w') as f:
            f.write(xml_rt)
        print(f"  Roundtrip XML: aios_system_roundtrip.xml")
        print(f"  Compare with: diff aios.system aios_system_roundtrip.xml")

    elif cmd == 'add-mr':
        # Quick helper: python3 tools/sysmap.py add-mr <name> <size> [phys_addr]
        if len(sys.argv) < 4:
            print("Usage: sysmap.py add-mr <name> <size> [phys_addr]")
            sys.exit(1)
        json_file = 'aios_system.json'
        if not os.path.exists(json_file):
            system = xml_to_json(xml_file)
        else:
            with open(json_file) as f:
                system = json.load(f)
        mr = {'name': sys.argv[2], 'size': sys.argv[3]}
        if len(sys.argv) > 4:
            mr['phys_addr'] = sys.argv[4]
        system['memory_regions'].append(mr)
        with open(json_file, 'w') as f:
            json.dump(system, f, indent=2)
        print(f"  Added memory region: {mr}")

    elif cmd == 'add-map':
        # python3 tools/sysmap.py add-map <pd_name> <mr> <vaddr> <perms> [setvar]
        if len(sys.argv) < 6:
            print("Usage: sysmap.py add-map <pd_name> <mr> <vaddr> <perms> [setvar]")
            sys.exit(1)
        json_file = 'aios_system.json'
        if not os.path.exists(json_file):
            system = xml_to_json(xml_file)
        else:
            with open(json_file) as f:
                system = json.load(f)

        pd_name = sys.argv[2]
        m = {'mr': sys.argv[3], 'vaddr': sys.argv[4], 'perms': sys.argv[5], 'cached': 'false'}
        if len(sys.argv) > 6:
            m['setvar_vaddr'] = sys.argv[6]

        def find_and_add(pds):
            for pd in pds:
                if pd['name'] == pd_name:
                    if 'maps' not in pd:
                        pd['maps'] = []
                    pd['maps'].append(m)
                    return True
                if find_and_add(pd.get('children', [])):
                    return True
            return False

        if find_and_add(system['protection_domains']):
            with open(json_file, 'w') as f:
                json.dump(system, f, indent=2)
            print(f"  Added map to {pd_name}: {m}")
        else:
            print(f"  ERROR: PD '{pd_name}' not found")

    else:
        print(f"Unknown command: {cmd}")
        print(__doc__)
        sys.exit(1)

if __name__ == '__main__':
    main()
