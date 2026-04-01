// UI Components - LogTicker, Tooltip

function LogTicker(props) {
  var logs = props.logs;
  return e("div", { className: "log-ticker" },
    logs.slice(-4).map(function(log, i) {
      var cls = log.warn ? "log-warn" : (log.err ? "log-err" : "log-action");
      return e("div", { key: i, className: "log-line" },
        e("span", { className: "log-time" }, log.time + " "),
        e("span", { className: "log-pd" }, "[" + log.pd.toUpperCase() + "] "),
        e("span", { className: cls }, log.action)
      );
    })
  );
}

function Tooltip(props) {
  if (!props.info) return null;
  return e("div", { className: "tooltip", style: { left: props.x + 10, top: props.y - 60 } },
    e("div", { className: "tooltip-title" }, props.info.name + " (P" + props.info.priority + ")"),
    e("div", null, props.info.desc)
  );
}
