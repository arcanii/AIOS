// useIPC - IPC wire activity

function useIPC(addLog, setRobotStates) {
  var wireState = useState({});
  var activeWires = wireState[0], setActiveWires = wireState[1];
  
  var phoneState = useState({});
  var ringingPhones = phoneState[0], setRingingPhones = phoneState[1];

  function triggerIPC() {
    if (Math.random() < 0.05) {
      var wire = randomChoice(WIRE_PAIRS);
      setActiveWires(function(w) { var n = Object.assign({}, w); n[wire] = true; return n; });
      var target = wire.split("-")[1];
      setRingingPhones(function(p) { var n = Object.assign({}, p); n[target] = true; return n; });
      setRobotStates(function(s) { var n = Object.assign({}, s); if (n[target]) n[target] = Object.assign({}, s[target], { lookDir: -1 }); return n; });
      
      var msg = randomChoice(LOG_MESSAGES);
      addLog(msg.pd, msg.action, msg.warn);
      
      setTimeout(function() {
        setActiveWires(function(w) { var n = Object.assign({}, w); n[wire] = false; return n; });
        setRingingPhones(function(p) { var n = Object.assign({}, p); n[target] = false; return n; });
      }, 800);
    }
  }

  return {
    activeWires: activeWires,
    ringingPhones: ringingPhones,
    triggerIPC: triggerIPC
  };
}
