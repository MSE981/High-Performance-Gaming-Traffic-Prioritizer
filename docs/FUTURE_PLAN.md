# Future work (Raspberry Pi 5 target)

Items not implemented yet; tracked for later milestones.

## Remote web dashboard (control plane)

- Run **nginx** on **logical CPU 1** as reverse proxy to a **FastCGI** service on a UNIX domain socket.
- FastCGI handlers: **GET** returns JSON built from `Telemetry` atomics (no blocking I/O, no contended locks); **POST** parses JSON and writes `Config` atomics plus any documented RCU swap for QoS tables.
- Browser: low-rate polling (about 1 Hz) for telemetry; POST on user actions.

## Telemetry rescan eventfds

- Optionally change `Telemetry::SystemInfo::init_event_fds()` to return `std::expected<void, std::string>` and thread failure through `App::init()` / `main()` so a broken `eventfd` pair fails startup visibly instead of only printing a warning.
