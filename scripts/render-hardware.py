#!/usr/bin/env python3
"""Render the reported circuit and breadboard layout using only Python's stdlib."""
import argparse
import json
from pathlib import Path
import re
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[1]
DIRECTORY = ROOT / "docs" / "hardware"
DATA = json.loads((DIRECTORY / "xc3800-wiring.json").read_text())
COMPONENTS = {component["id"]: component for component in DATA["components"]}
COLORS = DATA["nets"]


def strip(hole):
    match = re.fullmatch(r"([a-j+-])(\d+)", hole)
    if not match:
        raise ValueError(f"Invalid breadboard hole: {hole}")
    column, row = match[1], int(match[2])
    if not 1 <= row <= 60:
        raise ValueError(f"Invalid breadboard row: {hole}")
    return column if column in "+-" else ("left" if column <= "e" else "right", row)


def validate_wiring():
    # Model the conductive strips and jumpers independently of diagram geometry.
    # Passive components are not unions: joining their terminals would hide shorts.
    parents = {}

    def find(node):
        parents.setdefault(node, node)
        if parents[node] != node:
            parents[node] = find(parents[node])
        return parents[node]

    for first, second in DATA["jumpers"]:
        parents[find(strip(first))] = find(strip(second))
    net_roots = {}
    root_nets = {}
    occupied = set()
    for component in DATA["components"]:
        for pin, net, hole in component["pins"]:
            if net not in COLORS:
                raise ValueError(f"Unknown net: {net}")
            if hole in occupied:
                raise ValueError(f"Two component pins occupy {hole}")
            occupied.add(hole)
            root = find(strip(hole))
            if root in root_nets and root_nets[root] != net:
                raise ValueError(f"Short between {net} and {root_nets[root]} at {hole}")
            if net in net_roots and net_roots[net] != root:
                raise ValueError(f"Disconnected {net} at {component['id']} {pin} ({hole})")
            net_roots[net] = root
            root_nets[root] = net
    return {hole: root_nets[find(strip(hole))] for pair in DATA["jumpers"] for hole in pair}


class SVG:
    def __init__(self, width, height, title):
        self.parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
                      f'<title>{escape(title)}</title>',
                      '<style>text{font-family:DejaVu Sans,sans-serif;fill:#172b4d} .small{font-size:14px} .label{font-size:17px} .heading{font-size:24px;font-weight:bold}</style>',
                      f'<rect width="{width}" height="{height}" fill="#fff"/>']

    def text(self, x, y, value, css="label", anchor="start"):
        self.parts.append(f'<text x="{x}" y="{y}" class="{css}" text-anchor="{anchor}">{escape(str(value))}</text>')

    def line(self, x1, y1, x2, y2, color="#475569", width=3):
        self.parts.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{color}" stroke-width="{width}"/>')

    def rect(self, x, y, width, height, fill="#f1f5f9"):
        self.parts.append(f'<rect x="{x}" y="{y}" width="{width}" height="{height}" rx="5" fill="{fill}" stroke="#64748b" stroke-width="2"/>')

    def circle(self, x, y, radius=4, color="#94a3b8"):
        self.parts.append(f'<circle cx="{x}" cy="{y}" r="{radius}" fill="{color}"/>')

    def output(self):
        return "\n".join(self.parts + ["</svg>", ""])


def block(svg, component_id, x, y, width, height, sides):
    component = COMPONENTS[component_id]
    svg.rect(x, y, width, height)
    svg.text(x + width / 2, y + 27, component_id, anchor="middle")
    svg.text(x + width / 2, y + 49, component["name"], "small", "middle")
    for pin_index, side, pin_y in sides:
        pin, net, _ = component["pins"][pin_index]
        left = side == "left"
        edge = x if left else x + width
        end = edge - 100 if left else edge + 100
        svg.line(edge, pin_y, end, pin_y, COLORS[net])
        svg.text(edge + (10 if left else -10), pin_y - 7, pin, "small", "start" if left else "end")
        svg.text(end, pin_y - 8, net, "small", "start" if left else "end")


