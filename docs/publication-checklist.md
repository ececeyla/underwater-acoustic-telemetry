# Configuration, privacy and validation

Before running the project on new hardware:

- Use `SurfaceNode/src/config.example.h` to prepare a **local**, untracked `config.h`.
- Review the APN, GNSS commands, modem and sensor addresses, and board pin mapping.
- Do not commit passwords, SIM details, real GNSS logs, personal IP addresses or access tokens.
- Check firmware dependencies for the exact ESP32-S3 board and PlatformIO environment.
- Keep the node dashboards on a trusted local network unless authentication and transport security are added.

The project repository owner has confirmed approval to share the internship materials. Third-party library notices and collaborator credit should be retained in source files.

The available automated tests validate software logic and sample data. Full field testing, battery-life measurement, modem response rates and localization accuracy remain separate engineering checks.
