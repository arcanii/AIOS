// Robot Components - All 8 unique robot styles

// BOSS ROBOT - Orchestrator (top hat, monocle, mustache)
function BossRobot(props) {
  var x = props.x, y = props.y, color = props.color, label = props.label;
  var isWalking = props.isWalking, onClick = props.onClick;
  var lookDir = props.lookDir || 0, blinkState = props.blinkState || 0;
  var holdingCoffee = props.holdingCoffee, holdingSnack = props.holdingSnack;
  
  var now = Date.now();
  var bobOffset = isWalking ? Math.sin(now / 100) * 2 : 0;
  var legOffset = isWalking ? Math.sin(now / 80) * 3 : 0;
  var eyeOffsetX = lookDir * 2;
  var eyeHeight = blinkState === 2 ? 1 : (blinkState === 1 ? 4 : 6);
  var eyeY = blinkState === 2 ? -22 : (blinkState === 1 ? -23 : -24);
  
  return e("g", { transform: "translate(" + x + "," + (y + bobOffset) + ")", onClick: onClick, style: { cursor: "pointer" } },
    e("rect", { x: -10, y: -46, width: 20, height: 4, fill: "#222" }),
    e("rect", { x: -7, y: -58, width: 14, height: 14, fill: "#222" }),
    e("rect", { x: -7, y: -46, width: 14, height: 2, fill: "#c9a227" }),
    e("rect", { x: -14, y: -30, width: 28, height: 24, rx: 5, fill: color, stroke: "#333", strokeWidth: 2 }),
    e("circle", { cx: 7, cy: -20, r: 6, fill: "none", stroke: "#c9a227", strokeWidth: 1.5 }),
    e("line", { x1: 13, y1: -20, x2: 18, y2: -10, stroke: "#c9a227", strokeWidth: 1 }),
    e("rect", { x: -10, y: eyeY, width: 6, height: eyeHeight, rx: 1, fill: "#fff" }),
    e("rect", { x: 4, y: eyeY, width: 6, height: eyeHeight, rx: 1, fill: "#fff" }),
    blinkState < 2 ? e("circle", { cx: -7 + eyeOffsetX, cy: -20, r: 2, fill: "#333" }) : null,
    blinkState < 2 ? e("circle", { cx: 7 + eyeOffsetX, cy: -20, r: 2, fill: "#333" }) : null,
    e("path", { d: "M-8,-12 Q-12,-10 -14,-12 M8,-12 Q12,-10 14,-12", fill: "none", stroke: "#333", strokeWidth: 2, strokeLinecap: "round" }),
    e("polygon", { points: "-6,-6 0,-3 6,-6 6,-9 0,-6 -6,-9", fill: "#d33" }),
    e("rect", { x: -12, y: -4, width: 24, height: 18, rx: 4, fill: "#333", stroke: "#222", strokeWidth: 1.5 }),
    e("rect", { x: -2, y: -2, width: 4, height: 14, fill: "#444" }),
    e("circle", { cx: 0, cy: 2, r: 1.5, fill: "#c9a227" }),
    e("circle", { cx: 0, cy: 7, r: 1.5, fill: "#c9a227" }),
    holdingCoffee ? e("g", null,
      e("rect", { x: -20, y: -4, width: 5, height: 12, rx: 2, fill: "#888" }),
      e("rect", { x: -24, y: -6, width: 7, height: 9, rx: 1, fill: "#fff", stroke: "#ccc" }),
      e("rect", { x: -23, y: -4, width: 5, height: 5, fill: "#4a2c00" })
    ) : e("rect", { x: -16, y: -2, width: 5, height: 10, rx: 2, fill: "#888" }),
    holdingSnack ? e("g", null,
      e("rect", { x: 15, y: -4, width: 5, height: 12, rx: 2, fill: "#888" }),
      e("rect", { x: 18, y: -6, width: 7, height: 6, rx: 1, fill: "#fa0", stroke: "#c80" })
    ) : e("rect", { x: 11, y: -2, width: 5, height: 10, rx: 2, fill: "#888" }),
    e("rect", { x: -7 + legOffset, y: 14, width: 5, height: 10, rx: 2, fill: "#888" }),
    e("rect", { x: 2 - legOffset, y: 14, width: 5, height: 10, rx: 2, fill: "#888" }),
    e("text", { x: 0, y: 38, textAnchor: "middle", fontSize: 8, fill: "#333", fontWeight: 600 }, label)
  );
}

