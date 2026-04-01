// AIOS Status Page - Main Application

function StatusPage() {
  // Logs
  var logState = useState([]);
  var logs = logState[0], setLogs = logState[1];
  
  function addLog(pd, action, warn) {
    setLogs(function(l) { 
      return l.concat([{ time: getTimestamp(), pd: pd, action: action, warn: warn }]).slice(-20); 
    });
  }
  
  // Processes
  var procState = useState(INITIAL_PROCESSES);
  var processes = procState[0], setProcesses = procState[1];
  
  // Tooltip
  var tooltipState = useState(null);
  var tooltip = tooltipState[0], setTooltip = tooltipState[1];
  
  // Custom hooks
  var robots = useRobots();
  var cleanerHook = useCleaner([], function(){}, addLog);
  var pets = usePets(addLog, cleanerHook.dispatch);
  var transporter = useTransporter(processes, setProcesses, addLog);
  var ipc = useIPC(addLog, robots.setRobotStates);
  
  // Update cleaner with current messes
  var cleanerWithMesses = useCleaner(pets.messes, pets.cleanMess, addLog);

  // Main update loop
  useEffect(function() {
    var interval = setInterval(function() {
      // Robots
      robots.updatePositions();
      robots.updateWalkingState();
      robots.triggerBlink();
      robots.triggerWander();
      robots.triggerLook();
      robots.triggerSitting(addLog);
      robots.triggerCoffeeBreak(addLog);
      
      // Pets
      pets.updatePet();
      pets.spawnPet();
      
      // Cleaner
      cleanerWithMesses.updateCleaner();
      cleanerWithMesses.processCleaning();
      cleanerWithMesses.goToFirstMess();
      
      // Transporter
      transporter.beamIn();
      transporter.beamOut();
      
      // IPC
      ipc.triggerIPC();
      
      // Process movement
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
  }, []);

  function handleRobotClick(pd, event) {
    event.stopPropagation();
    setTooltip({ info: PD_INFO[pd], x: event.clientX, y: event.clientY });
    setTimeout(function() { setTooltip(null); }, 3000);
  }
  
  // Check if boss is sitting (to hide him and show on chair)
  var bossSitting = robots.robotStates.orch && robots.robotStates.orch.sitting;

  return e("div", { className: "crt-flicker" },
    e("svg", { width: "100%", viewBox: "0 0 720 520" },
      // Defs
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
      
      // Title
      e("text", { x: 360, y: 26, textAnchor: "middle", fontSize: 18, fontWeight: 600, fill: "#333" }, "AIOS 0.3.x System Status"),
      e("text", { x: 360, y: 42, textAnchor: "middle", fontSize: 11, fill: "#666" }, "8 Protection Domains | seL4 Microkernel"),
      e(LED, { x: 260, y: 34, color: "#0f0", size: 4, blink: 1000 }),
      e(LED, { x: 460, y: 34, color: "#0f0", size: 4, blink: 1200 }),

      // IPC Wires - FIXED coordinates
      // Orch to FS (top right of orch room to left of FS room)
      e(Wire, { x1: 210, y1: 100, x2: 270, y2: 100, isActive: ipc.activeWires["orch-fs"] }),
      // Orch to Net (bottom right of orch to left of net)
      e(Wire, { x1: 210, y1: 180, x2: 270, y2: 220, isActive: ipc.activeWires["orch-net"] }),
      // Sandbox to Orch (top of sandbox to bottom of orch) - FIXED to go all the way
      e(Wire, { x1: 100, y1: 345, x2: 100, y2: 215, isActive: ipc.activeWires["sandbox-orch"] }),
      // FS to BLK
      e(Wire, { x1: 395, y1: 100, x2: 450, y2: 100, isActive: ipc.activeWires["fs-blk"] }),
      // Net to NetDrv
      e(Wire, { x1: 395, y1: 220, x2: 450, y2: 170, isActive: ipc.activeWires["net-netdrv"] }),
      // Orch to Auth - NEW
      e(Wire, { x1: 210, y1: 150, x2: 610, y2: 275, isActive: ipc.activeWires["orch-auth"] }),
      // Orch to Serial - NEW
      e(Wire, { x1: 210, y1: 120, x2: 610, y2: 100, isActive: ipc.activeWires["orch-serial"] }),

      // Secret door
      e(SecretDoor, { x: 605, y: 500, isOpen: cleanerWithMesses.doorOpen }),

      // ORCHESTRATOR
      e(Room, { x: 20, y: 55, width: 190, height: 160, label: "ORCHESTRATOR (200)", color: "#ffeedd" },
        e(CoffeeMachine, { x: 30, y: 75, brewing: robots.machines.brewing }),
        e(VendingMachine, { x: 30, y: 135, dispensing: robots.machines.dispensing }),
        // Chair (boss sits here)
        e(Chair, { x: 105, y: 110, isOccupied: bossSitting, spin: robots.chair.spin }),
        e(Desk, { x: 150, y: 75 }),
        e(Phone, { x: 170, y: 135, isRinging: ipc.ringingPhones["orch"] }),
        // Only show boss robot if not sitting (when sitting, render on chair)
        !bossSitting ? renderRobot("orch", Object.assign({}, robots.getRobotProps("orch"), { onClick: function(ev) { handleRobotClick("orch", ev); } })) : null,
        // Boss sitting in chair
        bossSitting ? e("g", { transform: "translate(105, 95)" },
          // Seated boss (simplified, facing desk)
          e("rect", { x: -10, y: -46, width: 20, height: 4, fill: "#222" }),
          e("rect", { x: -7, y: -58, width: 14, height: 14, fill: "#222" }),
          e("rect", { x: -7, y: -46, width: 14, height: 2, fill: "#c9a227" }),
          e("rect", { x: -14, y: -30, width: 28, height: 24, rx: 5, fill: "#f90", stroke: "#333", strokeWidth: 2 }),
          e("circle", { cx: 7, cy: -20, r: 6, fill: "none", stroke: "#c9a227", strokeWidth: 1.5 }),
          e("rect", { x: -10, y: -24, width: 6, height: 6, rx: 1, fill: "#fff" }),
          e("rect", { x: 4, y: -24, width: 6, height: 6, rx: 1, fill: "#fff" }),
          e("circle", { cx: -7, cy: -20, r: 2, fill: "#333" }),
          e("circle", { cx: 7, cy: -20, r: 2, fill: "#333" }),
          e("path", { d: "M-8,-12 Q-12,-10 -14,-12 M8,-12 Q12,-10 14,-12", fill: "none", stroke: "#333", strokeWidth: 2, strokeLinecap: "round" }),
          e("polygon", { points: "-6,-6 0,-3 6,-6 6,-9 0,-6 -6,-9", fill: "#d33" }),
          e("rect", { x: -12, y: -4, width: 24, height: 12, rx: 4, fill: "#333", stroke: "#222", strokeWidth: 1.5 }),
          e("text", { x: 0, y: 20, textAnchor: "middle", fontSize: 8, fill: "#333", fontWeight: 600 }, "Boss")
        ) : null
      ),

      // FS_SERVER
      e(Room, { x: 270, y: 55, width: 125, height: 90, label: "FS_SERVER (240)", color: "#ddeeff" },
        e(Disk, { x: 90, y: 55 }),
        e(Phone, { x: 105, y: 70, isRinging: ipc.ringingPhones["fs"] }),
        renderRobot("fs", Object.assign({}, robots.getRobotProps("fs"), { onClick: function(ev) { handleRobotClick("fs", ev); } }))
      ),

      // BLK_DRIVER
      e(Room, { x: 450, y: 55, width: 125, height: 90, label: "BLK_DRIVER (250)", color: "#eeddff" },
        e("rect", { x: 75, y: 32, width: 38, height: 32, rx: 3, fill: "#555", stroke: "#333", strokeWidth: 1 }),
        e("rect", { x: 80, y: 38, width: 28, height: 5, fill: "#0f0" }),
        e(LED, { x: 80, y: 56, color: "#fa0", size: 2, blink: 400 }),
        e(LED, { x: 88, y: 56, color: "#0f0", size: 2, blink: 600 }),
        e("text", { x: 94, y: 50, textAnchor: "middle", fontSize: 6, fill: "#fff" }, "virtio-blk"),
        e(Phone, { x: 105, y: 70, isRinging: ipc.ringingPhones["blk"] }),
        renderRobot("blk", Object.assign({}, robots.getRobotProps("blk"), { onClick: function(ev) { handleRobotClick("blk", ev); } }))
      ),

      // NET_SERVER
      e(Room, { x: 270, y: 175, width: 125, height: 90, label: "NET_SERVER (210)", color: "#ddffee" },
        e("ellipse", { cx: 90, cy: 48, rx: 20, ry: 14, fill: "none", stroke: "#0a6", strokeWidth: 2 }),
        e("text", { x: 90, y: 52, textAnchor: "middle", fontSize: 7, fill: "#666" }, "TCP/IP"),
        e(LED, { x: 78, y: 35, color: "#0f0", size: 2, blink: 500 }),
        e(LED, { x: 102, y: 35, color: "#0af", size: 2, blink: 700 }),
        e(Phone, { x: 105, y: 70, isRinging: ipc.ringingPhones["net"] }),
        renderRobot("net", Object.assign({}, robots.getRobotProps("net"), { onClick: function(ev) { handleRobotClick("net", ev); } }))
      ),

      // NET_DRIVER
      e(Room, { x: 450, y: 125, width: 125, height: 90, label: "NET_DRIVER (230)", color: "#ffffdd" },
        e("rect", { x: 75, y: 32, width: 38, height: 28, rx: 3, fill: "#666" }),
        e(LED, { x: 80, y: 52, color: "#0f0", size: 2, blink: 300 }),
        e(LED, { x: 93, y: 52, color: "#fa0", size: 2, blink: 450 }),
        e("text", { x: 94, y: 48, textAnchor: "middle", fontSize: 6, fill: "#fff" }, "virtio-net"),
        e(Phone, { x: 105, y: 70, isRinging: ipc.ringingPhones["netdrv"] }),
        renderRobot("netdrv", Object.assign({}, robots.getRobotProps("netdrv"), { onClick: function(ev) { handleRobotClick("netdrv", ev); } }))
      ),

      // SERIAL
      e(Room, { x: 610, y: 55, width: 95, height: 90, label: "SERIAL (254)", color: "#ffe0e0" },
        e("rect", { x: 55, y: 28, width: 32, height: 22, rx: 2, fill: "#222" }),
        e("text", { x: 71, y: 43, textAnchor: "middle", fontSize: 6, fill: "#0f0" }, "UART"),
        e(LED, { x: 60, y: 45, color: "#0f0", size: 2, blink: 150 }),
        e(LED, { x: 82, y: 45, color: "#f00", size: 2, blink: 200 }),
        e(Phone, { x: 75, y: 65, isRinging: ipc.ringingPhones["serial"] }),
        renderRobot("serial", Object.assign({}, robots.getRobotProps("serial"), { onClick: function(ev) { handleRobotClick("serial", ev); } }))
      ),

      // AUTH
      e(Room, { x: 610, y: 235, width: 95, height: 80, label: "AUTH (210)", color: "#e0e0ff" },
        e("rect", { x: 58, y: 26, width: 26, height: 22, rx: 3, fill: "#fc0" }),
        e("circle", { cx: 71, cy: 32, r: 4, fill: "#333" }),
        e("rect", { x: 68, y: 36, width: 6, height: 8, fill: "#333" }),
        e(LED, { x: 60, y: 44, color: "#0f0", size: 2, on: true }),
        e(Phone, { x: 75, y: 58, isRinging: ipc.ringingPhones["auth"] }),
        renderRobot("auth", Object.assign({}, robots.getRobotProps("auth"), { onClick: function(ev) { handleRobotClick("auth", ev); } }))
      ),

      // SANDBOX
      e(Room, { x: 20, y: 345, width: 555, height: 150, label: "SANDBOX (150) - User-space Kernel", color: "#e8ffe8" },
        e("rect", { x: 12, y: 28, width: 90, height: 75, rx: 4, fill: "rgba(0,0,0,0.05)", stroke: "#bbb", strokeDasharray: "4 2" }),
        e("text", { x: 57, y: 42, textAnchor: "middle", fontSize: 7, fill: "#666" }, "Kernel Zone"),
        e(LED, { x: 20, y: 38, color: "#0f0", size: 2, blink: 800 }),
        e(LED, { x: 94, y: 38, color: "#0af", size: 2, blink: 600 }),
        
        renderRobot("sandbox", Object.assign({}, robots.getRobotProps("sandbox"), { onClick: function(ev) { handleRobotClick("sandbox", ev); } })),

        e("rect", { x: 115, y: 28, width: 310, height: 105, rx: 4, fill: "rgba(255,255,255,0.6)", stroke: "#bbb", strokeDasharray: "4 2" }),
        e("text", { x: 270, y: 42, textAnchor: "middle", fontSize: 7, fill: "#666" }, "User Memory Pool"),
        
        e(Transporter, { x: 480, y: 85, isActive: transporter.transporter.active, beamIn: transporter.transporter.beamIn, beamOut: transporter.transporter.beamOut }),
        
        processes.map(function(p) {
          return e(Human, { key: p.id, x: 135 + p.x, y: 95, label: p.name, color: p.color, isWalking: Math.abs(p.vx) > 0.1, isBeaming: p.isBeaming });
        }),

        e(Phone, { x: 18, y: 115, isRinging: ipc.ringingPhones["sandbox"] }),
        e("text", { x: 420, y: 125, textAnchor: "end", fontSize: 7, fill: "#666" }, "processes: " + processes.length)
      ),

      // Messes
      pets.messes.map(function(m) {
        return e(Mess, { key: m.id, x: m.x, y: m.y, type: m.type });
      }),

      // Pet
      pets.pet ? (pets.pet.type === "cat" ? 
        e(Cat, { x: pets.pet.x, y: pets.pet.y, isWalking: pets.pet.isWalking, direction: pets.pet.direction }) :
        e(Dog, { x: pets.pet.x, y: pets.pet.y, isWalking: pets.pet.isWalking, direction: pets.pet.direction, isPeeing: pets.pet.isPeeing })
      ) : null,

      // Cleaning robot
      cleanerWithMesses.cleaner.active ? e(CleaningRobot, { 
        x: cleanerWithMesses.cleaner.x, 
        y: cleanerWithMesses.cleaner.y, 
        isMoving: Math.abs(cleanerWithMesses.cleaner.targetX - cleanerWithMesses.cleaner.x) > 2, 
        isCleaning: cleanerWithMesses.cleaner.cleaning 
      }) : null,

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

// Mount
ReactDOM.render(e(StatusPage), document.getElementById("root"));
