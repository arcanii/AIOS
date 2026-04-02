// Cleaning Robot Component

function CleaningRobot(props) {
  var x = props.x, y = props.y, isMoving = props.isMoving, isCleaning = props.isCleaning;
  var now = Date.now();
  var spin = isMoving ? (now / 30) % 360 : 0;
  var wobble = isCleaning ? Math.sin(now / 50) * 2 : 0;
  
  return e("g", { transform: "translate(" + x + "," + y + ")" },
    e("ellipse", { cx: wobble, cy: 0, rx: 14, ry: 10, fill: "#4a4", stroke: "#383", strokeWidth: 2 }),
    e("ellipse", { cx: wobble, cy: -4, rx: 12, ry: 6, fill: "#5b5", stroke: "#494", strokeWidth: 1 }),
    e("circle", { cx: -4 + wobble, cy: -4, r: 2, fill: "#fff" }),
    e("circle", { cx: 4 + wobble, cy: -4, r: 2, fill: "#fff" }),
    e("circle", { cx: -4 + wobble, cy: -4, r: 1, fill: "#333" }),
    e("circle", { cx: 4 + wobble, cy: -4, r: 1, fill: "#333" }),
    isCleaning ? e("g", { transform: "rotate(" + spin + ", " + wobble + ", 4)" },
      e("line", { x1: wobble - 8, y1: 4, x2: wobble + 8, y2: 4, stroke: "#888", strokeWidth: 2, strokeDasharray: "2 2" })
    ) : null,
    e(LED, { x: wobble, y: -6, color: isCleaning ? "#f00" : "#0f0", size: 2, blink: isCleaning ? 100 : 0 }),
    e("text", { x: 0, y: 18, textAnchor: "middle", fontSize: 6, fill: "#666" }, "CLEAN-O")
  );
}