// NERDY ROBOT - FS Server (glasses, book)
function NerdyRobot(props) {
  var x = props.x, y = props.y, color = props.color, label = props.label;
  var isWalking = props.isWalking, onClick = props.onClick;
  var lookDir = props.lookDir || 0, blinkState = props.blinkState || 0;
  
  var now = Date.now();
  var bobOffset = isWalking ? Math.sin(now / 100) * 2 : 0;
  var legOffset = isWalking ? Math.sin(now / 80) * 3 : 0;
  var eyeOffsetX = lookDir * 1.5;
  var eyeHeight = blinkState === 2 ? 1 : (blinkState === 1 ? 3 : 5);
  
  return e("g", { transform: "translate(" + x + "," + (y + bobOffset) + ")", onClick: onClick, style: { cursor: "pointer" } },
    e("rect", { x: -12, y: -24, width: 24, height: 20, rx: 4, fill: color, stroke: "#333", strokeWidth: 1.5 }),
    e("rect", { x: -10, y: -20, width: 8, height: 6, rx: 2, fill: "none", stroke: "#333", strokeWidth: 1.5 }),
    e("rect", { x: 2, y: -20, width: 8, height: 6, rx: 2, fill: "none", stroke: "#333", strokeWidth: 1.5 }),
    e("line", { x1: -2, y1: -17, x2: 2, y2: -17, stroke: "#333", strokeWidth: 1.5 }),
    e("rect", { x: -8, y: -19, width: 4, height: eyeHeight - 1, rx: 1, fill: "#fff" }),
    e("rect", { x: 4, y: -19, width: 4, height: eyeHeight - 1, rx: 1, fill: "#fff" }),
    blinkState < 2 ? e("circle", { cx: -6 + eyeOffsetX, cy: -17, r: 1.5, fill: "#333" }) : null,
    blinkState < 2 ? e("circle", { cx: 6 + eyeOffsetX, cy: -17, r: 1.5, fill: "#333" }) : null,
    e("rect", { x: -10, y: -4, width: 20, height: 14, rx: 3, fill: color, stroke: "#333", strokeWidth: 1.5 }),
    e("rect", { x: -8, y: -2, width: 6, height: 8, fill: "#fff" }),
    e("rect", { x: -7, y: -1, width: 1, height: 6, fill: "#00f" }),
    e("rect", { x: -5, y: -1, width: 1, height: 6, fill: "#f00" }),
    e("rect", { x: -3, y: -1, width: 1, height: 5, fill: "#0a0" }),
    e("rect", { x: -14, y: -2, width: 4, height: 8, rx: 2, fill: "#888" }),
    e("g", { transform: "translate(12, 0)" },
      e("rect", { x: 0, y: -2, width: 4, height: 8, rx: 2, fill: "#888" }),
      e("rect", { x: 2, y: -4, width: 8, height: 10, fill: "#8B4513", stroke: "#5D2E0C" }),
      e("rect", { x: 4, y: -2, width: 5, height: 7, fill: "#ffe" })
    ),
    e("rect", { x: -6 + legOffset, y: 10, width: 4, height: 8, rx: 1, fill: "#888" }),
    e("rect", { x: 2 - legOffset, y: 10, width: 4, height: 8, rx: 1, fill: "#888" }),
    e("text", { x: 0, y: 32, textAnchor: "middle", fontSize: 8, fill: "#333", fontWeight: 500 }, label)
  );
}

