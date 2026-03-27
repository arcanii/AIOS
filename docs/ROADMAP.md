# AIOS Development Roadmap

## Milestone 1: POSIX Foundation (Current)
- [ ] libc with POSIX wrappers (open, read, write, close, stat, readdir)
- [ ] VFS server with file descriptor table
- [ ] Process server (sandbox lifecycle management)
- [ ] /dev/console for stdin/stdout
- [ ] Shell as a POSIX program
- [ ] Core utilities: ls, cat, echo, cp, rm, mkdir

## Milestone 2: Network Stack
- [ ] virtio-net driver (QEMU)
- [ ] TCP/IP stack (lwIP port)
- [ ] Socket API in libc
- [ ] HTTP client (for AI API access)
- [ ] HTTP server (status UI)

## Milestone 3: Status Web UI
- [ ] /api/status JSON endpoint
- [ ] /api/log real-time stream
- [ ] /api/priority user guidance endpoint
- [ ] HTML/JS dashboard

## Milestone 4: Raspberry Pi Port
- [ ] Microkit RPi4 board support
- [ ] BCM2711 UART driver
- [ ] SD card driver
- [ ] USB ethernet / WiFi

## Milestone 5: Build System Inside AIOS
- [ ] Port TCC as AIOS process
- [ ] libc headers on filesystem
- [ ] cc command, simple make tool

## Milestone 6: Git and Self-Hosted Development
- [ ] Minimal git client over HTTPS
- [ ] AI agent reads/modifies source
- [ ] Automated test suite
- [ ] Push at milestone intervals

## Milestone 7: AI Agent Autonomy
- [ ] AI agent PD with LLM API access
- [ ] Task queue from web UI
- [ ] Generate, compile, test, commit cycle
- [ ] Human review via web UI
