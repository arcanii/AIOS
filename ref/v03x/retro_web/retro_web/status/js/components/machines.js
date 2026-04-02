// Machine Components - Coffee, Vending, Transporter, SecretDoor

function CoffeeMachine(props) {
  var x = props.x, y = props.y, brewing = props.brewing;
  var steamOffset = brewing ? Math.sin(Date.now() / 100) * 2 : 0;
  return e("g", { transform: "translate(" + x + "," + y + ")" },
    e("rect", { x: -12, y: -20, width: 24, height: 28, rx: 2, fill: "#444", stroke: "#333", strokeWidth: 1 }),
    e("rect", { x: -10, y: -18, width: 20, height: 8, rx: 1, fill: "#222" }),
    e("rect", { x: -8, y: -8, width: 10, height: 6, rx: 1, fill: "#111" }),
    e(LED, { x: 5, y: -5, color: "#0f0", size: 2, on: true }),
    e(LED, { x: 9, y: -5, color: brewing ? "#f00" : "#333", size: 2, blink: brewing ? 200 : 0 }),
    e("rect", { x: -8, y: 0, width: 16, height: 8, rx: 1, fill: "#222" }),
    e("rect", { x: -4, y: 2, width: 8, height: 6, rx: 1, fill: "#fff", stroke: "#ccc", strokeWidth: 0.5 }),
    e("rect", { x: -3, y: 3, width: 6, height: 3, fill: brewing ? "#4a2c00" : "#fff" }),
    brewing ? e("g", { opacity: 0.6 },
      e("path", { d: "M-2,-2 Q0," + (-6 + steamOffset) + " 2,-2", fill: "none", stroke: "#fff", strokeWidth: 1 }),
      e("path", { d: "M0,-2 Q2," + (-8 - steamOffset) + " 0,-4", fill: "none", stroke: "#fff", strokeWidth: 1 })
    ) : null,
    e("text", { x: 0, y: 16, textAnchor: "middle", fontSize: 5, fill: "#666" }, "COFFEE")
  );
}

function VendingMachine(props) {
  var x = props.x, y = props.y, dispensing = props.dispensing;
  return e("g", { transform: "translate(" + x + "," + y + ")" },
    e("rect", { x: -14, y: -25, width: 28, height: 38, rx: 2, fill: "#d33", stroke: "#a22", strokeWidth: 1 }),
    e("rect", { x: -12, y: -23, width: 24, height: 22, rx: 1, fill: "#333", stroke: "#222", strokeWidth: 1 }),
    e("rect", { x: -10, y: -21, width: 20, height: 6, fill: "#444" }),
    e("rect", { x: -8, y: -20, width: 4, height: 4, rx: 1, fill: "#fa0" }),
    e("rect", { x: -3, y: -20, width: 4, height: 4, rx: 1, fill: "#0af" }),
    e("rect", { x: 2, y: -20, width: 4, height: 4, rx: 1, fill: "#f0a" }),
    e("rect", { x: 7, y: -20, width: 4, height: 4, rx: 1, fill: "#0f0" }),
    e("rect", { x: -10, y: -13, width: 20, height: 6, fill: "#444" }),
    e("rect", { x: -8, y: -12, width: 4, height: 4, rx: 1, fill: "#ff0" }),
    e("rect", { x: -3, y: -12, width: 4, height: 4, rx: 1, fill: "#f80" }),
    e("rect", { x: 2, y: -12, width: 4, height: 4, rx: 1, fill: "#08f" }),
    e("rect", { x: 7, y: -12, width: 4, height: 4, rx: 1, fill: "#f0f" }),
    e("rect", { x: -12, y: 0, width: 10, height: 10, rx: 1, fill: "#222" }),
    e("rect", { x: 0, y: 0, width: 12, height: 10, rx: 1, fill: "#111" }),
    dispensing ? e("rect", { x: 4, y: 2, width: 5, height: 4, rx: 1, fill: "#fa0", stroke: "#c80", strokeWidth: 0.5 }) : null,
    e(LED, { x: -11, y: -3, color: "#0f0", size: 1.5, on: true }),
    e("text", { x: 0, y: 18, textAnchor: "middle", fontSize: 5, fill: "#666" }, "SNACKS")
  );
}

