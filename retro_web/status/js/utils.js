// AIOS Status Page - Utility Functions

function getTimestamp() {
  var now = new Date();
  return String(now.getHours()).padStart(2, "0") + ":" + 
         String(now.getMinutes()).padStart(2, "0") + ":" + 
         String(now.getSeconds()).padStart(2, "0");
}

function randomChoice(arr) {
  return arr[Math.floor(Math.random() * arr.length)];
}

function clamp(val, min, max) {
  return Math.max(min, Math.min(max, val));
}

function distance(x1, y1, x2, y2) {
  return Math.sqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
}
