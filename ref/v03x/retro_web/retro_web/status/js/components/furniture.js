// Furniture Components - Desk, Phone, Disk

function Desk(props) {
  return e("g", { transform: "translate(" + props.x + "," + props.y + ")" },
    e("rect", { x: -18, y: 0, width: 36, height: 3, fill: "#8B4513", stroke: "#5D2E0C", strokeWidth: 1 }),
    e("rect", { x: -16, y: 3, width: 3, height: 10, fill: "#8B4513" }),
    e("rect", { x: 13, y: 3, width: 3, height: 10, fill: "#8B4513" }),
    e("rect", { x: -10, y: -6, width: 14, height: 8, rx: 1, fill: "#333", stroke: "#555", strokeWidth: 1 }),
    e("rect", { x: -8, y: -4, width: 10, height: 4, fill: "#0a3" }),
    e(LED, { x: 6, y: -4, color: "#0f0", size: 1.5, blink: 800 })
  );
}

function Phone(props) {
  var x = props.x, y = props.y, isRinging = props.isRinging;
  var ring = isRinging ? Math.sin(Date.now() / 50) * 2 : 0;
  return e("g", { transform: "translate(" + (x + ring) + "," + y + ")" },
    e("rect", { x: -6, y: -3, width: 12, height: 8, rx: 2, fill: "#222", stroke: "#444", strokeWidth: 1 }),
    e("ellipse", { cx: 0, cy: -6, rx: 8, ry: 3, fill: "none", stroke: "#333", strokeWidth: 2.5 }),
    e("circle", { cx: -6, cy: -6, r: 3, fill: "#333" }),
    e("circle", { cx: 6, cy: -6, r: 3, fill: "#333" }),
    e(LED, { x: 0, y: 0, color: isRinging ? "#f00" : "#333", size: 2, blink: isRinging ? 100 : 0 }),
    isRinging ? e("g", null,
      e("line", { x1: -10, y1: -12, x2: -14, y2: -16, stroke: "#ff0", strokeWidth: 2 }),
      e("line", { x1: 10, y1: -12, x2: 14, y2: -16, stroke: "#ff0", strokeWidth: 2 }),
      e("line", { x1: 0, y1: -10, x2: 0, y2: -16, stroke: "#ff0", strokeWidth: 2 })
    ) : null
  );
}

function Disk(props) {
  var spin = (Date.now() / 20) % 360;
  return e("g", { transform: "translate(" + props.x + "," + props.y + ")" },
    e("ellipse", { cx: 0, cy: 0, rx: 20, ry: 5, fill: "#666" }),
    e("rect", { x: -20, y: -14, width: 40, height: 14, fill: "#888" }),
    e("ellipse", { cx: 0, cy: -14, rx: 20, ry: 5, fill: "#aaa", stroke: "#666", strokeWidth: 1 }),
    e("ellipse", { cx: 0, cy: -14, rx: 12, ry: 3, fill: "#777" }),
    e("line", { x1: 0, y1: -14, x2: Math.cos(spin * Math.PI / 180) * 10, y2: -14 + Math.sin(spin * Math.PI / 180) * 2.5, stroke: "#555", strokeWidth: 1 }),
    e("circle", { cx: 0, cy: -14, r: 1.5, fill: "#333" }),
    e(LED, { x: 18, y: -8, color: "#0f0", size: 2, blink: 300 }),
    e("text", { x: 0, y: 12, textAnchor: "middle", fontSize: 7, fill: "#666" }, "ext2")
  );
}

// Executive chair for the Boss
function Chair(props) {
  var x = props.x, y = props.y;
  var isOccupied = props.isOccupied;
  var spin = props.spin || 0;
  
  return e("g", { transform: "translate(" + x + "," + y + ") rotate(" + spin + ")" },
    // Chair base (5 wheels)
    e("ellipse", { cx: 0, cy: 12, rx: 12, ry: 4, fill: "#333" }),
    e("circle", { cx: -10, cy: 14, r: 2, fill: "#222" }),
    e("circle", { cx: 10, cy: 14, r: 2, fill: "#222" }),
    e("circle", { cx: -6, cy: 16, r: 2, fill: "#222" }),
    e("circle", { cx: 6, cy: 16, r: 2, fill: "#222" }),
    e("circle", { cx: 0, cy: 16, r: 2, fill: "#222" }),
    // Hydraulic pole
    e("rect", { x: -2, y: 2, width: 4, height: 10, fill: "#444" }),
    // Seat cushion
    e("ellipse", { cx: 0, cy: 0, rx: 14, ry: 6, fill: "#222", stroke: "#111", strokeWidth: 1 }),
    e("ellipse", { cx: 0, cy: -1, rx: 12, ry: 5, fill: "#333" }),
    // Backrest
    e("rect", { x: -10, y: -20, width: 20, height: 18, rx: 3, fill: "#222", stroke: "#111", strokeWidth: 1 }),
    e("rect", { x: -8, y: -18, width: 16, height: 14, rx: 2, fill: "#333" }),
    // Armrests
    e("rect", { x: -16, y: -6, width: 6, height: 4, rx: 1, fill: "#222" }),
    e("rect", { x: 10, y: -6, width: 6, height: 4, rx: 1, fill: "#222" }),
    e("rect", { x: -16, y: -8, width: 4, height: 6, rx: 1, fill: "#333" }),
    e("rect", { x: 12, y: -8, width: 4, height: 6, rx: 1, fill: "#333" })
  );
}