// WORKER ROBOT - Block Driver (hard hat, safety vest)
function WorkerRobot(props) {
  var x = props.x, y = props.y, color = props.color, label = props.label;
  var isWalking = props.isWalking, onClick = props.onClick;
  var lookDir = props.lookDir || 0, blinkState = props.blinkState || 0;
  
  var now = Date.now();
  var bobOffset = isWalking ? Math.sin(now / 100) * 2 : 0;
  var legOffset = isWalking ? Math.sin(now / 80) * 3 : 0;
  var eyeOffsetX = lookDir * 1.5;
  var eyeHeight = blinkState === 2 ? 1 : (blinkState === 1 ? 3 : 5);
  
  return e("g", { transform: "translate(" + x + "," + (y + bobOffset) + ")", onClick: onClick, style: { cursor: "pointer" } },
    e("ellipse", { cx: 0, cy: -26, rx: 14, ry: 4, fill: "#ff0", stroke: "#cc0", strokeWidth: 1 }),
    e("path", { d: "M-12,-26 Q-14,-32 0,-34 Q14,-32 12,-26", fill: "#ff0", stroke: "#cc0", strokeWidth: 1 }),
    e("rect", { x: -10, y: -28, width: 20, height: 4, fill: "#ff0" }),
    e("rect", { x: -11, y: -22, width: 22, height: 18, rx: 2, fill: color, stroke: "#333", strokeWidth: 1.5 }),
    e("rect", { x: -8, y: -18, width: 5, height: eyeHeight, fill: "#fff" }),
    e("rect", { x: 3, y: -18, width: 5, height: eyeHeight, fill: "#fff" }),
    blinkState < 2 ? e("rect", { x: -7 + eyeOffsetX, y: -17, width: 3, height: 3, fill: "#333" }) : null,
    blinkState < 2 ? e("rect", { x: 4 + eyeOffsetX, y: -17, width: 3, height: 3, fill: "#333" }) : null,
    e("rect", { x: -12, y: -4, width: 24, height: 16, rx: 2, fill: color, stroke: "#333", strokeWidth: 1.5 }),
    e("rect", { x: -10, y: -2, width: 3, height: 14, fill: "#ff0" }),
    e("rect", { x: 7, y: -2, width: 3, height: 14, fill: "#ff0" }),
    e("rect", { x: -17, y: -2, width: 6, height: 10, rx: 2, fill: "#888" }),
    e("rect", { x: 11, y: -2, width: 6, height: 10, rx: 2, fill: "#888" }),
    e("rect", { x: -7 + legOffset, y: 12, width: 5, height: 10, rx: 1, fill: "#888" }),
    e("rect", { x: 2 - legOffset, y: 12, width: 5, height: 10, rx: 1, fill: "#888" }),
    e("text", { x: 0, y: 36, textAnchor: "middle", fontSize: 8, fill: "#333", fontWeight: 500 }, label)
  );
}

