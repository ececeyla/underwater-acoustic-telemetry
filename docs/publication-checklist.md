# Configuration, privacy and validation

- Copy `SurfaceNode/src/config.example.h` to a local, untracked `config.h` and set the APN, network, modem addresses and board configuration.
- Keep passwords, SIM details, access tokens and real GNSS logs out of version control.
- Check the firmware dependencies for the exact ESP32-S3 board and PlatformIO environment.
- Run the dashboards on a trusted local network unless authentication and transport security are added.

The automated tests cover software logic and sample data. Field testing, battery-life measurements, modem response rates and localization accuracy require hardware validation.

Retain third-party library notices and collaborator credit when reusing the source.
