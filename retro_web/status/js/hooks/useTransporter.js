// useTransporter - Beam in/out processes

function useTransporter(processes, setProcesses, addLog) {
  var transporterState = useState({ active: false, beamIn: false, beamOut: false });
  var transporter = transporterState[0], setTransporter = transporterState[1];
  
  var processIdState = useState(4);
  var nextProcessId = processIdState[0], setNextProcessId = processIdState[1];

  function beamIn() {
    if (Math.random() < 0.003 && !transporter.active && processes.length < 6) {
      var newName = randomChoice(PROCESS_NAMES);
      var newColor = randomChoice(PROCESS_COLORS);
      
      setTransporter({ active: true, beamIn: true, beamOut: false });
      addLog("sandbox", "TRANSPORTER: incoming " + newName);
      
      setTimeout(function() {
        setProcesses(function(procs) {
          var newId = nextProcessId;
          setNextProcessId(function(id) { return id + 1; });
          return procs.concat([{ id: newId, name: newName, x: 450, color: newColor, targetX: 100 + Math.random() * 100, vx: 0, isBeaming: true }]);
        });
      }, 1500);
      
      setTimeout(function() {
        setProcesses(function(procs) { return procs.map(function(p) { return Object.assign({}, p, { isBeaming: false }); }); });
        setTransporter({ active: false, beamIn: false, beamOut: false });
      }, 3000);
    }
  }

  function beamOut() {
    if (Math.random() < 0.002 && !transporter.active && processes.length > 2) {
      var exitIndex = Math.floor(Math.random() * processes.length);
      var exitProc = processes[exitIndex];
      
      addLog("sandbox", "TRANSPORTER: " + exitProc.name + " beaming out");
      setTransporter({ active: true, beamIn: false, beamOut: true });
      
      setProcesses(function(procs) {
        return procs.map(function(p, i) {
          if (i === exitIndex) return Object.assign({}, p, { targetX: 450, isBeaming: true });
          return p;
        });
      });
      
      setTimeout(function() {
        setProcesses(function(procs) { return procs.filter(function(p, i) { return i !== exitIndex; }); });
        setTransporter({ active: false, beamIn: false, beamOut: false });
      }, 2500);
    }
  }

  return {
    transporter: transporter,
    beamIn: beamIn,
    beamOut: beamOut
  };
}
