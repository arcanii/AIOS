// useRobots - Robot state and movement logic

function useRobots() {
  var initPositions = {};
  var initStates = {};
  
  ROBOT_KEYS.forEach(function(k) {
    initPositions[k] = { x: 0, y: 0, targetX: 0, targetY: 0 };
    initStates[k] = { walking: false, lookDir: 0, blinkState: 0, holdingCoffee: false, holdingSnack: false, activity: "idle", sitting: false };
  });
  
  var posState = useState(initPositions);
  var robotPositions = posState[0], setRobotPositions = posState[1];
  
  var stateState = useState(initStates);
  var robotStates = stateState[0], setRobotStates = stateState[1];
  
  var machineState = useState({ brewing: false, dispensing: false });
  var machines = machineState[0], setMachines = machineState[1];
  
  var chairState = useState({ spin: 0, targetSpin: 0 });
  var chair = chairState[0], setChair = chairState[1];

  function updatePositions() {
    setRobotPositions(function(pos) {
      var newPos = {};
      ROBOT_KEYS.forEach(function(key) {
        var p = pos[key];
        var dx = p.targetX - p.x;
        var dy = p.targetY - p.y;
        newPos[key] = { x: p.x + dx * 0.02, y: p.y + dy * 0.02, targetX: p.targetX, targetY: p.targetY };
      });
      return newPos;
    });
    
    // Update chair spin
    setChair(function(c) {
      var dSpin = c.targetSpin - c.spin;
      return { spin: c.spin + dSpin * 0.05, targetSpin: c.targetSpin };
    });
  }

  function updateWalkingState() {
    setRobotStates(function(states) {
      var newStates = Object.assign({}, states);
      ROBOT_KEYS.forEach(function(key) {
        var p = robotPositions[key];
        var dx = p.targetX - p.x;
        var dy = p.targetY - p.y;
        var isMoving = Math.abs(dx) > 0.5 || Math.abs(dy) > 0.5;
        newStates[key] = Object.assign({}, states[key], { walking: isMoving && !states[key].sitting });
      });
      return newStates;
    });
  }

  function triggerBlink() {
    if (Math.random() < 0.03) {
      var robot = randomChoice(ROBOT_KEYS);
      setRobotStates(function(s) { var n = Object.assign({}, s); n[robot] = Object.assign({}, s[robot], { blinkState: 1 }); return n; });
      setTimeout(function() { setRobotStates(function(s) { var n = Object.assign({}, s); n[robot] = Object.assign({}, s[robot], { blinkState: 2 }); return n; }); }, 50);
      setTimeout(function() { setRobotStates(function(s) { var n = Object.assign({}, s); n[robot] = Object.assign({}, s[robot], { blinkState: 1 }); return n; }); }, 100);
      setTimeout(function() { setRobotStates(function(s) { var n = Object.assign({}, s); n[robot] = Object.assign({}, s[robot], { blinkState: 0 }); return n; }); }, 150);
    }
  }

  function triggerWander() {
    if (Math.random() < 0.01) {
      var robot = randomChoice(ROBOT_KEYS);
      if (robot === "orch" && (robotStates.orch.activity !== "idle" || robotStates.orch.sitting)) return;
      
      var config = ROBOT_CONFIG[robot];
      var goToEquip = Math.random() < 0.4;
      var newTargetX, newTargetY, newLookDir;
      
      if (goToEquip) {
        newTargetX = (config.equipX - config.homeX) * 0.3;
        newTargetY = (config.equipY - config.homeY) * 0.3;
        newLookDir = config.equipX > config.homeX ? 1 : -1;
      } else {
        newTargetX = (Math.random() - 0.5) * config.rangeX;
        newTargetY = (Math.random() - 0.5) * config.rangeY;
        newLookDir = Math.random() < 0.33 ? -1 : (Math.random() < 0.5 ? 0 : 1);
      }
      
      setRobotPositions(function(pos) { var n = Object.assign({}, pos); n[robot] = Object.assign({}, pos[robot], { targetX: newTargetX, targetY: newTargetY }); return n; });
      setRobotStates(function(s) { var n = Object.assign({}, s); n[robot] = Object.assign({}, s[robot], { lookDir: newLookDir }); return n; });
    }
  }

  function triggerLook() {
    if (Math.random() < 0.02) {
      var robot = randomChoice(ROBOT_KEYS);
      var newLook = Math.random() < 0.33 ? -1 : (Math.random() < 0.5 ? 0 : 1);
      setRobotStates(function(s) { var n = Object.assign({}, s); n[robot] = Object.assign({}, s[robot], { lookDir: newLook }); return n; });
    }
  }
  
  function triggerSitting(addLog) {
    // Boss randomly sits in chair
    if (Math.random() < 0.004) {
      var orchState = robotStates.orch;
      if (orchState.activity !== "idle" || orchState.holdingCoffee || orchState.holdingSnack) return;
      
      if (!orchState.sitting) {
        // Go to chair and sit
        var chairX = 85;  // Chair position relative to room
        var chairY = 55;
        var config = ROBOT_CONFIG.orch;
        
        setRobotPositions(function(pos) { 
          var n = Object.assign({}, pos); 
          n.orch = Object.assign({}, pos.orch, { targetX: chairX - config.homeX, targetY: chairY - config.homeY }); 
          return n; 
        });
        
        setTimeout(function() {
          setRobotStates(function(s) { 
            var n = Object.assign({}, s); 
            n.orch = Object.assign({}, s.orch, { sitting: true, lookDir: 1 }); 
            return n; 
          });
          addLog("orch", "reviewing status reports...");
          
          // Spin chair occasionally while sitting
          var spinInterval = setInterval(function() {
            if (Math.random() < 0.3) {
              setChair(function(c) { return { spin: c.spin, targetSpin: c.targetSpin + (Math.random() - 0.5) * 30 }; });
            }
          }, 1000);
          
          // Get up after a while
          setTimeout(function() {
            clearInterval(spinInterval);
            setRobotStates(function(s) { 
              var n = Object.assign({}, s); 
              n.orch = Object.assign({}, s.orch, { sitting: false }); 
              return n; 
            });
            setChair({ spin: 0, targetSpin: 0 });
            // Move away from chair
            setRobotPositions(function(pos) { 
              var n = Object.assign({}, pos); 
              n.orch = Object.assign({}, pos.orch, { targetX: 20, targetY: 0 }); 
              return n; 
            });
          }, 6000 + Math.random() * 4000);
        }, 1500);
      }
    }
  }

  function triggerCoffeeBreak(addLog) {
    if (Math.random() < 0.005) {
      var orchState = robotStates.orch;
      if (orchState.activity !== "idle" || orchState.holdingCoffee || orchState.holdingSnack || orchState.sitting) return;
      
      var goForCoffee = Math.random() < 0.5;
      var config = ROBOT_CONFIG.orch;
      
      if (goForCoffee) {
        setRobotPositions(function(pos) { var n = Object.assign({}, pos); n.orch = Object.assign({}, pos.orch, { targetX: config.coffeeX - config.homeX, targetY: config.coffeeY - config.homeY }); return n; });
        setRobotStates(function(s) { var n = Object.assign({}, s); n.orch = Object.assign({}, s.orch, { activity: "coffee", lookDir: -1 }); return n; });
        setTimeout(function() { setMachines(function(m) { return Object.assign({}, m, { brewing: true }); }); addLog("orch", "brewing espresso..."); }, 2000);
        setTimeout(function() { setMachines(function(m) { return Object.assign({}, m, { brewing: false }); }); setRobotStates(function(s) { var n = Object.assign({}, s); n.orch = Object.assign({}, s.orch, { holdingCoffee: true }); return n; }); }, 4000);
        setTimeout(function() { setRobotPositions(function(pos) { var n = Object.assign({}, pos); n.orch = Object.assign({}, pos.orch, { targetX: 20, targetY: 0 }); return n; }); setRobotStates(function(s) { var n = Object.assign({}, s); n.orch = Object.assign({}, s.orch, { lookDir: 1 }); return n; }); }, 5000);
        setTimeout(function() { setRobotStates(function(s) { var n = Object.assign({}, s); n.orch = Object.assign({}, s.orch, { holdingCoffee: false, activity: "idle" }); return n; }); }, 10000);
      } else {
        setRobotPositions(function(pos) { var n = Object.assign({}, pos); n.orch = Object.assign({}, pos.orch, { targetX: config.vendingX - config.homeX, targetY: config.vendingY - config.homeY }); return n; });
        setRobotStates(function(s) { var n = Object.assign({}, s); n.orch = Object.assign({}, s.orch, { activity: "snack", lookDir: -1 }); return n; });
        setTimeout(function() { setMachines(function(m) { return Object.assign({}, m, { dispensing: true }); }); addLog("orch", "getting chips..."); }, 2000);
        setTimeout(function() { setMachines(function(m) { return Object.assign({}, m, { dispensing: false }); }); setRobotStates(function(s) { var n = Object.assign({}, s); n.orch = Object.assign({}, s.orch, { holdingSnack: true }); return n; }); }, 3000);
        setTimeout(function() { setRobotPositions(function(pos) { var n = Object.assign({}, pos); n.orch = Object.assign({}, pos.orch, { targetX: 20, targetY: -10 }); return n; }); setRobotStates(function(s) { var n = Object.assign({}, s); n.orch = Object.assign({}, s.orch, { lookDir: 1 }); return n; }); }, 4000);
        setTimeout(function() { setRobotStates(function(s) { var n = Object.assign({}, s); n.orch = Object.assign({}, s.orch, { holdingSnack: false, activity: "idle" }); return n; }); }, 9000);
      }
    }
  }

  function getRobotProps(key) {
    var config = ROBOT_CONFIG[key];
    var pos = robotPositions[key];
    var state = robotStates[key];
    return {
      x: config.homeX + pos.x,
      y: config.homeY + pos.y,
      color: ROBOT_COLORS[key],
      label: ROBOT_LABELS[key],
      isWalking: state.walking,
      lookDir: state.lookDir,
      blinkState: state.blinkState,
      holdingCoffee: state.holdingCoffee,
      holdingSnack: state.holdingSnack,
      sitting: state.sitting
    };
  }

  return {
    robotPositions: robotPositions,
    robotStates: robotStates,
    machines: machines,
    chair: chair,
    updatePositions: updatePositions,
    updateWalkingState: updateWalkingState,
    triggerBlink: triggerBlink,
    triggerWander: triggerWander,
    triggerLook: triggerLook,
    triggerSitting: triggerSitting,
    triggerCoffeeBreak: triggerCoffeeBreak,
    getRobotProps: getRobotProps,
    setRobotStates: setRobotStates
  };
}
