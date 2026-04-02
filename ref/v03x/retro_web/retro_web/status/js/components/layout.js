// Layout Components - Wire, Room

function Wire(props) {
  var x1 = props.x1, y1 = props.y1, x2 = props.x2, y2 = props.y2, isActive = props.isActive;
  var dashOffset = isActive ? -(Date.now() / 50) % 20 : 0;
  var midX = x1 + (x2 - x1) / 2;
  return e("path", {
    d: "M" + x1 + "," + y1 + " C" + midX + "," + y1 + " " + midX + "," + y2 + " " + x2 + "," + y2,
    fill: "none",
    stroke: isActive ? "#0af" : "#999",
    strokeWidth: isActive ? 2.5 : 1.5,
    strokeDasharray: isActive ? "4 4" : "none",
    strokeDashoffset: dashOffset
  });
}

function Room(props) {
  var x = props.x, y = props.y, width = props.width, height = props.height;
  var label = props.label, color = props.color, children = props.children;
  return e("g", { transform: "translate(" + x + "," + y + ")" },
    e("rect", { x: 0, y: 0, width: width, height: height, fill: color, stroke: "#777", strokeWidth: 2, rx: 6 }),
    e("rect", { x: 0, y: 0, width: width, height: 16, fill: "rgba(0,0,0,0.1)", rx: 6 }),
    e(LED, { x: width - 10, y: 8, color: "#0f0", size: 3, on: true }),
    e("text", { x: 6, y: 11, fontSize: 8, fill: "#333", fontWeight: 600 }, label),
    children
  );
}
