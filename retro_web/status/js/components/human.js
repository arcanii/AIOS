// Human Component

function Human(props) {
  var x = props.x, y = props.y, label = props.label, color = props.color, isWalking = props.isWalking;
  var isBeaming = props.isBeaming;
  var now = Date.now();
  var bobOffset = isWalking ? Math.sin(now / 120) * 1.5 : 0;
  var armSwing = isWalking ? Math.sin(now / 100) * 15 : 0;
  var beamOpacity = isBeaming ? (Math.sin(now / 50) * 0.3 + 0.7) : 1;
  
  return e("g", { transform: "translate(" + x + "," + (y + bobOffset) + ")", opacity: beamOpacity },
    e("circle", { cx: 0, cy: -16, r: 6, fill: color, stroke: "#333", strokeWidth: 1 }),
    e("circle", { cx: -2, cy: -17, r: 1, fill: "#333" }),
    e("circle", { cx: 2, cy: -17, r: 1, fill: "#333" }),
    e("rect", { x: -5, y: -10, width: 10, height: 12, rx: 2, fill: color, stroke: "#333", strokeWidth: 1 }),
    e("line", { x1: -5, y1: -6, x2: -10, y2: -2 + armSwing / 3, stroke: color, strokeWidth: 3, strokeLinecap: "round" }),
    e("line", { x1: 5, y1: -6, x2: 10, y2: -2 - armSwing / 3, stroke: color, strokeWidth: 3, strokeLinecap: "round" }),
    e("line", { x1: -2, y1: 2, x2: -4, y2: 14, stroke: "#555", strokeWidth: 3, strokeLinecap: "round" }),
    e("line", { x1: 2, y1: 2, x2: 4, y2: 14, stroke: "#555", strokeWidth: 3, strokeLinecap: "round" }),
    e("text", { x: 0, y: 26, textAnchor: "middle", fontSize: 7, fill: "#666" }, label)
  );
}
