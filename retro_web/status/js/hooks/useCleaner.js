// useCleaner - Cleaning robot dispatch and behavior

function useCleaner(messes, cleanMess, addLog) {
  var cleanerState = useState({ active: false, x: 590, y: 490, targetX: 590, targetY: 490, cleaning: false });
  var cleaner = cleanerState[0], setCleaner = cleanerState[1];
  
  var doorState = useState(false);
  var doorOpen = doorState[0], setDoorOpen = doorState[1];

  function dispatch() {
    setDoorOpen(true);
    setCleaner(function(c) { return Object.assign({}, c, { active: true }); });
  }

  function updateCleaner() {
    if (!cleaner.active) return;
    
    setCleaner(function(c) {
      var dx = c.targetX - c.x;
      var dy = c.targetY - c.y;
      var newX = c.x + dx * 0.02;
      var newY = c.y + dy * 0.02;
      var isMoving = Math.abs(dx) > 2 || Math.abs(dy) > 2;
      
      if (!isMoving && messes.length > 0 && !c.cleaning) {
        return Object.assign({}, c, { x: newX, y: newY, cleaning: true });
      }
      
      return Object.assign({}, c, { x: newX, y: newY });
    });
  }

  function processCleaning() {
    if (cleaner.cleaning && messes.length > 0) {
      setTimeout(function() {
        cleanMess();
        addLog("system", "CLEAN-O: mess cleaned!");
        setCleaner(function(c) { return Object.assign({}, c, { cleaning: false }); });
        
        setTimeout(function() {
          if (messes.length <= 1) {
            setCleaner(function(c) { return Object.assign({}, c, { targetX: 590, targetY: 490 }); });
            setTimeout(function() {
              setCleaner(function(c) { return Object.assign({}, c, { active: false }); });
              setDoorOpen(false);
            }, 3000);
          } else {
            var nextMess = messes[1];
            setCleaner(function(c) { return Object.assign({}, c, { targetX: nextMess.x, targetY: nextMess.y }); });
          }
        }, 500);
      }, 2000);
    }
  }

  function goToFirstMess() {
    if (cleaner.active && messes.length > 0 && cleaner.targetX === 590 && cleaner.targetY === 490) {
      var firstMess = messes[0];
      setCleaner(function(c) { return Object.assign({}, c, { targetX: firstMess.x, targetY: firstMess.y }); });
    }
  }

  return {
    cleaner: cleaner,
    doorOpen: doorOpen,
    dispatch: dispatch,
    updateCleaner: updateCleaner,
    processCleaning: processCleaning,
    goToFirstMess: goToFirstMess
  };
}