// TECH ROBOT - Net Server (antennas, visor)
function TechRobot(props) {
  var x = props.x, y = props.y, color = props.color, label = props.label;
  var isWalking = props.isWalking, onClick = props.onClick;
  var lookDir = props.lookDir || 0, blinkState = props.blinkState || 0;
  
  var now = Date.now();
  var bobOffset = isWalking ? Math.sin(now / 100) * 2 : 0;
  var legOffset = isWalking ? Math.sin(now / 80) * 3 : 0;
  var eyeOffsetX = lookDir * 1.5;
  var eyeHeight = blinkState === 2 ? 1 : (blinkState === 1 ? 3 : 5);
  var antennaWave = Math.sin(now / 200) * 3;
  
  return e("g", { transform: "translate(" + x + "," + (y + bobOffset) + ")", onClick: onClick, style: { cursor: "pointer" } },
    e("line", { x1: -8, y1: -24, x2: -12 + antennaWave, y2: -36, stroke: "#666", strokeWidth: 2 }),
    e("line", { x1: 0, y1: -24, x2: 0, y2: -38, stroke: "#666", strokeWidth: 2 }),
    e("line", { x1: 8, y1: -24, x2: 12 - antennaWave, y2: -36, stroke: "#666", strokeWidth: 2 }),
    e(LED, { x: -12 + antennaWave, y: -38, color: "#0ff", size: 3, blink: 300 }),
    e(LED, { x: 0, y: -40, color: "#0f0", size: 3, blink: 500 }),
    e(LED, { x: 12 - antennaWave, y: -38, color: "#0ff", size: 3, blink: 400 }),
    e("path", { d: "M-12,-24 L-10,-8 L10,-8 L12,-24 Q0,-28 -12,-24", fill: color, stroke: "#333", strokeWidth: 1.5 }),
    e("rect", { x: -9, y: -20, width: 18, height: eyeHeight + 2, rx: 2, fill: "#111" }),
    blinkState < 2 ? e("rect", { x: -7 + eyeOffsetX, y: -19, width: 4, height: eyeHeight, fill: "#0ff" }) : null,
    blinkState < 2 ? e("rect", { x: 3 + eyeOffsetX, y: -19, width: 4, height: eyeHeight, fill: "#0ff" }) : null,
    e("rect", { x: -10, y: -4, width: 20, height: 14, rx: 3, fill: color, stroke: "#333", strokeWidth: 1.5 }),
    e("rect", { x: -6, y: 0, width: 12, height: 6, rx: 2, fill: "#111" }),
    e(LED, { x: -3, y: 3, color: "#0f0", size: 1.5, blink: 200 }),
    e(LED, { x: 0, y: 3, color: "#ff0", size: 1.5, blink: 300 }),
    e(LED, { x: 3, y: 3, color: "#0ff", size: 1.5, blink: 400 }),
    e("rect", { x: -14, y: -2, width: 4, height: 8, rx: 2, fill: "#888" }),
    e("rect", { x: 10, y: -2, width: 4, height: 8, rx: 2, fill: "#888" }),
    e("rect", { x: -6 + legOffset, y: 10, width: 4, height: 8, rx: 1, fill: "#888" }),
    e("rect", { x: 2 - legOffset, y: 10, width: 4, height: 8, rx: 1, fill: "#888" }),
    e("text", { x: 0, y: 32, textAnchor: "middle", fontSize: 8, fill: "#333", fontWeight: 500 }, label)
  );
}

// DRIVER ROBOT - Net Driver (wheels)
function DriverRobot(props) {
  var x = props.x, y = props.y, color = props.color, label = props.label;
  var isWalking = props.isWalking, onClick = props.onClick;
  var lookDir = props.lookDir || 0, blinkState = props.blinkState || 0;
  
  var now = Date.now();
  var wheelSpin = isWalking ? (now / 50) % 360 : 0;
  var eyeOffsetX = lookDir * 1.5;
  var eyeHeight = blinkState === 2 ? 1 : (blinkState === 1 ? 3 : 5);
  
  return e("g", { transform: "translate(" + x + "," + y + ")", onClick: onClick, style: { cursor: "pointer" } },
    e("rect", { x: -10, y: -20, width: 20, height: 16, rx: 4, fill: color, stroke: "#333", strokeWidth: 1.5 }),
    e("rect", { x: -2, y: -26, width: 4, height: 8, fill: "#666" }),
    e(LED, { x: 0, y: -28, color: "#f80", size: 3, blink: 400 }),
    e("rect", { x: -7, y: -16, width: 5, height: eyeHeight, rx: 1, fill: "#fff" }),
    e("rect", { x: 2, y: -16, width: 5, height: eyeHeight, rx: 1, fill: "#fff" }),
    blinkState < 2 ? e("circle", { cx: -4.5 + eyeOffsetX, cy: -13.5, r: 1.5, fill: "#333" }) : null,
    blinkState < 2 ? e("circle", { cx: 4.5 + eyeOffsetX, cy: -13.5, r: 1.5, fill: "#333" }) : null,
    e("rect", { x: -12, y: -4, width: 24, height: 12, rx: 3, fill: color, stroke: "#333", strokeWidth: 1.5 }),
    e("rect", { x: -8, y: 0, width: 16, height: 4, rx: 1, fill: "#222" }),
    e("g", { transform: "rotate(" + wheelSpin + ", -10, 12)" },
      e("circle", { cx: -10, cy: 12, r: 6, fill: "#333", stroke: "#222", strokeWidth: 1 }),
      e("circle", { cx: -10, cy: 12, r: 2, fill: "#666" }),
      e("line", { x1: -10, y1: 6, x2: -10, y2: 18, stroke: "#555", strokeWidth: 1 })
    ),
    e("g", { transform: "rotate(" + wheelSpin + ", 10, 12)" },
      e("circle", { cx: 10, cy: 12, r: 6, fill: "#333", stroke: "#222", strokeWidth: 1 }),
      e("circle", { cx: 10, cy: 12, r: 2, fill: "#666" }),
      e("line", { x1: 10, y1: 6, x2: 10, y2: 18, stroke: "#555", strokeWidth: 1 })
    ),
    e("text", { x: 0, y: 28, textAnchor: "middle", fontSize: 8, fill: "#333", fontWeight: 500 }, label)
  );
}

