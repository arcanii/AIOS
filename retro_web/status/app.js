// AIOS Status Page - Main Application

var useState = React.useState;
var useEffect = React.useEffect;

var ROBOT_CONFIG = {
  orch: { homeX: 95, homeY: 115, rangeX: 50, rangeY: 20, equipX: 130, equipY: 90, coffeeX: 30, coffeeY: 75, vendingX: 30, vendingY: 130 },
  fs: { homeX: 40, homeY: 60, rangeX: 35, rangeY: 15, equipX: 90, equipY: 55 },
  blk: { homeX: 40, homeY: 60, rangeX: 30, rangeY: 15, equipX: 94, equipY: 44 },
  net: { homeX: 40, homeY: 60, rangeX: 35, rangeY: 15, equipX: 90, equipY: 48 },
  netdrv: { homeX: 40, homeY: 60, rangeX: 30, rangeY: 15, equipX: 94, equipY: 40 },
  serial: { homeX: 35, homeY: 55, rangeX: 25, rangeY: 12, equipX: 71, equipY: 35 },
  auth: { homeX: 35, homeY: 52, rangeX: 25, rangeY: 10, equipX: 71, equipY: 37 },
  sandbox: { homeX: 57, homeY: 80, rangeX: 25, rangeY: 15, equipX: 57, equipY: 42 }
};

// Robot type mapping
var ROBOT_TYPES = {
  orch: "boss",
  fs: "nerdy",
  blk: "worker",
  net: "tech",
  netdrv: "driver",
  serial: "retro",
  auth: "guard",
  sandbox: "friendly"
};

