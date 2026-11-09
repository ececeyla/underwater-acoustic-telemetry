#pragma once
// EXAMPLE configuration only. Copy to config.h and update for your own devices.
// Do not commit config.h or credentials to source control.
namespace AppConfig {
  inline constexpr const char *NODE_ID = "A";
  inline constexpr const char *SIM_PIN = "";
  inline constexpr const char *WIFI_AP_SSID = "AcousticNode-A";
  inline constexpr uint16_t MODEM_ADDRESS = 111;
  inline constexpr const char *APN = "your-operator-apn";
  inline constexpr const char *APN_USER = "";
  inline constexpr const char *APN_PASS = "";
  inline constexpr const char *FORCE_OPERATOR_PLMN = "";
  inline constexpr const char *FORCE_OPERATOR_ACT = "";
  inline constexpr bool SCAN_OPERATORS_AT_BOOT = false;
  inline constexpr bool GSM_ENABLED_DEFAULT = false;
  inline constexpr const char *SERVER_HOST = "192.0.2.10";
  inline constexpr uint16_t SERVER_PORT = 2100;
  inline constexpr uint32_t TELEMETRY_INTERVAL_MS = 60000;
  inline constexpr size_t MAX_PENDING_RECORDS = 12;
  inline constexpr uint16_t ACOUSTIC_TARGET_ADDRESS = 123;
  inline constexpr uint32_t ACOUSTIC_TIMEOUT_MS = 3000;
  inline constexpr float SPEED_OF_SOUND_M_PER_S = 1500.0f;
  inline constexpr const char *WIFI_AP_PASS = "replace-with-a-strong-password";
  inline constexpr uint32_t WIFI_AP_IP = 0x0104A8C0u;
  inline constexpr uint32_t WIFI_AP_GATEWAY = WIFI_AP_IP;
  inline constexpr uint32_t WIFI_AP_SUBNET = 0x00FFFFFFu;
  inline constexpr const char *WIFI_STA_SSID = "your-local-network";
  inline constexpr const char *WIFI_STA_PASS = "replace-with-local-wifi-password";
  inline constexpr bool WIFI_STATION_MODE = false;
  inline constexpr const char *LAN_SERVER_HOST = "192.0.2.10";
  inline constexpr uint16_t LAN_SERVER_PORT = 2100;
  inline constexpr uint32_t GPS_FIX_TIMEOUT_MS = 10000;
  inline constexpr uint32_t MODEM_INIT_TIMEOUT_MS = 10000;
  inline constexpr bool ENABLE_VERBOSE_DEBUG = false;
}
