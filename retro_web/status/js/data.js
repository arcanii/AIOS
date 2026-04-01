// AIOS Status Page - Data & Configuration

var e = React.createElement;
var useState = React.useState;
var useEffect = React.useEffect;
var useRef = React.useRef;
var useCallback = React.useCallback;

var LOG_MESSAGES = [
  { pd: "orch", action: "routing syscall SYS_OPEN to fs_server" },
  { pd: "fs", action: "ext2: inode lookup /bin/shell" },
  { pd: "sandbox", action: "spawn process pid=4 httpd" },
  { pd: "net", action: "TCP connect 10.0.2.15:80 established" },
  { pd: "blk", action: "virtio-blk: read sector 2048-2056" },
  { pd: "serial", action: "UART TX 64 bytes to console" },
  { pd: "auth", action: "credential check uid=1000 OK" },
  { pd: "sandbox", action: "thread_create tid=7 stack=0x20400000" },
  { pd: "fs", action: "ext2: write inode 42 size=1024" },
  { pd: "orch", action: "preemption tick -> sandbox" },
  { pd: "net", action: "UDP recv 512 bytes from 10.0.2.2:53", warn: true },
  { pd: "sandbox", action: "mutex_lock tid=3 blocked", warn: true },
  { pd: "blk", action: "virtio-blk: write sector 4096" },
  { pd: "orch", action: "PPC call from sandbox completed" },
  { pd: "fs", action: "ext2: readdir /home count=12" },
  { pd: "sandbox", action: "sbrk pid=2 +4096 bytes heap" },
  { pd: "netdrv", action: "virtio-net: TX 128 bytes" },
  { pd: "netdrv", action: "virtio-net: RX 256 bytes" },
  { pd: "serial", action: "UART RX newline -> shell" }
];

var PD_INFO = {
  orch: { name: "Orchestrator", priority: 200, desc: "Service router and policy enforcer. Routes syscalls to appropriate PDs." },
  fs: { name: "FS Server", priority: 240, desc: "ext2 filesystem. Handles file I/O and inode management." },
  blk: { name: "Block Driver", priority: 250, desc: "virtio-blk driver. Raw sector read/write." },
  net: { name: "Net Server", priority: 210, desc: "TCP/IP stack (lwIP). Socket API." },
  netdrv: { name: "Net Driver", priority: 230, desc: "virtio-net driver. Ethernet TX/RX." },
  serial: { name: "Serial Driver", priority: 254, desc: "UART driver. Console I/O." },
  auth: { name: "Auth Server", priority: 210, desc: "Authentication. UID/GID validation." },
  sandbox: { name: "Sandbox Kernel", priority: 150, desc: "User-space kernel. Process and thread management." }
};

var INITIAL_PROCESSES = [
  { id: 1, name: "shell", x: 40, color: "#6af", targetX: 40, vx: 0 },
  { id: 2, name: "httpd", x: 100, color: "#fa6", targetX: 100, vx: 0 },
  { id: 3, name: "prog", x: 160, color: "#af6", targetX: 160, vx: 0 }
];

var ROBOT_KEYS = ["orch", "fs", "net", "sandbox", "blk", "netdrv", "serial", "auth"];
var WIRE_PAIRS = ["orch-fs", "orch-net", "sandbox-orch", "fs-blk", "net-netdrv", "orch-auth", "orch-serial"];

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

var ROBOT_TYPES = {
  orch: "boss", fs: "nerdy", blk: "worker", net: "tech",
  netdrv: "driver", serial: "retro", auth: "guard", sandbox: "secretservice"
};

var ROBOT_COLORS = {
  orch: "#f90", fs: "#48f", blk: "#a6f", net: "#0c8",
  netdrv: "#cc0", serial: "#f66", auth: "#88f", sandbox: "#6c6"
};

var ROBOT_LABELS = {
  orch: "Boss", fs: "FS", blk: "Blk", net: "Net",
  netdrv: "NetDrv", serial: "Serial", auth: "Auth", sandbox: "SbxKernel"
};

var PROCESS_NAMES = ["daemon", "worker", "logger", "cron", "nginx", "redis"];
var PROCESS_COLORS = ["#f66", "#6f6", "#66f", "#ff6", "#f6f", "#6ff"];
