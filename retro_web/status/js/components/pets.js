// Pet Components - Cat, Dog, Mess

function Cat(props) {
  var x = props.x, y = props.y, isWalking = props.isWalking, direction = props.direction || 1;
  var now = Date.now();
  var tailWag = Math.sin(now / 150) * 20;
  var legOffset = isWalking ? Math.sin(now / 80) * 3 : 0;
  var scale = direction < 0 ? -1 : 1;
  
  return e("g", { transform: "translate(" + x + "," + y + ") scale(" + scale + ", 1)" },
    e("path", { d: "M-10,0 Q-18," + (-5 + tailWag / 3) + " -16," + (-12 + tailWag / 2), fill: "none", stroke: "#888", strokeWidth: 3, strokeLinecap: "round" }),
    e("ellipse", { cx: 0, cy: 2, rx: 10, ry: 6, fill: "#888" }),
    e("circle", { cx: 10, cy: -2, r: 6, fill: "#888" }),
    e("polygon", { points: "6,-8 8,-14 12,-8", fill: "#888", stroke: "#666", strokeWidth: 0.5 }),
    e("polygon", { points: "10,-8 12,-14 16,-8", fill: "#888", stroke: "#666", strokeWidth: 0.5 }),
    e("polygon", { points: "7,-9 8,-12 11,-9", fill: "#faa" }),
    e("polygon", { points: "11,-9 12,-12 15,-9", fill: "#faa" }),
    e("circle", { cx: 8, cy: -3, r: 1.5, fill: "#ff0" }),
    e("ellipse", { cx: 8, cy: -3, rx: 0.5, ry: 1.2, fill: "#333" }),
    e("circle", { cx: 13, cy: -3, r: 1.5, fill: "#ff0" }),
    e("ellipse", { cx: 13, cy: -3, rx: 0.5, ry: 1.2, fill: "#333" }),
    e("ellipse", { cx: 10.5, cy: 0, rx: 2, ry: 1.5, fill: "#fcc" }),
    e("line", { x1: 6, y1: -1, x2: 1, y2: -2, stroke: "#666", strokeWidth: 0.5 }),
    e("line", { x1: 6, y1: 0, x2: 1, y2: 1, stroke: "#666", strokeWidth: 0.5 }),
    e("line", { x1: 15, y1: -1, x2: 20, y2: -2, stroke: "#666", strokeWidth: 0.5 }),
    e("line", { x1: 15, y1: 0, x2: 20, y2: 1, stroke: "#666", strokeWidth: 0.5 }),
    e("rect", { x: -6 + legOffset, y: 6, width: 3, height: 5, rx: 1, fill: "#888" }),
    e("rect", { x: -2 - legOffset, y: 6, width: 3, height: 5, rx: 1, fill: "#888" }),
    e("rect", { x: 4 + legOffset, y: 6, width: 3, height: 5, rx: 1, fill: "#888" }),
    e("rect", { x: 8 - legOffset, y: 6, width: 3, height: 5, rx: 1, fill: "#888" })
  );
}

function Dog(props) {
  var x = props.x, y = props.y, isWalking = props.isWalking, direction = props.direction || 1;
  var isPeeing = props.isPeeing;
  var now = Date.now();
  var tailWag = Math.sin(now / 100) * 25;
  var legOffset = isWalking ? Math.sin(now / 80) * 4 : 0;
  var scale = direction < 0 ? -1 : 1;
  
  return e("g", { transform: "translate(" + x + "," + y + ") scale(" + scale + ", 1)" },
    e("path", { d: "M-12,0 Q-16," + (-8 + tailWag / 5) + " -14," + (-14 + tailWag / 3), fill: "none", stroke: "#c90", strokeWidth: 4, strokeLinecap: "round" }),
    e("ellipse", { cx: 0, cy: 2, rx: 12, ry: 7, fill: "#c90" }),
    e("ellipse", { cx: 14, cy: -2, rx: 7, ry: 6, fill: "#c90" }),
    e("ellipse", { cx: 20, cy: 0, rx: 4, ry: 3, fill: "#da4" }),
    e("ellipse", { cx: 22, cy: -1, rx: 2, ry: 1.5, fill: "#333" }),
    e("ellipse", { cx: 9, cy: -4, rx: 4, ry: 6, fill: "#a70" }),
    e("ellipse", { cx: 17, cy: -6, rx: 3, ry: 5, fill: "#a70" }),
    e("circle", { cx: 12, cy: -3, r: 2, fill: "#fff" }),
    e("circle", { cx: 16, cy: -3, r: 2, fill: "#fff" }),
    e("circle", { cx: 12, cy: -3, r: 1, fill: "#333" }),
    e("circle", { cx: 16, cy: -3, r: 1, fill: "#333" }),
    !isPeeing ? e("ellipse", { cx: 20, cy: 4, rx: 2, ry: 3, fill: "#f66" }) : null,
    isPeeing ? e("g", null,
      e("rect", { x: -8, y: 6, width: 4, height: 6, rx: 1, fill: "#c90" }),
      e("rect", { x: -2, y: 6, width: 4, height: 6, rx: 1, fill: "#c90" }),
      e("rect", { x: 4, y: 6, width: 4, height: 6, rx: 1, fill: "#c90" }),
      e("rect", { x: 10, y: 0, width: 4, height: 6, rx: 1, fill: "#c90", transform: "rotate(-60, 12, 6)" }),
      e("path", { d: "M14,2 Q20,8 18,14", fill: "none", stroke: "#ff0", strokeWidth: 2, strokeDasharray: "3 2", opacity: 0.8 })
    ) : e("g", null,
      e("rect", { x: -8 + legOffset, y: 7, width: 4, height: 6, rx: 1, fill: "#c90" }),
      e("rect", { x: -2 - legOffset, y: 7, width: 4, height: 6, rx: 1, fill: "#c90" }),
      e("rect", { x: 6 + legOffset, y: 7, width: 4, height: 6, rx: 1, fill: "#c90" }),
      e("rect", { x: 12 - legOffset, y: 7, width: 4, height: 6, rx: 1, fill: "#c90" })
    )
  );
}

function Mess(props) {
  var x = props.x, y = props.y, type = props.type;
  if (type === "poop") {
    return e("g", { transform: "translate(" + x + "," + y + ")" },
      e("ellipse", { cx: 0, cy: 4, rx: 5, ry: 2, fill: "#543" }),
      e("ellipse", { cx: 0, cy: 1, rx: 4, ry: 3, fill: "#654" }),
      e("ellipse", { cx: 0, cy: -2, rx: 3, ry: 2.5, fill: "#765" }),
      e("ellipse", { cx: 0, cy: -4, rx: 2, ry: 2, fill: "#876" }),
      e("text", { x: 8, y: 0, fontSize: 8 }, "\uD83D\uDCA9")
    );
  } else {
    return e("g", { transform: "translate(" + x + "," + y + ")" },
      e("ellipse", { cx: 0, cy: 0, rx: 8, ry: 4, fill: "#ff0", opacity: 0.4 }),
      e("ellipse", { cx: 0, cy: 0, rx: 6, ry: 3, fill: "#ff0", opacity: 0.6 }),
      e("text", { x: 6, y: -2, fontSize: 6 }, "\uD83D\uDCA6")
    );
  }
}
