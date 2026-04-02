retro_web/status/
├── index.html
├── styles.css
├── js/
│   ├── data.js              # Constants, configs
│   ├── utils.js             # Helper functions
│   ├── components/
│   │   ├── led.js           # LED
│   │   ├── robots.js        # All 8 robot types
│   │   ├── pets.js          # Cat, Dog, Mess
│   │   ├── machines.js      # Coffee, Vending, Transporter, Door
│   │   ├── furniture.js     # Desk, Phone, Disk
│   │   ├── layout.js        # Wire, Room
│   │   ├── ui.js            # LogTicker, Tooltip
│   │   ├── cleaner.js       # CleaningRobot
│   │   └── human.js         # Human
│   ├── hooks/
│   │   ├── useRobots.js     # Robot state & movement
│   │   ├── usePets.js       # Pet spawning & behavior
│   │   ├── useCleaner.js    # Cleaner dispatch
│   │   ├── useTransporter.js # Beam in/out
│   │   └── useIPC.js        # Wire activity
│   └── app.js               # Main StatusPage
└── README.md
