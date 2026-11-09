# Security, privacy and publication checklist

- [x] Original local Wi-Fi credentials and private server endpoints **not committed**.
- [x] Credential-bearing `SurfaceNode/src/config.h` excluded; safe `config.example.h` provided.
- [x] `node_modules`, `.pio`, local `.env` and telemetry logs excluded.
- [ ] Audit copied firmware and frontend scripts for any residual credentials/PII before making public.
- [ ] Confirm that no real GNSS tracks, modem identifiers, SIM identifiers or personal network details appear in screenshots.
- [ ] Confirm attribution and license terms of components supplied by UPC/SARTI or third-party libraries.
- [ ] Verify a clean PlatformIO build with all required dependencies.
- [ ] Verify the two dashboard startup paths and API data schema.
- [ ] Validate modem and acoustic transmission hardware behavior in a reproducible field setup.
- [ ] Document which firmware/dashboard components were personally authored versus team-provided.
- [ ] Consider authentication, rate limiting and CORS tightening before any off-LAN deployment.

**Do not publish the repository until this checklist has been reviewed.**