def schematic():
    svg = SVG(1320, 1050, DATA["title"] + ": schematic")
    svg.text(35, 42, DATA["title"], "heading")
    svg.text(35, 73, "Net-labelled schematic: every repeated net name is electrically connected.", "small")
    svg.text(35, 97, DATA["status"], "small")
    block(svg, "U2", 135, 140, 295, 205,
          [(0, "right", 215), (1, "right", 265), (2, "right", 315)])
    block(svg, "J1", 855, 140, 315, 205,
          [(0, "left", 215), (1, "left", 265), (2, "left", 315)])
    block(svg, "U1", 175, 400, 395, 290,
          [(0, "left", 485), (1, "left", 540), (2, "left", 595), (3, "left", 650),
           (7, "right", 485), (6, "right", 540), (5, "right", 595), (4, "right", 650)])
    block(svg, "A1", 855, 400, 410, 290,
          [(0, "left", 485), (1, "left", 540), (2, "left", 595), (3, "left", 650)])
    svg.text(175, 719, "U1 pin numbers: DIP-8 top view; use the -330 (3.3 V) variant.", "small")
    for component_id, x in [("R1", 180), ("R2", 490), ("C1", 800), ("C2", 1110)]:
        component = COMPONENTS[component_id]
        upper_net, lower_net = [pin[1] for pin in component["pins"]]
        svg.text(x, 775, upper_net, anchor="middle")
        svg.line(x, 788, x, 815, COLORS[upper_net])
        if component_id.startswith("R"):
            svg.rect(x - 12, 815, 24, 65, "#fff")
        else:
            svg.line(x - 25, 838, x + 25, 838)
            svg.line(x - 25, 854, x + 25, 854)
            svg.line(x, 815, x, 838, COLORS[upper_net])
            svg.line(x, 854, x, 880, COLORS[lower_net])
            svg.text(x - 46, 832, "+")
        svg.line(x, 880, x, 906, COLORS[lower_net])
        svg.text(x, 929, lower_net, anchor="middle")
        svg.text(x + 37, 828, component_id, "small")
        svg.text(x, 962, component["name"], "small", "middle")
    svg.text(35, 1004, "VREG connects only to C2. Do not join VREG and +3V3. C1 fitted; no reverse-polarity diode fitted.", "small")
    svg.text(35, 1028, "See hardware.md for capacitor ESR, supply sequencing, and power-rail verification limits.", "small")
    return svg.output()