// RETRO ROBOT - Serial (dials, coil antenna)
function RetroRobot(props) {
  var x = props.x, y = props.y, color = props.color, label = props.label;
  var isWalking = props.isWalking, onClick = props.onClick;
  var lookDir = props.lookDir || 0, blinkState = props.blinkState || 0;
  
  var now = Date.now();
  var bobOffset = isWalking ? Math.sin(now / 100) * 2 : 0;
  var legOffset = isWalking ? Math.sin(now / 80) * 3 : 0;
  var eyeOffsetX = lookDir * 1.5;
  var dialAngle = (now / 100) % 360;
  
  return e("g", { transform: "translate(" + x + "," + (y + bobOffset) + ")", onClick: onClick, style: { cursor: "pointer" } },
    e("path", { d: "M0,-24 Q-3,-28 0,-32 Q3,-36 0,-40", fill: "none", stroke: "#888", strokeWidth: 2 }),
    e("circle", { cx: 0, cy: -42, r: 3, fill: "#f00" }),
    e("rect", { x: -12, y: -24, width: 24, height: 20, rx: 2, fill: color, stroke: "#333", strokeWidth: 1.5 }),
    e("circle", { cx: -5, cy: -14, r: 5, fill: "#222", stroke: "#444", strokeWidth: 1 }),
    e("circle", { cx: 5, cy: -14, r: 5, fill: "#222", stroke: "#444", strokeWidth: 1 }),
    blinkState < 2 ? e("circle", { cx: -5 + eyeOffsetX, cy: -14, r: 2.5, fill: "#f80" }) : null,
    blinkState < 2 ? e("circle", { cx: 5 + eyeOffsetX, cy: -14, r: 2.5, fill: "#f80" }) : null,
    e("rect", { x: -6, y: -8, width: 12, height: 4, fill: "#222" }),
    e("line", { x1: -4, y1: -6, x2: 4, y2: -6, stroke: "#444", strokeWidth: 1 }),
    e("rect", { x: -10, y: -2, width: 20, height: 14, rx: 2, fill: color, stroke: "#333", strokeWidth: 1.5 }),
    e("circle", { cx: -4, cy: 4, r: 4, fill: "#ddd", stroke: "#999", strokeWidth: 1 }),
    e("line", { x1: -4, y1: 4, x2: -4 + Math.cos(dialAngle * Math.PI / 180) * 3, y2: 4 + Math.sin(dialAngle * Math.PI / 180) * 3, stroke: "#333", strokeWidth: 1 }),
    e("circle", { cx: 4, cy: 4, r: 4, fill: "#ddd", stroke: "#999", strokeWidth: 1 }),
    e("line", { x1: 4, y1: 4, x2: 4 + Math.cos(-dialAngle * Math.PI / 180) * 3, y2: 4 + Math.sin(-dialAngle * Math.PI / 180) * 3, stroke: "#333", strokeWidth: 1 }),
    e("path", { d: "M-10,0 L-14,-2 L-12,2 L-16,0 L-14,4 L-18,2", fill: "none", stroke: "#888", strokeWidth: 2 }),
    e("path", { d: "M10,0 L14,-2 L12,2 L16,0 L14,4 L18,2", fill: "none", stroke: "#888", strokeWidth: 2 }),
    e("rect", { x: -6 + legOffset, y: 12, width: 4, height: 8, rx: 1, fill: "#888" }),
    e("rect", { x: 2 - legOffset, y: 12, width: 4, height: 8, rx: 1, fill: "#888" }),
    e("text", { x: 0, y: 32, textAnchor: "middle", fontSize: 8, fill: "#333", fontWeight: 500 }, label)
  );
}

