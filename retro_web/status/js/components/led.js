// LED Component

function LED(props) {
  var color = props.color, on = props.on, blink = props.blink;
  var x = props.x, y = props.y, size = props.size || 4;
  var isOn = on;
  if (blink) isOn = Math.floor(Date.now() / blink) % 2 === 0;
  return e("g", null,
    e("circle", { cx: x, cy: y, r: size, fill: isOn ? color : "#333", stroke: "#222", strokeWidth: 0.5 }),
    isOn ? e("circle", { cx: x, cy: y, r: size * 0.6, fill: color, opacity: 0.6 }) : null
  );
}