def breadboard(jumper_nets):
    svg = SVG(1320, 1800, DATA["title"] + ": breadboard")
    svg.text(35, 42, "XC3800 breadboard layout — as reported", "heading")
    svg.text(35, 73, "Component pins in labelled holes; jumpers connect only their endpoints. Not to physical scale.", "small")
    svg.text(35, 97, "Assumes continuous power rails. Verify rail breaks and board pin labels before connecting power.", "small")
    xs = {column: 145 + i * 42 + (32 if i >= 5 else 0) for i, column in enumerate("abcdefghij")}
    xs.update({"+": 655, "-": 720})

    def xy(hole):
        column, row = hole[0], int(hole[1:])
        return xs[column], 180 + (row - 1) * 34 if row <= 16 else 820 + (row - 38) * 34

    for column, x in xs.items():
        svg.text(x, 147, column, anchor="middle")
        svg.text(x, 801, column, anchor="middle")
    for row in list(range(1, 17)) + list(range(38, 61)):
        y = xy(f"a{row}")[1]
        svg.text(22, y + 5, row, "small", "end")
        for column, x in xs.items():
            svg.circle(x, y, color="#e1b6a6" if column == "+" else "#cbd5e1")
    svg.text(350, 755, "Rows 17–37 omitted", "small", "middle")
    svg.rect(315, 470, 92, 136, "#e2e8f0")
    svg.text(350, 562, "U1", "small", "middle")
    svg.parts.append('<path d="M 350 470 Q 360 491 370 470" fill="none" stroke="#64748b" stroke-width="2"/>')
    svg.rect(40, 228, 96, 110)
    svg.text(85, 217, "U2", "small", "middle")
    svg.rect(168, 875, 390, 650, "#eef2ff")
    svg.text(363, 913, "A1 / XC3800", anchor="middle")
    svg.text(363, 976, "antenna toward row 1", "small", "middle")
    svg.text(363, 1487, "USB end", "small", "middle")
    for component_id in ("U1", "U2", "A1"):
        for pin, net, hole in COMPONENTS[component_id]["pins"]:
            x, y = xy(hole)
            svg.circle(x, y, 6, COLORS[net])
            if component_id == "U1":
                svg.text(x + (10 if hole[0] == "e" else -10), y - 9,
                         pin.split()[0], "small", "start" if hole[0] == "e" else "end")
            elif component_id == "A1":
                svg.text(x + (10 if hole[0] == "b" else -10), y - 9,
                         pin, "small", "start" if hole[0] == "b" else "end")
            else:
                svg.text(x - 15, y + 5, pin, "small", "end")
    for component_id in ("R1", "R2", "C1", "C2"):
        component = COMPONENTS[component_id]
        first, second = [xy(pin[2]) for pin in component["pins"]]
        svg.line(*first, *second, "#334155", 3)
        mx, my = (first[0] + second[0]) / 2, (first[1] + second[1]) / 2
        if component_id.startswith("C"):
            if first[0] == second[0]:
                svg.rect(mx - 12, my - 14, 24, 28, "#fff")
                svg.line(mx - 16, my - 6, mx + 16, my - 6)
                svg.line(mx - 16, my + 6, mx + 16, my + 6)
            else:
                svg.rect(mx - 14, my - 12, 28, 24, "#fff")
                svg.line(mx - 6, my - 16, mx - 6, my + 16)
                svg.line(mx + 6, my - 16, mx + 6, my + 16)
            svg.text(first[0] - 20, first[1] + 5, "+", "small")
        else:
            svg.rect(mx - 11, my - 28, 22, 56, "#fef3c7")
        svg.text(mx + 20, my - 20, component_id, "small")
        for _, net, hole in component["pins"]:
            svg.circle(*xy(hole), 5, COLORS[net])
    for pin, net, hole in COMPONENTS["J1"]["pins"]:
        x, y = xy(hole)
        svg.circle(x, y, 7, COLORS[net])
        svg.text(x + 10, y - 9, f"J1 {pin}", "small")
    long_jumpers = []
    for index, (first, second) in enumerate(DATA["jumpers"], 1):
        a, b = xy(first), xy(second)
        color = COLORS[jumper_nets[first]]
        if (int(first[1:]) < 38) != (int(second[1:]) < 38):
            # A matching callout pair represents the long wire across the omitted rows.
            long_jumpers.append((index, first, second))
            for x, y in (a, b):
                svg.circle(x, y, 9, "#fff")
                svg.circle(x, y, 4, color)
                svg.text(x - 12, y - 12, f"W{index}", "small", "end")
        else:
            svg.line(*a, *b, color, 4)
            svg.circle(*a, 4, color)
            svg.circle(*b, 4, color)
    svg.text(800, 165, "Components", "heading")
    y = 205
    for component in DATA["components"]:
        svg.text(800, y, component["id"], "label")
        svg.text(800, y + 22, component["name"], "small")
        y += 61
    svg.text(800, 745, "Long jumper callouts", "heading")
    y = 783
    for index, first, second in long_jumpers:
        svg.text(800, y, f"W{index}: {first} → {second}")
        y += 31
    svg.text(800, 955, "Heat-pump wires", "heading")
    y = 998
    for pin, _, hole in COMPONENTS["J1"]["pins"]:
        svg.text(800, y, f"{pin} → {hole}")
        y += 35
    for i, note in enumerate(["U2 body overhangs the left edge.", "U1 notch toward row 1.", "C1 and C2 are polarised.", "No diode is fitted.", "VREG and +3V3 stay separate.", "Remove ESP32 before USB flashing."]):
        svg.text(800, 1170 + 31 * i, note, "small")
    svg.text(35, 1662, DATA["status"], "small")
    svg.text(35, 1695, "Rail drawing is symbolic: check continuity between the upper circuit and the ESP32 ground connection.", "small")
    svg.text(35, 1728, "The wiring table in hardware.md lists every jumper, including those represented by matching W labels.", "small")
    return svg.output()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Check connectivity and that committed SVGs match their source")
    args = parser.parse_args()
    jumper_nets = validate_wiring()
    outputs = {"xc3800-schematic.svg": schematic(), "xc3800-breadboard.svg": breadboard(jumper_nets)}
    for name, contents in outputs.items():
        path = DIRECTORY / name
        if args.check:
            if not path.exists() or path.read_text() != contents:
                raise SystemExit(f"Stale diagram: {path}; rerun scripts/render-hardware.py")
        else:
            path.write_text(contents)
    print("Breadboard nets are consistent under the stated rail assumption; both diagrams are current.")


if __name__ == "__main__":
    main()