function Transporter(props) {
  var x = props.x, y = props.y, isActive = props.isActive;
  var beamIn = props.beamIn, beamOut = props.beamOut;
  var now = Date.now();
  var sparkle = (now / 50) % 20;
  
  return e("g", { transform: "translate(" + x + "," + y + ")" },
    e("ellipse", { cx: 0, cy: 10, rx: 25, ry: 6, fill: "#444", stroke: "#333", strokeWidth: 2 }),
    e("ellipse", { cx: 0, cy: 8, rx: 22, ry: 5, fill: "#555" }),
    e("ellipse", { cx: 0, cy: 8, rx: 18, ry: 4, fill: "none", stroke: "#666", strokeWidth: 1 }),
    e("ellipse", { cx: 0, cy: 8, rx: 12, ry: 2.5, fill: "none", stroke: "#666", strokeWidth: 1 }),
    e("ellipse", { cx: 0, cy: 8, rx: 6, ry: 1.5, fill: "none", stroke: "#666", strokeWidth: 1 }),
    e(LED, { x: -20, y: 10, color: isActive ? "#0ff" : "#066", size: 2, blink: isActive ? 100 : 0 }),
    e(LED, { x: -10, y: 12, color: isActive ? "#0ff" : "#066", size: 2, blink: isActive ? 150 : 0 }),
    e(LED, { x: 0, y: 13, color: isActive ? "#0ff" : "#066", size: 2, blink: isActive ? 200 : 0 }),
    e(LED, { x: 10, y: 12, color: isActive ? "#0ff" : "#066", size: 2, blink: isActive ? 150 : 0 }),
    e(LED, { x: 20, y: 10, color: isActive ? "#0ff" : "#066", size: 2, blink: isActive ? 100 : 0 }),
    (beamIn || beamOut) ? e("g", null,
      e("rect", { x: -15, y: -40, width: 30, height: 50, fill: "url(#beamGradient)", opacity: 0.6 }),
      Array.from({ length: 8 }, function(_, i) {
        var sparkY = ((sparkle + i * 6) % 50) - 40;
        var sparkX = Math.sin((now / 100) + i) * 12;
        return e("circle", { key: i, cx: sparkX, cy: sparkY, r: 1.5, fill: "#fff", opacity: 0.8 });
      })
    ) : null,
    e("text", { x: 0, y: 22, textAnchor: "middle", fontSize: 6, fill: "#666" }, "TRANSPORTER")
  );
}

function SecretDoor(props) {
  var x = props.x, y = props.y, isOpen = props.isOpen;
  var openAmount = isOpen ? 12 : 0;
  
  return e("g", { transform: "translate(" + x + "," + y + ")" },
    e("rect", { x: -15, y: -25, width: 30, height: 30, fill: "#555", stroke: "#444", strokeWidth: 2 }),
    e("rect", { x: -14 - openAmount, y: -24, width: 14, height: 28, fill: "#666", stroke: "#555", strokeWidth: 1 }),
    e("rect", { x: openAmount, y: -24, width: 14, height: 28, fill: "#666", stroke: "#555", strokeWidth: 1 }),
    isOpen ? e("g", null,
      e("rect", { x: -10, y: -20, width: 20, height: 4, fill: "#ff0" }),
      e("rect", { x: -8, y: -18, width: 4, height: 2, fill: "#333" }),
      e("rect", { x: 0, y: -18, width: 4, height: 2, fill: "#333" })
    ) : null,
    e("text", { x: 0, y: 12, textAnchor: "middle", fontSize: 5, fill: "#888" }, "SERVICE")
  );
}
