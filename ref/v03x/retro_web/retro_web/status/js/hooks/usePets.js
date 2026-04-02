// usePets - Pet spawning and behavior

function usePets(addLog, dispatchCleaner) {
  var petState = useState(null);
  var pet = petState[0], setPet = petState[1];
  
  var messState = useState([]);
  var messes = messState[0], setMesses = messState[1];

  function updatePet() {
    if (!pet) return;
    
    setPet(function(p) {
      if (!p) return null;
      var dx = p.targetX - p.x;
      var dy = p.targetY - p.y;
      var newX = p.x + dx * 0.015;
      var newY = p.y + dy * 0.015;
      var isMoving = Math.abs(dx) > 2 || Math.abs(dy) > 2;
      var dir = dx > 0 ? 1 : -1;
      
      if (!isMoving && !p.didAction && p.actionType) {
        setMesses(function(m) { return m.concat([{ x: p.targetX, y: p.targetY + 10, type: p.actionType, id: Date.now() }]); });
        addLog("system", "ALERT: " + p.type + " made a mess!", true);
        setTimeout(dispatchCleaner, 500);
        return Object.assign({}, p, { x: newX, y: newY, isWalking: false, didAction: true, isPeeing: p.actionType === "pee" });
      }
      
      if (p.isPeeing && p.didAction) {
        setTimeout(function() { setPet(function(pp) { return pp ? Object.assign({}, pp, { isPeeing: false }) : null; }); }, 1500);
      }
      
      if (p.didAction && !p.leaving) {
        setTimeout(function() {
          setPet(function(pp) {
            if (!pp) return null;
            return Object.assign({}, pp, { leaving: true, targetX: -50, targetY: pp.y });
          });
        }, 3000);
      }
      
      if (p.leaving && newX < -40) return null;
      
      return Object.assign({}, p, { x: newX, y: newY, isWalking: isMoving, direction: dir });
    });
  }

  function spawnPet() {
    if (Math.random() < 0.002 && !pet) {
      var petType = Math.random() < 0.5 ? "cat" : "dog";
      var startY = 100 + Math.random() * 350;
      var targetX = 100 + Math.random() * 500;
      var targetY = startY + (Math.random() - 0.5) * 100;
      var actionType = petType === "dog" ? (Math.random() < 0.5 ? "pee" : "poop") : null;
      
      addLog("system", "INTRUDER: " + petType + " detected!", true);
      
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
  }

  function cleanMess() {
    setMesses(function(m) { return m.slice(1); });
  }

  return {
    pet: pet,
    messes: messes,
    updatePet: updatePet,
    spawnPet: spawnPet,
    cleanMess: cleanMess
  };
}
