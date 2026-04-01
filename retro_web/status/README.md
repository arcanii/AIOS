# AIOS Retro Status UI

A fun, animated status page showing AIOS system components.

## Overview

This is a kid-friendly visualization of the AIOS 0.3.x architecture:

- **Robots** represent kernel services (Orchestrator, FS Server, Net Server, Sandbox Kernel)
- **Little humans** represent user processes (shell, httpd, programs)
- **Telephones with wires** represent IPC channels between Protection Domains
- **Rooms** represent each Protection Domain with its priority level

## Running Locally

Open `index.html` in a browser, or serve it:

    python3 -m http.server 8080
    # then open http://localhost:8080/retro_web/status/

## Features

- Animated robots that occasionally walk around
- User processes that move within the sandbox
- IPC wires that light up when communication happens
- Phones that ring during active calls
- Retro grid background

## Future Plans

- Real-time data from AIOS httpd server
- Click interactions to inspect PD status
- Process spawn/exit animations
- Memory usage visualization
- Log stream ticker
