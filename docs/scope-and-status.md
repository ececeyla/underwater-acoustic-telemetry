# Scope, attribution and status

The repository reorganizes an existing UPC/SARTI 2026 underwater acoustic telemetry research bundle into a navigable portfolio. Some files may be shared/team-provided components. A GitHub import alone cannot establish single-author ownership of every source file.

## What is represented in the imported files

- **Surface firmware:** ESP32-S3 state handling, GNSS, SIM7080G cellular telemetry, Wi-Fi configuration, UART acoustic modem transactions and sleep/wake support.
- **Submerged firmware:** sequential acoustic pings to three node addresses, RTT collection, payload formatting and retry handling.
- **Python backend:** TCP JSON-line telemetry ingestion, node-specific logs, geographic projections, geometric trilateration, position visualization and CSV exports.
- **Node dashboard:** local discovery, status polling, monitoring and remote control endpoints.

## Verified by inspection versus field validation

- The source files contain these features; this was established by code inspection.
- The provided documentation describes a multi-node system and instructions to run it.
- This GitHub import **did not** rerun the system in hardware, establish field accuracy or verify every combination of modem/firmware versions.
- The project was developed in a laboratory internship environment; individual authorship of every module has not been independently established.

## Reproducibility notes

- A fresh board setup must use an appropriately filled local `SurfaceNode/src/config.h`.
- Wi-Fi/password, APN and external server addresses were removed from the GitHub import.
- The two viewers are alternatives and can both default to port 8080, so assign separate ports if running simultaneously.
- Node addressing can vary between code/config snapshots and README examples. Configure a consistent address map.
- Recording real logs risks leaking location, IPs and other identifiers. Use synthetic/example data in future public demonstrations.

## Possible documented extensions

- A synthetic telemetry fixture and repeatable viewer integration tests.
- Exact PlatformIO library/version pinning and build checks.
- Measured current consumption in active/sleep states.
- Acoustic RTT success-rate studies and GNSS positioning evaluation.