function StatusPage() {
  var tickState = useState(0);
  var tick = tickState[0], setTick = tickState[1];
  
  var wireState = useState({});
  var activeWires = wireState[0], setActiveWires = wireState[1];
  
  var phoneState = useState({});
  var ringingPhones = phoneState[0], setRingingPhones = phoneState[1];
  
  var initPositions = {};
  ROBOT_KEYS.forEach(function(k) { 
    initPositions[k] = { x: 0, y: 0, targetX: 0, targetY: 0 }; 
  });
  var posState = useState(initPositions);
  var robotPositions = posState[0], setRobotPositions = posState[1];
  
  var initStates = {};
  ROBOT_KEYS.forEach(function(k) { 
    initStates[k] = { walking: false, lookDir: 0, blinkState: 0, holdingCoffee: false, holdingSnack: false, activity: "idle" }; 
  });
  var robotState = useState(initStates);
  var robotStates = robotState[0], setRobotStates = robotState[1];
  
  var machineState = useState({ brewing: false, dispensing: false });
  var machines = machineState[0], setMachines = machineState[1];
  
  var procState = useState(INITIAL_PROCESSES);
  var processes = procState[0], setProcesses = procState[1];
  
  var logState = useState([]);
  var logs = logState[0], setLogs = logState[1];
  
  var tooltipState = useState(null);
  var tooltip = tooltipState[0], setTooltip = tooltipState[1];
  
  // Pet state
  var petState = useState(null);
  var pet = petState[0], setPet = petState[1];
  
  // Mess state
  var messState = useState([]);
  var messes = messState[0], setMesses = messState[1];
  
  // Cleaning robot state
  var cleanerState = useState({ active: false, x: 590, y: 490, targetX: 590, targetY: 490, cleaning: false });
  var cleaner = cleanerState[0], setCleaner = cleanerState[1];
  
  // Secret door state
  var doorState = useState(false);
  var doorOpen = doorState[0], setDoorOpen = doorState[1];
  
  // Transporter state
  var transporterState = useState({ active: false, beamIn: false, beamOut: false, pendingProcess: null });
  var transporter = transporterState[0], setTransporter = transporterState[1];
  
  // Process ID counter
  var processIdState = useState(4);
  var nextProcessId = processIdState[0], setNextProcessId = processIdState[1];

  function addLog(pd, action, warn) {
    var now = new Date();
    var time = String(now.getHours()).padStart(2, "0") + ":" + String(now.getMinutes()).padStart(2, "0") + ":" + String(now.getSeconds()).padStart(2, "0");
    setLogs(function(l) { return l.concat([{ time: time, pd: pd, action: action, warn: warn }]).slice(-20); });
  }

  useEffect(function() {
    var interval = setInterval(function() {
      setTick(function(t) { return t + 1; });
      
      // Update robot positions
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
      
      setRobotStates(function(states) {
        var newStates = Object.assign({}, states);
        ROBOT_KEYS.forEach(function(key) {
          var p = robotPositions[key];
          var dx = p.targetX - p.x;
          var dy = p.targetY - p.y;
          var isMoving = Math.abs(dx) > 0.5 || Math.abs(dy) > 0.5;
          newStates[key] = Object.assign({}, states[key], { walking: isMoving });
        });
        return newStates;
      });

      // Pet movement
      if (pet) {
        setPet(function(p) {
          if (!p) return null;
          var dx = p.targetX - p.x;
          var dy = p.targetY - p.y;
          var newX = p.x + dx * 0.015;
          var newY = p.y + dy * 0.015;
          var isMoving = Math.abs(dx) > 2 || Math.abs(dy) > 2;
          var dir = dx > 0 ? 1 : -1;
          
          // Check if reached target
          if (!isMoving && !p.didAction && p.actionType) {
            // Do the action
            if (p.actionType === "pee" || p.actionType === "poop") {
              setMesses(function(m) { return m.concat([{ x: p.targetX, y: p.targetY + 10, type: p.actionType, id: Date.now() }]); });
              addLog("system", "ALERT: " + p.type + " made a mess! Dispatching cleaner...", true);
              // Dispatch cleaner
              setTimeout(function() {
                setDoorOpen(true);
                setCleaner(function(c) { return Object.assign({}, c, { active: true }); });
              }, 500);
            }
            return Object.assign({}, p, { x: newX, y: newY, isWalking: false, didAction: true, isPeeing: p.actionType === "pee" });
          }
          
          // Clear peeing after a moment
          if (p.isPeeing && p.didAction) {
            setTimeout(function() {
              setPet(function(pp) { return pp ? Object.assign({}, pp, { isPeeing: false }) : null; });
            }, 1500);
          }
          
          // Leave after action
          if (p.didAction && !p.leaving) {
            setTimeout(function() {
              setPet(function(pp) {
                if (!pp) return null;
                return Object.assign({}, pp, { leaving: true, targetX: -50, targetY: pp.y });
              });
            }, 3000);
          }
          
          // Remove when off screen
          if (p.leaving && newX < -40) {
            return null;
          }
          
          return Object.assign({}, p, { x: newX, y: newY, isWalking: isMoving, direction: dir });
        });
      }
      
      // Cleaner movement
      if (cleaner.active) {
        setCleaner(function(c) {
          var dx = c.targetX - c.x;
          var dy = c.targetY - c.y;
          var newX = c.x + dx * 0.02;
          var newY = c.y + dy * 0.02;
          var isMoving = Math.abs(dx) > 2 || Math.abs(dy) > 2;
          
          // Check if reached mess
          if (!isMoving && messes.length > 0 && !c.cleaning) {
            // Start cleaning
            return Object.assign({}, c, { x: newX, y: newY, cleaning: true });
          }
          
          return Object.assign({}, c, { x: newX, y: newY });
        });
      }
      
      // Cleaning logic
      if (cleaner.cleaning && messes.length > 0) {
        setTimeout(function() {
          setMesses(function(m) { return m.slice(1); });
          addLog("system", "CLEAN-O: mess cleaned up successfully");
          setCleaner(function(c) { return Object.assign({}, c, { cleaning: false }); });
          
          // Check if more messes
          setTimeout(function() {
            setMesses(function(currentMesses) {
              if (currentMesses.length === 0) {
                // Return home
                setCleaner(function(c) { return Object.assign({}, c, { targetX: 590, targetY: 490 }); });
                setTimeout(function() {
                  setCleaner(function(c) { return Object.assign({}, c, { active: false }); });
                  setDoorOpen(false);
                }, 3000);
              } else {
                // Go to next mess
                var nextMess = currentMesses[0];
                setCleaner(function(c) { return Object.assign({}, c, { targetX: nextMess.x, targetY: nextMess.y }); });
              }
              return currentMesses;
            });
          }, 500);
        }, 2000);
      }
      
      // Go to first mess
      if (cleaner.active && messes.length > 0 && cleaner.targetX === 590 && cleaner.targetY === 490) {
        var firstMess = messes[0];
        setCleaner(function(c) { return Object.assign({}, c, { targetX: firstMess.x, targetY: firstMess.y }); });
      }

      // Random blinking
      if (Math.random() < 0.03) {
        var robot = ROBOT_KEYS[Math.floor(Math.random() * ROBOT_KEYS.length)];
        setRobotStates(function(s) { var n = Object.assign({}, s); n[robot] = Object.assign({}, s[robot], { blinkState: 1 }); return n; });
        setTimeout(function() { setRobotStates(function(s) { var n = Object.assign({}, s); n[robot] = Object.assign({}, s[robot], { blinkState: 2 }); return n; }); }, 50);
        setTimeout(function() { setRobotStates(function(s) { var n = Object.assign({}, s); n[robot] = Object.assign({}, s[robot], { blinkState: 1 }); return n; }); }, 100);
        setTimeout(function() { setRobotStates(function(s) { var n = Object.assign({}, s); n[robot] = Object.assign({}, s[robot], { blinkState: 0 }); return n; }); }, 150);
      }

      // Boss coffee/snack break
      if (Math.random() < 0.006) {
        var orchState = robotStates.orch;
        if (orchState.activity === "idle" && !orchState.holdingCoffee && !orchState.holdingSnack) {
          var goForCoffee = Math.random() < 0.5;
          var config = ROBOT_CONFIG.orch;
          
          if (goForCoffee) {
            setRobotPositions(function(pos) { var n = Object.assign({}, pos); n.orch = Object.assign({}, pos.orch, { targetX: config.coffeeX - config.homeX, targetY: config.coffeeY - config.homeY }); return n; });
            setRobotStates(function(s) { var n = Object.assign({}, s); n.orch = Object.assign({}, s.orch, { activity: "coffee", lookDir: -1 }); return n; });
            setTimeout(function() { setMachines(function(m) { return Object.assign({}, m, { brewing: true }); }); addLog("orch", "brewing espresso... need caffeine"); }, 2000);
            setTimeout(function() { setMachines(function(m) { return Object.assign({}, m, { brewing: false }); }); setRobotStates(function(s) { var n = Object.assign({}, s); n.orch = Object.assign({}, s.orch, { holdingCoffee: true }); return n; }); }, 4000);
            setTimeout(function() { setRobotPositions(function(pos) { var n = Object.assign({}, pos); n.orch = Object.assign({}, pos.orch, { targetX: 20, targetY: 0 }); return n; }); setRobotStates(function(s) { var n = Object.assign({}, s); n.orch = Object.assign({}, s.orch, { lookDir: 1 }); return n; }); }, 5000);
            setTimeout(function() { setRobotStates(function(s) { var n = Object.assign({}, s); n.orch = Object.assign({}, s.orch, { holdingCoffee: false, activity: "idle" }); return n; }); }, 10000);
          } else {
            setRobotPositions(function(pos) { var n = Object.assign({}, pos); n.orch = Object.assign({}, pos.orch, { targetX: config.vendingX - config.homeX, targetY: config.vendingY - config.homeY }); return n; });
            setRobotStates(function(s) { var n = Object.assign({}, s); n.orch = Object.assign({}, s.orch, { activity: "snack", lookDir: -1 }); return n; });
            setTimeout(function() { setMachines(function(m) { return Object.assign({}, m, { dispensing: true }); }); addLog("orch", "getting chips... managing is hungry work"); }, 2000);
            setTimeout(function() { setMachines(function(m) { return Object.assign({}, m, { dispensing: false }); }); setRobotStates(function(s) { var n = Object.assign({}, s); n.orch = Object.assign({}, s.orch, { holdingSnack: true }); return n; }); }, 3000);
            setTimeout(function() { setRobotPositions(function(pos) { var n = Object.assign({}, pos); n.orch = Object.assign({}, pos.orch, { targetX: 20, targetY: -10 }); return n; }); setRobotStates(function(s) { var n = Object.assign({}, s); n.orch = Object.assign({}, s.orch, { lookDir: 1 }); return n; }); }, 4000);
            setTimeout(function() { setRobotStates(function(s) { var n = Object.assign({}, s); n.orch = Object.assign({}, s.orch, { holdingSnack: false, activity: "idle" }); return n; }); }, 9000);
          }
        }
      }

      // Random pet appearance
      if (Math.random() < 0.002 && !pet) {
        var petType = Math.random() < 0.5 ? "cat" : "dog";
        var startY = 100 + Math.random() * 350;
        var targetX = 100 + Math.random() * 500;
        var targetY = startY + (Math.random() - 0.5) * 100;
        var actionType = petType === "dog" ? (Math.random() < 0.5 ? "pee" : "poop") : null;
        
        addLog("system", "INTRUDER: " + petType + " detected in facility!", true);
        
        setPet({
          type: petType,
          x: -30,
          y: startY,
          targetX: targetX,
          targetY: targetY,
          isWalking: true,
          direction: 1,
          actionType: actionType,
          didAction: false,
          isPeeing: false,
          leaving: false
        });
      }

      // Random robot wander
      if (Math.random() < 0.01) {
        var robot = ROBOT_KEYS[Math.floor(Math.random() * ROBOT_KEYS.length)];
        if (robot === "orch" && robotStates.orch.activity !== "idle") {
          // Skip
        } else {
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
      
      // Random look
      if (Math.random() < 0.02) {
        var robot = ROBOT_KEYS[Math.floor(Math.random() * ROBOT_KEYS.length)];
        var newLook = Math.random() < 0.33 ? -1 : (Math.random() < 0.5 ? 0 : 1);
        setRobotStates(function(s) { var n = Object.assign({}, s); n[robot] = Object.assign({}, s[robot], { lookDir: newLook }); return n; });
      }

      // Transporter - beam in new process
      if (Math.random() < 0.003 && !transporter.active && processes.length < 6) {
        var newName = ["daemon", "worker", "logger", "cron", "nginx", "redis"][Math.floor(Math.random() * 6)];
        var newColor = ["#f66", "#6f6", "#66f", "#ff6", "#f6f", "#6ff"][Math.floor(Math.random() * 6)];
        
        setTransporter({ active: true, beamIn: true, beamOut: false });
        addLog("sandbox", "TRANSPORTER: incoming process " + newName);
        
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
      
      // Transporter - beam out process
      if (Math.random() < 0.002 && !transporter.active && processes.length > 2) {
        var exitIndex = Math.floor(Math.random() * processes.length);
        var exitProc = processes[exitIndex];
        
        addLog("sandbox", "TRANSPORTER: process " + exitProc.name + " beaming out");
        setTransporter({ active: true, beamIn: false, beamOut: true });
        
        // Move process to transporter
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

      // IPC activity
      if (Math.random() < 0.05) {
        var wire = WIRE_PAIRS[Math.floor(Math.random() * WIRE_PAIRS.length)];
        setActiveWires(function(w) { var n = Object.assign({}, w); n[wire] = true; return n; });
        var target = wire.split("-")[1];
        setRingingPhones(function(p) { var n = Object.assign({}, p); n[target] = true; return n; });
        setRobotStates(function(s) { var n = Object.assign({}, s); if (n[target]) n[target] = Object.assign({}, s[target], { lookDir: -1 }); return n; });
        
        var msg = LOG_MESSAGES[Math.floor(Math.random() * LOG_MESSAGES.length)];
        addLog(msg.pd, msg.action, msg.warn);
        
        setTimeout(function() {
          setActiveWires(function(w) { var n = Object.assign({}, w); n[wire] = false; return n; });
          setRingingPhones(function(p) { var n = Object.assign({}, p); n[target] = false; return n; });
        }, 800);
      }

      // Update process positions
      setProcesses(function(procs) {
        return procs.map(function(p) {
          if (Math.random() < 0.01 && !p.isBeaming) {
            return Object.assign({}, p, { targetX: 30 + Math.random() * 170 });
          }
          var dx = p.targetX - p.x;
          if (Math.abs(dx) > 1) {
            return Object.assign({}, p, { x: p.x + dx * 0.03, vx: dx > 0 ? 1 : -1 });
          }
          return Object.assign({}, p, { vx: 0 });
        });
      });
    }, 50);
    return function() { clearInterval(interval); };
  }, [robotPositions, robotStates, pet, cleaner, messes, transporter, processes, nextProcessId]);

  function handleRobotClick(pd, event) {
    event.stopPropagation();
    setTooltip({ info: PD_INFO[pd], x: event.clientX, y: event.clientY });
    setTimeout(function() { setTooltip(null); }, 3000);
  }
  
  function getRobotProps(key, color, label) {
    var config = ROBOT_CONFIG[key];
    var pos = robotPositions[key];
    var state = robotStates[key];
    return {
      x: config.homeX + pos.x,
      y: config.homeY + pos.y,
      color: color,
      label: label,
      isWalking: state.walking,
      lookDir: state.lookDir,
      blinkState: state.blinkState,
      holdingCoffee: state.holdingCoffee,
      holdingSnack: state.holdingSnack,
      onClick: function(ev) { handleRobotClick(key, ev); }
    };
  }
  
  // Render robot by type
  function renderRobot(key, color, label) {
    var props = getRobotProps(key, color, label);
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
      default: return e(FriendlyRobot, props);
    }
  }

  return e("div", { className: "crt-flicker" },
    e("svg", { width: "100%", viewBox: "0 0 720 520" },
      e("defs", null,
        e("pattern", { id: "grid", width: 20, height: 20, patternUnits: "userSpaceOnUse" },
          e("path", { d: "M 20 0 L 0 0 0 20", fill: "none", stroke: "#ccc", strokeWidth: 0.5 })
        ),
        e("linearGradient", { id: "beamGradient", x1: "0%", y1: "0%", x2: "0%", y2: "100%" },
          e("stop", { offset: "0%", stopColor: "#0ff", stopOpacity: 0 }),
          e("stop", { offset: "50%", stopColor: "#0ff", stopOpacity: 0.8 }),
          e("stop", { offset: "100%", stopColor: "#0ff", stopOpacity: 0 })
        )
      ),
      e("rect", { width: "100%", height: "100%", fill: "url(#grid)" }),
      
      e("text", { x: 360, y: 26, textAnchor: "middle", fontSize: 18, fontWeight: 600, fill: "#333" }, "AIOS 0.3.x System Status"),
      e("text", { x: 360, y: 42, textAnchor: "middle", fontSize: 11, fill: "#666" }, "8 Protection Domains | seL4 Microkernel"),
      e(LED, { x: 260, y: 34, color: "#0f0", size: 4, blink: 1000 }),
      e(LED, { x: 460, y: 34, color: "#0f0", size: 4, blink: 1200 }),

      // IPC Wires
      e(Wire, { x1: 205, y1: 130, x2: 270, y2: 90, isActive: activeWires["orch-fs"] }),
      e(Wire, { x1: 205, y1: 160, x2: 270, y2: 210, isActive: activeWires["orch-net"] }),
      e(Wire, { x1: 115, y1: 270, x2: 115, y2: 210, isActive: activeWires["sandbox-orch"] }),
      e(Wire, { x1: 395, y1: 90, x2: 450, y2: 90, isActive: activeWires["fs-blk"] }),
      e(Wire, { x1: 395, y1: 230, x2: 450, y2: 165, isActive: activeWires["net-netdrv"] }),

      // Secret door for cleaner
      e(SecretDoor, { x: 605, y: 500, isOpen: doorOpen }),

      // ORCHESTRATOR
      e(Room, { x: 20, y: 55, width: 190, height: 160, label: "ORCHESTRATOR (200)", color: "#ffeedd" },
        e(CoffeeMachine, { x: 30, y: 75, brewing: machines.brewing }),
        e(VendingMachine, { x: 30, y: 135, dispensing: machines.dispensing }),
        e(Desk, { x: 140, y: 95 }),
        e(Phone, { x: 170, y: 135, isRinging: ringingPhones["orch"] }),
        renderRobot("orch", "#f90", "Boss")
      ),

      // FS_SERVER
      e(Room, { x: 270, y: 55, width: 125, height: 90, label: "FS_SERVER (240)", color: "#ddeeff" },
        e(Disk, { x: 90, y: 55 }),
        e(Phone, { x: 105, y: 70, isRinging: ringingPhones["fs"] }),
        renderRobot("fs", "#48f", "FS")
      ),

      // BLK_DRIVER
      e(Room, { x: 450, y: 55, width: 125, height: 90, label: "BLK_DRIVER (250)", color: "#eeddff" },
        e("rect", { x: 75, y: 32, width: 38, height: 32, rx: 3, fill: "#555", stroke: "#333", strokeWidth: 1 }),
        e("rect", { x: 80, y: 38, width: 28, height: 5, fill: "#0f0" }),
        e(LED, { x: 80, y: 56, color: "#fa0", size: 2, blink: 400 }),
        e(LED, { x: 88, y: 56, color: "#0f0", size: 2, blink: 600 }),
        e("text", { x: 94, y: 50, textAnchor: "middle", fontSize: 6, fill: "#fff" }, "virtio-blk"),
        e(Phone, { x: 105, y: 70, isRinging: ringingPhones["blk"] }),
        renderRobot("blk", "#a6f", "Blk")
      ),

      // NET_SERVER
      e(Room, { x: 270, y: 175, width: 125, height: 90, label: "NET_SERVER (210)", color: "#ddffee" },
        e("ellipse", { cx: 90, cy: 48, rx: 20, ry: 14, fill: "none", stroke: "#0a6", strokeWidth: 2 }),
        e("text", { x: 90, y: 52, textAnchor: "middle", fontSize: 7, fill: "#666" }, "TCP/IP"),
        e(LED, { x: 78, y: 35, color: "#0f0", size: 2, blink: 500 }),
        e(LED, { x: 102, y: 35, color: "#0af", size: 2, blink: 700 }),
        e(Phone, { x: 105, y: 70, isRinging: ringingPhones["net"] }),
        renderRobot("net", "#0c8", "Net")
      ),

      // NET_DRIVER
      e(Room, { x: 450, y: 125, width: 125, height: 90, label: "NET_DRIVER (230)", color: "#ffffdd" },
        e("rect", { x: 75, y: 32, width: 38, height: 28, rx: 3, fill: "#666" }),
        e(LED, { x: 80, y: 52, color: "#0f0", size: 2, blink: 300 }),
        e(LED, { x: 93, y: 52, color: "#fa0", size: 2, blink: 450 }),
        e("text", { x: 94, y: 48, textAnchor: "middle", fontSize: 6, fill: "#fff" }, "virtio-net"),
        e(Phone, { x: 105, y: 70, isRinging: ringingPhones["netdrv"] }),
        renderRobot("netdrv", "#cc0", "NetDrv")
      ),

      // SERIAL
      e(Room, { x: 610, y: 55, width: 95, height: 90, label: "SERIAL (254)", color: "#ffe0e0" },
        e("rect", { x: 55, y: 28, width: 32, height: 22, rx: 2, fill: "#222" }),
        e("text", { x: 71, y: 43, textAnchor: "middle", fontSize: 6, fill: "#0f0" }, "UART"),
        e(LED, { x: 60, y: 45, color: "#0f0", size: 2, blink: 150 }),
        e(LED, { x: 82, y: 45, color: "#f00", size: 2, blink: 200 }),
        e(Phone, { x: 75, y: 65, isRinging: ringingPhones["serial"] }),
        renderRobot("serial", "#f66", "Serial")
      ),

      // AUTH
      e(Room, { x: 610, y: 235, width: 95, height: 80, label: "AUTH (210)", color: "#e0e0ff" },
        e("rect", { x: 58, y: 26, width: 26, height: 22, rx: 3, fill: "#fc0" }),
        e("circle", { cx: 71, cy: 32, r: 4, fill: "#333" }),
        e("rect", { x: 68, y: 36, width: 6, height: 8, fill: "#333" }),
        e(LED, { x: 60, y: 44, color: "#0f0", size: 2, on: true }),
        e(Phone, { x: 75, y: 58, isRinging: ringingPhones["auth"] }),
        renderRobot("auth", "#88f", "Auth")
      ),

      // SANDBOX
      e(Room, { x: 20, y: 345, width: 555, height: 150, label: "SANDBOX (150) - User-space Kernel", color: "#e8ffe8" },
        e("rect", { x: 12, y: 28, width: 90, height: 75, rx: 4, fill: "rgba(0,0,0,0.05)", stroke: "#bbb", strokeDasharray: "4 2" }),
        e("text", { x: 57, y: 42, textAnchor: "middle", fontSize: 7, fill: "#666" }, "Kernel Zone"),
        e(LED, { x: 20, y: 38, color: "#0f0", size: 2, blink: 800 }),
        e(LED, { x: 94, y: 38, color: "#0af", size: 2, blink: 600 }),
        
        renderRobot("sandbox", "#6c6", "SbxKernel"),

        e("rect", { x: 115, y: 28, width: 310, height: 105, rx: 4, fill: "rgba(255,255,255,0.6)", stroke: "#bbb", strokeDasharray: "4 2" }),
        e("text", { x: 270, y: 42, textAnchor: "middle", fontSize: 7, fill: "#666" }, "User Memory Pool"),
        
        // Transporter
        e(Transporter, { x: 480, y: 85, isActive: transporter.active, beamIn: transporter.beamIn, beamOut: transporter.beamOut }),
        
        // Processes
        processes.map(function(p) {
          return e(Human, { 
            key: p.id,
            x: 135 + p.x,
            y: 95,
            label: p.name,
            color: p.color,
            isWalking: Math.abs(p.vx) > 0.1,
            isBeaming: p.isBeaming
          });
        }),

        e(Phone, { x: 18, y: 115, isRinging: ringingPhones["sandbox"] }),
        e("text", { x: 420, y: 125, textAnchor: "end", fontSize: 7, fill: "#666" }, "processes: " + processes.length)
      ),

      // Messes
      messes.map(function(m) {
        return e(Mess, { key: m.id, x: m.x, y: m.y, type: m.type });
      }),

      // Pet
      pet ? (pet.type === "cat" ? 
        e(Cat, { x: pet.x, y: pet.y, isWalking: pet.isWalking, direction: pet.direction }) :
        e(Dog, { x: pet.x, y: pet.y, isWalking: pet.isWalking, direction: pet.direction, isPeeing: pet.isPeeing })
      ) : null,

      // Cleaning robot
      cleaner.active ? e(CleaningRobot, { x: cleaner.x, y: cleaner.y, isMoving: Math.abs(cleaner.targetX - cleaner.x) > 2, isCleaning: cleaner.cleaning }) : null,

      // Legend
      e("rect", { x: 25, y: 505, width: 10, height: 10, rx: 2, fill: "#0f0" }),
      e("text", { x: 40, y: 513, fontSize: 9, fill: "#555" }, "Active"),
      e("line", { x1: 85, y1: 510, x2: 120, y2: 510, stroke: "#0af", strokeWidth: 2.5, strokeDasharray: "4 4" }),
      e("text", { x: 125, y: 513, fontSize: 9, fill: "#555" }, "IPC"),
      e("text", { x: 180, y: 513, fontSize: 9, fill: "#888" }, "Click robots for info"),

      e("text", { x: 695, y: 513, textAnchor: "end", fontSize: 9, fill: "#555" }, "QEMU virt | AArch64")
    ),
    e(LogTicker, { logs: logs }),
    e("div", { className: "crt-overlay" }),
    tooltip ? e(Tooltip, { info: tooltip.info, x: tooltip.x, y: tooltip.y }) : null
  );
}

ReactDOM.render(e(StatusPage), document.getElementById("root"));