// GUARD ROBOT - Auth (cap, badge)
function GuardRobot(props) {
  var x = props.x, y = props.y, color = props.color, label = props.label;
  var isWalking = props.isWalking, onClick = props.onClick;
  var lookDir = props.lookDir || 0, blinkState = props.blinkState || 0;
  
  var now = Date.now();
  var bobOffset = isWalking ? Math.sin(now / 100) * 2 : 0;
  var legOffset = isWalking ? Math.sin(now / 80) * 3 : 0;
  var eyeOffsetX = lookDir * 1.5;
  
  return e("g", { transform: "translate(" + x + "," + (y + bobOffset) + ")", onClick: onClick, style: { cursor: "pointer" } },
    e("ellipse", { cx: 0, cy: -24, rx: 13, ry: 3, fill: "#224" }),
    e("rect", { x: -12, y: -30, width: 24, height: 8, rx: 2, fill: "#224" }),
    e("ellipse", { cx: 0, cy: -22, rx: 10, ry: 2, fill: "#335" }),
    e("circle", { cx: 0, cy: -26, r: 3, fill: "#fc0" }),
    e("rect", { x: -11, y: -22, width: 22, height: 18, rx: 3, fill: color, stroke: "#333", strokeWidth: 1.5 }),
    e("polygon", { points: "-9,-16 -3,-16 -3,-11 -9,-13", fill: "#fff" }),
    e("polygon", { points: "9,-16 3,-16 3,-11 9,-13", fill: "#fff" }),
    blinkState < 2 ? e("circle", { cx: -6 + eyeOffsetX, cy: -14, r: 1.5, fill: "#333" }) : null,
    blinkState < 2 ? e("circle", { cx: 6 + eyeOffsetX, cy: -14, r: 1.5, fill: "#333" }) : null,
    e("path", { d: "M-4,-8 Q0,-10 4,-8", fill: "none", stroke: "#333", strokeWidth: 1.5 }),
    e("rect", { x: -10, y: -4, width: 20, height: 14, rx: 3, fill: "#224", stroke: "#112", strokeWidth: 1.5 }),
    e("path", { d: "M-2,-2 L0,-4 L2,-2 L2,3 L0,5 L-2,3 Z", fill: "#fc0", stroke: "#ca0", strokeWidth: 0.5 }),
    e("rect", { x: -14, y: -2, width: 4, height: 8, rx: 2, fill: "#224" }),
    e("rect", { x: 10, y: -2, width: 4, height: 8, rx: 2, fill: "#224" }),
    e("rect", { x: -6 + legOffset, y: 10, width: 4, height: 8, rx: 1, fill: "#224" }),
    e("rect", { x: 2 - legOffset, y: 10, width: 4, height: 8, rx: 1, fill: "#224" }),
    e("text", { x: 0, y: 32, textAnchor: "middle", fontSize: 8, fill: "#333", fontWeight: 500 }, label)
  );
}

// FRIENDLY ROBOT - Sandbox (round, cute)
function FriendlyRobot(props) {
  var x = props.x, y = props.y, color = props.color, label = props.label;
  var isWalking = props.isWalking, onClick = props.onClick;
  var lookDir = props.lookDir || 0, blinkState = props.blinkState || 0;
  
  var now = Date.now();
  var bobOffset = isWalking ? Math.sin(now / 100) * 2 : 0;
  var legOffset = isWalking ? Math.sin(now / 80) * 3 : 0;
  var eyeOffsetX = lookDir * 1.5;
  var eyeHeight = blinkState === 2 ? 1 : (blinkState === 1 ? 4 : 8);
  
  return e("g", { transform: "translate(" + x + "," + (y + bobOffset) + ")", onClick: onClick, style: { cursor: "pointer" } },
    e("circle", { cx: -12, cy: -20, r: 4, fill: color, stroke: "#333", strokeWidth: 1 }),
    e("circle", { cx: 12, cy: -20, r: 4, fill: color, stroke: "#333", strokeWidth: 1 }),
    e("circle", { cx: 0, cy: -14, r: 14, fill: color, stroke: "#333", strokeWidth: 1.5 }),
    e("ellipse", { cx: -5, cy: -16, rx: 4, ry: eyeHeight / 2 + 1, fill: "#fff" }),
    e("ellipse", { cx: 5, cy: -16, rx: 4, ry: eyeHeight / 2 + 1, fill: "#fff" }),
    blinkState < 2 ? e("circle", { cx: -5 + eyeOffsetX, cy: -15, r: 2.5, fill: "#333" }) : null,
    blinkState < 2 ? e("circle", { cx: 5 + eyeOffsetX, cy: -15, r: 2.5, fill: "#333" }) : null,
    blinkState < 2 ? e("circle", { cx: -4 + eyeOffsetX, cy: -16, r: 1, fill: "#fff" }) : null,
    blinkState < 2 ? e("circle", { cx: 6 + eyeOffsetX, cy: -16, r: 1, fill: "#fff" }) : null,
    e("path", { d: "M-5,-6 Q0,-2 5,-6", fill: "none", stroke: "#333", strokeWidth: 1.5, strokeLinecap: "round" }),
    e("ellipse", { cx: -9, cy: -10, rx: 3, ry: 2, fill: "#faa", opacity: 0.5 }),
    e("ellipse", { cx: 9, cy: -10, rx: 3, ry: 2, fill: "#faa", opacity: 0.5 }),
    e("ellipse", { cx: 0, cy: 6, rx: 10, ry: 8, fill: color, stroke: "#333", strokeWidth: 1.5 }),
    e("path", { d: "M0,2 C-2,0 -4,2 -4,4 C-4,6 0,9 0,9 C0,9 4,6 4,4 C4,2 2,0 0,2", fill: "#f66" }),
    e("ellipse", { cx: -12, cy: 4, rx: 3, ry: 5, fill: color, stroke: "#333", strokeWidth: 1 }),
    e("ellipse", { cx: 12, cy: 4, rx: 3, ry: 5, fill: color, stroke: "#333", strokeWidth: 1 }),
    e("ellipse", { cx: -5 + legOffset, cy: 16, rx: 4, ry: 5, fill: color, stroke: "#333", strokeWidth: 1 }),
    e("ellipse", { cx: 5 - legOffset, cy: 16, rx: 4, ry: 5, fill: color, stroke: "#333", strokeWidth: 1 }),
    e("text", { x: 0, y: 32, textAnchor: "middle", fontSize: 8, fill: "#333", fontWeight: 500 }, label)
  );
}


// SECRET SERVICE ROBOT - Sandbox (sunglasses, earpiece, suit)
function SecretServiceRobot(props) {
  var x = props.x, y = props.y, color = props.color, label = props.label;
  var isWalking = props.isWalking, onClick = props.onClick;
  var lookDir = props.lookDir || 0, blinkState = props.blinkState || 0;
  
  var now = Date.now();
  var bobOffset = isWalking ? Math.sin(now / 100) * 2 : 0;
  var legOffset = isWalking ? Math.sin(now / 80) * 3 : 0;
  var eyeOffsetX = lookDir * 1.5;
  
  return e("g", { transform: "translate(" + x + "," + (y + bobOffset) + ")", onClick: onClick, style: { cursor: "pointer" } },
    // Earpiece wire
    e("path", { d: "M10,-18 Q16,-16 14,-8 Q12,-4 14,0", fill: "none", stroke: "#333", strokeWidth: 1.5 }),
    e("circle", { cx: 10, cy: -18, r: 2.5, fill: "#333" }),
    // Head
    e("rect", { x: -11, y: -24, width: 22, height: 20, rx: 4, fill: color, stroke: "#333", strokeWidth: 1.5 }),
    // Slicked back hair
    e("rect", { x: -10, y: -26, width: 20, height: 6, rx: 2, fill: "#222" }),
    // Sunglasses (always on, no blinking visible)
    e("rect", { x: -10, y: -20, width: 9, height: 6, rx: 1, fill: "#111", stroke: "#333", strokeWidth: 1 }),
    e("rect", { x: 1, y: -20, width: 9, height: 6, rx: 1, fill: "#111", stroke: "#333", strokeWidth: 1 }),
    e("line", { x1: -1, y1: -17, x2: 1, y2: -17, stroke: "#333", strokeWidth: 1.5 }),
    e("line", { x1: -10, y1: -17, x2: -13, y2: -18, stroke: "#333", strokeWidth: 1 }),
    e("line", { x1: 10, y1: -17, x2: 13, y2: -18, stroke: "#333", strokeWidth: 1 }),
    // Reflection on glasses
    e("line", { x1: -8, y1: -19, x2: -5, y2: -16, stroke: "#444", strokeWidth: 0.5 }),
    e("line", { x1: 3, y1: -19, x2: 6, y2: -16, stroke: "#444", strokeWidth: 0.5 }),
    // Stern mouth
    e("line", { x1: -4, y1: -10, x2: 4, y2: -10, stroke: "#333", strokeWidth: 1.5 }),
    // Body (black suit)
    e("rect", { x: -12, y: -4, width: 24, height: 18, rx: 3, fill: "#222", stroke: "#111", strokeWidth: 1.5 }),
    // White shirt and tie
    e("rect", { x: -3, y: -4, width: 6, height: 14, fill: "#fff" }),
    e("polygon", { points: "0,-4 -3,-1 0,10 3,-1", fill: "#111" }),
    // Suit lapels
    e("path", { d: "M-3,-4 L-10,2 L-10,-4", fill: "#333" }),
    e("path", { d: "M3,-4 L10,2 L10,-4", fill: "#333" }),
    // Badge on lapel
    e("circle", { cx: -7, cy: 0, r: 2, fill: "#c9a227" }),
    // Arms (hands together or at sides)
    isWalking ? e("g", null,
      e("rect", { x: -16, y: -2 + Math.sin(now / 90) * 3, width: 5, height: 12, rx: 2, fill: "#222" }),
      e("rect", { x: 11, y: -2 - Math.sin(now / 90) * 3, width: 5, height: 12, rx: 2, fill: "#222" })
    ) : e("g", null,
      // Hands clasped in front
      e("rect", { x: -16, y: -2, width: 5, height: 10, rx: 2, fill: "#222" }),
      e("rect", { x: 11, y: -2, width: 5, height: 10, rx: 2, fill: "#222" }),
      e("ellipse", { cx: 0, cy: 12, rx: 6, ry: 3, fill: color })
    ),
    // Legs
    e("rect", { x: -7 + legOffset, y: 14, width: 5, height: 10, rx: 1, fill: "#222" }),
    e("rect", { x: 2 - legOffset, y: 14, width: 5, height: 10, rx: 1, fill: "#222" }),
    // Shoes
    e("rect", { x: -8 + legOffset, y: 22, width: 7, height: 3, rx: 1, fill: "#111" }),
    e("rect", { x: 1 - legOffset, y: 22, width: 7, height: 3, rx: 1, fill: "#111" }),
    e("text", { x: 0, y: 38, textAnchor: "middle", fontSize: 8, fill: "#333", fontWeight: 500 }, label)
  );
}

// Robot renderer helper
function renderRobot(key, props) {
  var type = ROBOT_TYPES[key];
  switch(type) {
    case "boss": return e(BossRobot, props);
    case "nerdy": return e(NerdyRobot, props);
    case "worker": return e(WorkerRobot, props);
    case "tech": return e(TechRobot, props);
    case "driver": return e(DriverRobot, props);
    case "retro": return e(RetroRobot, props);
    case "guard": return e(GuardRobot, props);
    case "friendly": return e(FriendlyRobot, props);
    case "secretservice": return e(SecretServiceRobot, props);
    default: return e(FriendlyRobot, props);
  }
}
