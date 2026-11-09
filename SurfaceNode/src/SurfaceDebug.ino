#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <vector>
#include "utilities.h"
#include "config.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#define TINY_GSM_RX_BUFFER 1024
#define SerialAT Serial1
#define SerialUser Serial2
#define USER_UART_TXD_PIN 43
#define USER_UART_RXD_PIN 44
#define USER_UART_BAUD 9600

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"
XPowersPMU PMU;
WebServer server(80);
unsigned long lastTelemetrySend = 0;

// GNSS and LTE/TCP share RF hardware on the SIM7080G and cannot run concurrently (see the
// board README and SIMCom's Hardware Design Manual). While gnssEnabled is true we only read
// GPS/ping and accumulate records; the batch is flushed with GNSS off every TELEMETRY_INTERVAL_MS.
bool gnssEnabled = true;
std::vector<String> pendingRecords;

// Runtime toggle for the whole cellular feature; changeable via GET/POST /api/gsm?enabled=0|1.
bool gsmFeatureEnabled = AppConfig::GSM_ENABLED_DEFAULT;

// Runtime toggle for acoustic pings from this surface node (NOT modem broadcasts from submerged).
bool acousticPingEnabled = true;
bool sleepRequested = false;
// Runtime WiFi/LAN config, changeable via GET/POST /api/wifi and persisted in NVS so it survives
// a reboot. AppConfig::* values above are only the first-ever-boot defaults.
Preferences prefs;
bool wifiStationMode = AppConfig::WIFI_STATION_MODE;
String wifiStaSsid = AppConfig::WIFI_STA_SSID;
String wifiStaPass = AppConfig::WIFI_STA_PASS;
String lanServerHost = AppConfig::LAN_SERVER_HOST;
uint16_t lanServerPort = AppConfig::LAN_SERVER_PORT;

String consoleLine;
String lastInternalCommand = "(ninguno)";
String lastInternalResponse = "(ninguna)";
String lastExternalCommand = "(ninguno)";
String lastExternalResponse = "(ninguna)";
String lastInternalAsync;
String lastExternalAsync;
String statusCPIN;
String statusCSQ;
String statusCEREG;
String statusCGREG;
unsigned long lastStatusProbe = 0;
unsigned long lastGsmRadioReset = 0;

static String jsonEscape(const String &in);
static String transactAT(Stream &port, const String &cmd, uint32_t timeoutMs, uint32_t quietMs);
static String transactRaw(Stream &port, const String &cmd, uint32_t timeoutMs);
static String transactExternalRaw(const String &cmd, uint32_t timeoutMs);
static String transactInternalAT(const String &cmd, uint32_t timeoutMs, uint32_t quietMs);
static void sendUnicastCommand(bool enable);
static void configureAcousticModemAddress();
static void refreshInternalStatus(bool force = false);
static const char *wifiStatusToString(wl_status_t status);
static void triggerTxFlash();
static void configureWifiRadio();

static void loadRuntimeConfig()
{
    prefs.begin("surfcfg", true);
    gsmFeatureEnabled = prefs.getBool("gsmEn", AppConfig::GSM_ENABLED_DEFAULT);
    wifiStationMode = prefs.getBool("wifiSta", AppConfig::WIFI_STATION_MODE);
    wifiStaSsid = prefs.getString("staSsid", AppConfig::WIFI_STA_SSID);
    wifiStaPass = prefs.getString("staPass", AppConfig::WIFI_STA_PASS);
    lanServerHost = prefs.getString("lanHost", AppConfig::LAN_SERVER_HOST);
    lanServerPort = prefs.getUShort("lanPort", AppConfig::LAN_SERVER_PORT);
    acousticPingEnabled = prefs.getBool("acouPing", true);
    prefs.end();
}

static void saveRuntimeConfig()
{
    prefs.begin("surfcfg", false);
    prefs.putBool("gsmEn", gsmFeatureEnabled);
    prefs.putBool("wifiSta", wifiStationMode);
    prefs.putString("staSsid", wifiStaSsid);
    prefs.putString("staPass", wifiStaPass);
    prefs.putString("lanHost", lanServerHost);
    prefs.putUShort("lanPort", lanServerPort);
    prefs.putBool("acouPing", acousticPingEnabled);
    prefs.end();
}

struct DeviceTelemetry {
    bool gpsFix = false;
    String gpsTimestamp = "unknown";
    double latitude = 0.0;
    double longitude = 0.0;
    bool pingOk = false;
    String pingReply = "none";
    int pingTarget = 0;
    int pingRttMs = -1;
    float pingDistanceM = 0.0f;
    bool acousticModemHealthy = false;
    int acousticModemAddress = -1;
    float acousticModemVoltageV = 0.0f;
    // Submerged node broadcast reception (asincróno desde el nodo sumergido)
    bool submergedBroadcastOk = false;
    int submergedSourceAddress = -1;
    int submergedRttA_ms = -1;
    int submergedRttB_ms = -1;
    int submergedRttC_ms = -1;
    float submergedDistanceA_m = -1.0f;
    float submergedDistanceB_m = -1.0f;
    float submergedDistanceC_m = -1.0f;
    // Remote control: commands sent to submerged node
    String lastRemoteCommand = "none";      // "enable" o "disable"
    int lastRemoteCommandStatus = 0;        // 0=none, 1=sent, 2=acked
    int lastRemoteCommandRtt = -1;          // RTT de confirmación (ms)
    bool gsmRegistered = false;
    bool gsmDataConnected = false;
    String gsmIp = "0.0.0.0";
    int gsmSignalRssi = 99;
    String gsmState = "unknown";
    String state = "gps-no-fix";
};

DeviceTelemetry deviceTelemetry;

static bool parseAcousticReply(const String &raw, int &target, int &rttMs, float &distanceM)
{
    String clean = raw;
    clean.replace("\r", " ");
    clean.replace("\n", " ");
    clean.trim();

    int replyStart = clean.lastIndexOf("#R");
    if (replyStart < 0) {
        return false;
    }

    String reply = clean.substring(replyStart);
    reply.trim();
    int targetEnd = reply.indexOf('T');
    if (!reply.startsWith("#R") || targetEnd <= 0) {
        return false;
    }

    String targetField = reply.substring(2, targetEnd);
    String rttField = reply.substring(targetEnd + 1);
    targetField.trim();
    rttField.trim();
    if (targetField.length() == 0 || rttField.length() == 0) {
        return false;
    }

    target = targetField.toInt();
    rttMs = rttField.toInt();
    distanceM = (float)rttMs * AppConfig::SPEED_OF_SOUND_M_PER_S * 3.125e-5f;
    return true;
}

static bool hasValidAcousticReply(const String &raw)
{
    int target = 0;
    int rttMs = -1;
    float distanceM = 0.0f;
    return parseAcousticReply(raw, target, rttMs, distanceM);
}

/**
 * Parsea una respuesta de identidad acústica del formato #AxxxVyyyyy
 * @param raw String con respuesta cruda
 * @param address Referencia donde se guardará la dirección del modem
 * @param voltageRaw Referencia donde se guardará el valor bruto de voltaje
 * @param voltageV Referencia donde se guardará el voltaje real en Voltios
 * @return true si el parseo fue exitoso, false si falló
 */
static bool parseAcousticIdentity(const String &raw, int &address, int &voltageRaw, float &voltageV)
{
    String clean = raw;
    clean.replace("\r", " ");
    clean.replace("\n", " ");
    clean.trim();

    int replyStart = clean.lastIndexOf("#A");
    if (replyStart < 0) {
        return false;
    }

    String reply = clean.substring(replyStart);
    reply.trim();
    int addressEnd = reply.indexOf('V');
    if (!reply.startsWith("#A") || addressEnd <= 0) {
        return false;
    }

    String addressField = reply.substring(2, addressEnd);
    addressField.trim();
    
    if (addressField.length() == 0) {
        return false;
    }

    address = addressField.toInt();
    
    // Extraer voltageRaw: solo los dígitos después de V
    // El modem puede devolver: #A102V22335R001.006... así que necesitamos ser explícitos
    String voltageFieldRaw = reply.substring(addressEnd + 1);
    int voltageEnd = 0;
    while (voltageEnd < voltageFieldRaw.length() && isdigit(voltageFieldRaw[voltageEnd])) {
        voltageEnd++;
    }
    
    if (voltageEnd == 0) {
        return false;
    }
    
    String voltageField = voltageFieldRaw.substring(0, voltageEnd);
    voltageRaw = voltageField.toInt();
    voltageV = (float)voltageRaw * 15.0f / 65536.0f;
    return true;
}

/**
 * Parsea un broadcast del nodo sumergido del formato #BsssllldAxxxx,Byyyy,Czzzz
 * Donde: sss=source address, lll=payload length, dAxxxx,Byyyy,Czzzz=payload con tiempos RTT
 */
static bool parseSubmergedBroadcast(const String &raw, int &sourceAddr, int &rttA, int &rttB, int &rttC)
{
    String clean = raw;
    clean.replace("\r", " ");
    clean.replace("\n", " ");
    clean.trim();

    int bcastStart = clean.lastIndexOf("#B");
    if (bcastStart < 0) {
        return false;
    }

    String bcast = clean.substring(bcastStart);
    bcast.trim();

    if (!bcast.startsWith("#B") || bcast.length() < 8) {
        return false;
    }

    // Extraer source address (3 dígitos después de #B)
    String sourceStr = bcast.substring(2, 5);
    sourceStr.trim();
    sourceAddr = sourceStr.toInt();

    // Extraer payload (después de #Bsssllld)
    int payloadStart = 7;
    if (payloadStart >= bcast.length()) {
        return false;
    }

    String payload = bcast.substring(payloadStart);
    payload.trim();

    // Parsear payload: Axxxx,Byyyy,Czzzz
    int aIdx = payload.indexOf('A');
    int bIdx = payload.indexOf('B', aIdx + 1);
    int cIdx = payload.indexOf('C', bIdx + 1);

    if (aIdx < 0 || bIdx < 0 || cIdx < 0) {
        return false;
    }

    String aVal = payload.substring(aIdx + 1, bIdx);
    String bVal = payload.substring(bIdx + 1, cIdx);
    String cVal = payload.substring(cIdx + 1);

    aVal.trim();
    bVal.trim();
    cVal.trim();

    rttA = aVal.toInt();
    rttB = bVal.toInt();
    rttC = cVal.toInt();

    return true;
}

static void processSubmergedBroadcasts()
{
    if (lastExternalAsync.length() == 0) {
        return;
    }

    int bcastIdx = lastExternalAsync.indexOf("#B");
    if (bcastIdx < 0) {
        return;
    }

    int endIdx = lastExternalAsync.indexOf('\n', bcastIdx);
    if (endIdx < 0) {
        endIdx = lastExternalAsync.length();
    }

    String bcastLine = lastExternalAsync.substring(bcastIdx, endIdx);

    int sourceAddr = -1;
    int rttA = -1, rttB = -1, rttC = -1;

    if (parseSubmergedBroadcast(bcastLine, sourceAddr, rttA, rttB, rttC)) {
        deviceTelemetry.submergedBroadcastOk = true;
        deviceTelemetry.submergedSourceAddress = sourceAddr;
        deviceTelemetry.submergedRttA_ms = rttA;
        deviceTelemetry.submergedRttB_ms = rttB;
        deviceTelemetry.submergedRttC_ms = rttC;

        if (rttA >= 0) {
            deviceTelemetry.submergedDistanceA_m = (float)rttA * AppConfig::SPEED_OF_SOUND_M_PER_S * 3.125e-5f;
        } else {
            deviceTelemetry.submergedDistanceA_m = -1.0f;
        }
        if (rttB >= 0) {
            deviceTelemetry.submergedDistanceB_m = (float)rttB * AppConfig::SPEED_OF_SOUND_M_PER_S * 3.125e-5f;
        } else {
            deviceTelemetry.submergedDistanceB_m = -1.0f;
        }
        if (rttC >= 0) {
            deviceTelemetry.submergedDistanceC_m = (float)rttC * AppConfig::SPEED_OF_SOUND_M_PER_S * 3.125e-5f;
        } else {
            deviceTelemetry.submergedDistanceC_m = -1.0f;
        }

        if (AppConfig::ENABLE_VERBOSE_DEBUG) {
            Serial.print("[SUBMERGED BROADCAST] Source=");
            Serial.print(sourceAddr);
            Serial.print(" RTT_A=");
            Serial.print(rttA);
            Serial.print("ms RTT_B=");
            Serial.print(rttB);
            Serial.print("ms RTT_C=");
            Serial.print(rttC);
            Serial.println("ms");
        }

        lastExternalAsync.remove(bcastIdx, endIdx - bcastIdx + 1);
    }
}

/**
 * Envía un comando unicast al nodo sumergido para habilitar/deshabilitar pings.
 * Formato: $Mxxxnndata
 *   xxx = target address (102 en este caso)
 *   nn = 02 (2 bytes de datos)
 *   data = EP (enable ping) o DP (disable ping)
 * 
 * Respuesta esperada: #RxxxTyyyyy (donde yyyyy es el RTT)
 */
static void sendUnicastCommand(bool enable)
{
    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.println("\n=== [UNICAST CONTROL] ===");
        Serial.print("[UNICAST] Enviando comando al nodo sumergido: ");
        Serial.println(enable ? "ENABLE PING" : "DISABLE PING");
    }

    // Formatar comando: $Mxxxnndata
    // xxx = ACOUSTIC_TARGET_ADDRESS (102)
    // nn = 02 (longitud de datos)
    // data = EP o DP
    String data = enable ? "EP" : "DP";
    String cmd = "$M" + String(AppConfig::ACOUSTIC_TARGET_ADDRESS) + "02" + data;

    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.print("[UNICAST CMD] ");
        Serial.println(cmd);
    }

    // Enviar comando
    String response = transactRaw(SerialUser, cmd, 5000);

    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.print("[UNICAST RAW RX] ");
        Serial.println(response);
    }

    // Actualizar estado local
    deviceTelemetry.lastRemoteCommand = enable ? "enable" : "disable";
    deviceTelemetry.lastRemoteCommandStatus = 1;  // sent
    deviceTelemetry.lastRemoteCommandRtt = -1;

    // Parsear confirmación: #RxxxTyyyyy
    int rIdx = response.indexOf("#R");
    if (rIdx >= 0) {
        String confirmResp = response.substring(rIdx);
        int tIdx = confirmResp.indexOf('T');
        
        if (tIdx > 0) {
            String rttStr = confirmResp.substring(tIdx + 1);
            int rtt = rttStr.toInt();
            
            if (rtt > 0) {
                deviceTelemetry.lastRemoteCommandStatus = 2;  // acked
                deviceTelemetry.lastRemoteCommandRtt = rtt;
                
                if (AppConfig::ENABLE_VERBOSE_DEBUG) {
                    Serial.print("[UNICAST ACK] Comando entregado. RTT=");
                    Serial.print(rtt);
                    Serial.println("ms");
                }
            }
        }
    } else {
        if (AppConfig::ENABLE_VERBOSE_DEBUG) {
            Serial.println("[UNICAST] Sin confirmación de entrega");
        }
    }

    applyLedState();
}

// Extracts the RSSI value from an "+CSQ: <rssi>,<ber>" reply; 99 means unknown/no signal.
static int parseCsqRssi(const String &raw)
{
    int pos = raw.indexOf("+CSQ:");
    if (pos < 0) {
        return 99;
    }
    int valueStart = pos + 5;
    int comma = raw.indexOf(',', valueStart);
    if (comma < 0) {
        return 99;
    }
    String rssiField = raw.substring(valueStart, comma);
    rssiField.trim();
    if (rssiField.length() == 0) {
        return 99;
    }
    return rssiField.toInt();
}

// Extracts the IP from an "+CNACT: <pdpidx>,<status>,\"<ip>\"" reply.
static String parseCnactIp(const String &raw)
{
    int pos = raw.indexOf("+CNACT: 0,");
    if (pos < 0) {
        return "0.0.0.0";
    }
    int quoteStart = raw.indexOf('"', pos);
    if (quoteStart < 0) {
        return "0.0.0.0";
    }
    int quoteEnd = raw.indexOf('"', quoteStart + 1);
    if (quoteEnd < 0) {
        return "0.0.0.0";
    }
    String ip = raw.substring(quoteStart + 1, quoteEnd);
    return ip.length() ? ip : "0.0.0.0";
}

static bool isRegistrationStableForTelemetry()
{
    bool registered = (statusCEREG.indexOf(",1") >= 0) || (statusCEREG.indexOf(",5") >= 0) ||
                      (statusCGREG.indexOf(",1") >= 0) || (statusCGREG.indexOf(",5") >= 0);
    bool signalUsable = (deviceTelemetry.gsmSignalRssi != 99 && deviceTelemetry.gsmSignalRssi > 0 &&
                         deviceTelemetry.gsmSignalRssi < 31);
    bool notSearching = (statusCEREG.indexOf(",2") < 0) && (statusCGREG.indexOf(",2") < 0) &&
                        (statusCEREG.indexOf(",0") < 0) && (statusCGREG.indexOf(",0") < 0);
    return registered && signalUsable && notSearching;
}

static bool connectGprsDirect()
{
    // With weak/flickering coverage, registration can blip without the PDP context
    // actually being torn down; adopt an already-active PDP instead of re-running it.
    String existing = parseCnactIp(transactAT(SerialAT, "AT+CNACT?", 2000, 150));
    if (existing != "0.0.0.0") {
        deviceTelemetry.gsmIp = existing;
        return true;
    }

    transactAT(SerialAT, "AT+CGDCONT=1,\"IP\",\"" + String(AppConfig::APN) + "\"", 2000, 150);
    if (transactAT(SerialAT, "AT+CGATT=1", 15000, 300).indexOf("OK") < 0) {
        return false;
    }
    transactAT(SerialAT, "AT+CGNAPN", 2000, 150);
    // ip_type=0 (IPv4, coincide con el contexto "IP" de CGDCONT); authtype=0 (sin auth) salvo
    // que haya usuario/password configurados, si no la negociacion PAP/CHAP con credenciales
    // vacias puede hacer que la red rechace el PDP context.
    bool hasApnAuth = String(AppConfig::APN_USER).length() > 0 || String(AppConfig::APN_PASS).length() > 0;
    String cncfg = "AT+CNCFG=0,0,\"" + String(AppConfig::APN) + "\",\"" +
                   String(AppConfig::APN_USER) + "\",\"" + String(AppConfig::APN_PASS) + "\"," +
                   (hasApnAuth ? "3" : "0");
    transactAT(SerialAT, cncfg, 2000, 150);

    // AT+CNACT=0,1 answers OK almost instantly, but the real "+APP PDP: 0,ACTIVE"
    // confirmation can arrive a couple of seconds later.
    bool active = false;
    for (int attempt = 0; attempt < 5 && !active; attempt++) {
        String cnactResp = transactAT(SerialAT, "AT+CNACT=0,1", 15000, 1000);
        active = cnactResp.indexOf("0,ACTIVE") >= 0;
        if (!active) {
            delay(500);
        }
    }
    if (!active) {
        deviceTelemetry.gsmIp = "0.0.0.0";
        return false;
    }

    deviceTelemetry.gsmIp = parseCnactIp(transactAT(SerialAT, "AT+CNACT?", 2000, 150));
    return deviceTelemetry.gsmIp != "0.0.0.0";
}

static void applyLedState()
{
    if (!deviceTelemetry.gpsFix) {
        deviceTelemetry.state = "gps-no-fix";
        return;
    }

    if (deviceTelemetry.pingOk) {
        deviceTelemetry.state = "ping-ok";
        return;
    }

    deviceTelemetry.state = "gps-fix";
}

// Single PMU-controlled LED, so status is encoded as a repeating count of short pulses,
// separated by a pause, so it can be read like a blink code:
//   1 pulse  = no GPS fix
//   2 pulses = GPS fix, no acoustic ping reply
//   3 pulses = GPS fix + acoustic ping OK
// Sending a JSON record (WiFi or cellular) overrides the cycle with 2 quick flashes so the
// user can see data actually leaving the device, independent of the status pulse count.
enum LedPhase { LED_PHASE_PULSE_ON, LED_PHASE_PULSE_OFF, LED_PHASE_PAUSE };

static LedPhase ledPhase = LED_PHASE_PAUSE;
static uint32_t ledPhaseStartedAt = 0;
static uint8_t ledPulsesDone = 0;
static volatile bool ledTxFlashPending = false;

static const uint32_t LED_PULSE_ON_MS = 150;
static const uint32_t LED_PULSE_OFF_MS = 150;
static const uint32_t LED_PAUSE_MS = 900;
static const uint32_t LED_TXFLASH_STEP_MS = 60;

// Call right after a JSON record is actually sent out, over WiFi or cellular.
static void triggerTxFlash()
{
    ledTxFlashPending = true;
}

static uint8_t ledStatusPulseCount()
{
    if (!deviceTelemetry.gpsFix) {
        return 1;
    }
    if (!deviceTelemetry.pingOk) {
        return 2;
    }
    return 3;
}

// Non-blocking status pulses; must be called every loop() iteration (no delay() in between).
static void updateStatusLed()
{
    uint32_t now = millis();

    if (ledTxFlashPending) {
        ledTxFlashPending = false;
        // Two quick flashes, visually distinct from the slower status pulses above.
        PMU.setChargingLedMode(XPOWERS_CHG_LED_ON);
        delay(LED_TXFLASH_STEP_MS);
        PMU.setChargingLedMode(XPOWERS_CHG_LED_OFF);
        delay(LED_TXFLASH_STEP_MS);
        PMU.setChargingLedMode(XPOWERS_CHG_LED_ON);
        delay(LED_TXFLASH_STEP_MS);
        PMU.setChargingLedMode(XPOWERS_CHG_LED_OFF);
        ledPhase = LED_PHASE_PAUSE;
        ledPhaseStartedAt = now;
        ledPulsesDone = 0;
        return;
    }

    switch (ledPhase) {
    case LED_PHASE_PAUSE:
        if (now - ledPhaseStartedAt >= LED_PAUSE_MS) {
            ledPulsesDone = 0;
            ledPhase = LED_PHASE_PULSE_ON;
            ledPhaseStartedAt = now;
            PMU.setChargingLedMode(XPOWERS_CHG_LED_ON);
        }
        break;
    case LED_PHASE_PULSE_ON:
        if (now - ledPhaseStartedAt >= LED_PULSE_ON_MS) {
            PMU.setChargingLedMode(XPOWERS_CHG_LED_OFF);
            ledPhase = LED_PHASE_PULSE_OFF;
            ledPhaseStartedAt = now;
        }
        break;
    case LED_PHASE_PULSE_OFF:
        if (now - ledPhaseStartedAt >= LED_PULSE_OFF_MS) {
            ledPulsesDone++;
            if (ledPulsesDone >= ledStatusPulseCount()) {
                ledPhase = LED_PHASE_PAUSE;
                ledPhaseStartedAt = now;
            } else {
                PMU.setChargingLedMode(XPOWERS_CHG_LED_ON);
                ledPhase = LED_PHASE_PULSE_ON;
                ledPhaseStartedAt = now;
            }
        }
        break;
    }
}

static String buildStateJson()
{
    IPAddress ip = wifiStationMode ? WiFi.localIP() : WiFi.softAPIP();
    String body = "{";
    body += "\"ok\":true,";
    body += "\"nodeId\":\"" + String(AppConfig::NODE_ID) + "\",";
    // Features flags FIRST to ensure they appear in response
    body += "\"gsmFeatureEnabled\":" + String(gsmFeatureEnabled ? "true" : "false") + ",";
    body += "\"acousticPingEnabled\":" + String(acousticPingEnabled ? "true" : "false") + ",";
    // Telemetry data
    body += "\"gpsFix\":" + String(deviceTelemetry.gpsFix ? "true" : "false") + ",";
    body += "\"gpsTimestamp\":\"" + jsonEscape(deviceTelemetry.gpsTimestamp) + "\",";
    body += "\"latitude\":" + String(deviceTelemetry.latitude, 6) + ",";
    body += "\"longitude\":" + String(deviceTelemetry.longitude, 6) + ",";
    body += "\"pingOk\":" + String(deviceTelemetry.pingOk ? "true" : "false") + ",";
    body += "\"pingRttMs\":" + String(deviceTelemetry.pingRttMs) + ",";
    body += "\"acousticModemHealthy\":" + String(deviceTelemetry.acousticModemHealthy ? "true" : "false") + ",";
    body += "\"acousticModemAddress\":" + String(deviceTelemetry.acousticModemAddress) + ",";
    body += "\"acousticModemVoltageV\":" + String(deviceTelemetry.acousticModemVoltageV, 2) + ",";
    body += "\"submergedBroadcastOk\":" + String(deviceTelemetry.submergedBroadcastOk ? "true" : "false") + ",";
    body += "\"submergedSourceAddress\":" + String(deviceTelemetry.submergedSourceAddress) + ",";
    body += "\"submergedRttA_ms\":" + String(deviceTelemetry.submergedRttA_ms) + ",";
    body += "\"submergedRttB_ms\":" + String(deviceTelemetry.submergedRttB_ms) + ",";
    body += "\"submergedRttC_ms\":" + String(deviceTelemetry.submergedRttC_ms) + ",";
    body += "\"submergedDistanceA_m\":" + String(deviceTelemetry.submergedDistanceA_m, 2) + ",";
    body += "\"submergedDistanceB_m\":" + String(deviceTelemetry.submergedDistanceB_m, 2) + ",";
    body += "\"submergedDistanceC_m\":" + String(deviceTelemetry.submergedDistanceC_m, 2) + ",";
    body += "\"lastRemoteCommand\":\"" + String(deviceTelemetry.lastRemoteCommand) + "\",";
    body += "\"lastRemoteCommandStatus\":" + String(deviceTelemetry.lastRemoteCommandStatus) + ",";
    body += "\"lastRemoteCommandRtt\":" + String(deviceTelemetry.lastRemoteCommandRtt) + ",";
    body += "\"gsmRegistered\":" + String(deviceTelemetry.gsmRegistered ? "true" : "false") + ",";
    body += "\"gsmDataConnected\":" + String(deviceTelemetry.gsmDataConnected ? "true" : "false") + ",";
    body += "\"gsmSignalRssi\":" + String(deviceTelemetry.gsmSignalRssi) + ",";
    body += "\"gsmIp\":\"" + jsonEscape(deviceTelemetry.gsmIp) + "\",";
    body += "\"wifiApIp\":\"" + jsonEscape(ip.toString()) + "\",";
    body += "\"wifiMode\":\"" + String(wifiStationMode ? "station" : "ap") + "\",";
    body += "\"lastInternalCommand\":\"" + jsonEscape(lastInternalCommand) + "\",";
    body += "\"lastInternalResponse\":\"" + jsonEscape(lastInternalResponse) + "\",";
    body += "\"lastExternalCommand\":\"" + jsonEscape(lastExternalCommand) + "\",";
    body += "\"lastExternalResponse\":\"" + jsonEscape(lastExternalResponse) + "\"";
    body += "}";
    return body;
}

static bool isGsmRegisteredNow()
{
    String cereg = transactAT(SerialAT, "AT+CEREG?", 1200, 120);
    String cgreg = transactAT(SerialAT, "AT+CGREG?", 1200, 120);
    return (cereg.indexOf(",1") >= 0) || (cereg.indexOf(",5") >= 0) ||
           (cgreg.indexOf(",1") >= 0) || (cgreg.indexOf(",5") >= 0);
}

// Manual PLMN selection can take much longer than automatic (network scan across all bands),
// but must still be capped: if the SIM has no roaming agreement with that PLMN, the modem can
// search indefinitely without ever returning OK or ERROR, hanging boot forever.
static void selectOperator()
{
    String plmn = AppConfig::FORCE_OPERATOR_PLMN;
    if (plmn.length() > 0) {
        String act = AppConfig::FORCE_OPERATOR_ACT;
        String cmd = "AT+COPS=1,2,\"" + plmn + "\"" + (act.length() > 0 ? "," + act : "");
        String resp = transactAT(SerialAT, cmd, 45000, 300);
        if (resp.indexOf("OK") < 0) {
            // Forced PLMN not reachable: fall back to automatic instead of leaving the modem
            // stuck mid-search.
            transactAT(SerialAT, "AT+COPS=0", 5000, 200);
        }
    } else {
        transactAT(SerialAT, "AT+COPS=0", 5000, 200);
    }
}

static bool ensureSimUnlocked()
{
    String cpin = transactAT(SerialAT, "AT+CPIN?", 1200, 120);
    if (cpin.indexOf("CPIN: READY") >= 0) {
        return true;
    }
    if (String(AppConfig::SIM_PIN).length() == 0) {
        // No PIN configured (e.g. Hologram doesn't use one): nothing to send, sending
        // AT+CPIN="" would just waste a failed attempt against the SIM.
        return false;
    }
    transactAT(SerialAT, "AT+CPIN=\"" + String(AppConfig::SIM_PIN) + "\"", 3000, 200);
    cpin = transactAT(SerialAT, "AT+CPIN?", 1200, 120);
    return cpin.indexOf("CPIN: READY") >= 0;
}

// AT+CFUN=0 re-locks the SIM (URC "+CPIN: NOT READY"); after CFUN=1 the SIM needs a moment to
// come back to "+CPIN: READY" before the network registration attempt can actually succeed.
static bool waitSimReady(uint32_t timeoutMs)
{
    uint32_t deadline = millis() + timeoutMs;
    while (millis() < deadline) {
        String cpin = transactAT(SerialAT, "AT+CPIN?", 1200, 120);
        if (cpin.indexOf("CPIN: READY") >= 0) {
            return true;
        }
        delay(500);
    }
    return false;
}

// Opens mux 0 as a plain TCP socket (no SSL). Returns true only on "+CAOPEN: 0,0" (success).
static bool openTcpSocketRaw()
{
    transactAT(SerialAT, "AT+CACID=0", 5000, 150);
    String cmd = "AT+CAOPEN=0,0,\"TCP\",\"" + String(AppConfig::SERVER_HOST) + "\"," + String(AppConfig::SERVER_PORT);
    String resp = transactAT(SerialAT, cmd, 20000, 300);
    return resp.indexOf("+CAOPEN: 0,0") >= 0;
}

// AT+CASEND waits for a ">" prompt before it will accept raw (non-AT) payload bytes.
static bool sendTcpPayloadRaw(const String &payload)
{
    while (SerialAT.available()) {
        SerialAT.read();
    }
    SerialAT.print("AT+CASEND=0," + String(payload.length()));
    SerialAT.print("\r\n");

    uint32_t start = millis();
    String resp;
    while (millis() - start < 5000) {
        while (SerialAT.available()) {
            char c = static_cast<char>(SerialAT.read());
            resp += c;
            if (AppConfig::ENABLE_VERBOSE_DEBUG) Serial.write(c);
        }
        if (resp.indexOf('>') >= 0) {
            break;
        }
        delay(2);
    }
    if (resp.indexOf('>') < 0) {
        return false;
    }

    SerialAT.print(payload);

    start = millis();
    resp = "";
    while (millis() - start < 5000) {
        while (SerialAT.available()) {
            char c = static_cast<char>(SerialAT.read());
            resp += c;
            if (AppConfig::ENABLE_VERBOSE_DEBUG) Serial.write(c);
        }
        if (resp.indexOf("OK") >= 0) {
            break;
        }
        delay(2);
    }
    return resp.indexOf("OK") >= 0;
}

// With CASEND recv_mode 0, received bytes only surface via an explicit AT+CARECV
// after the "+CADATAIND" URC; the URC itself carries no payload.
static String readTcpAckRaw(uint32_t timeoutMs)
{
    uint32_t deadline = millis() + timeoutMs;
    String urc;
    while (millis() < deadline) {
        while (SerialAT.available()) {
            char c = static_cast<char>(SerialAT.read());
            urc += c;
            if (AppConfig::ENABLE_VERBOSE_DEBUG) Serial.write(c);
        }
        if (urc.indexOf("+CADATAIND") >= 0) {
            break;
        }
        delay(20);
    }
    if (urc.indexOf("+CADATAIND") < 0) {
        return "";
    }
    return transactAT(SerialAT, "AT+CARECV=0,64", 2000, 150);
}

static void closeTcpSocketRaw()
{
    transactAT(SerialAT, "AT+CACLOSE=0", 5000, 200);
}

static void recoverTelemetryPdp()
{
    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.println("[TELEMETRY] Resetting PDP after repeated TCP failures");
    }
    closeTcpSocketRaw();
    transactAT(SerialAT, "AT+CNACT=0,0", 10000, 300);
    delay(1000);

    deviceTelemetry.gsmDataConnected = false;
    deviceTelemetry.gsmIp = "0.0.0.0";

    if (connectGprsDirect()) {
        deviceTelemetry.gsmDataConnected = true;
        if (AppConfig::ENABLE_VERBOSE_DEBUG) {
            Serial.print("[TELEMETRY] PDP recovered, IP=");
            Serial.println(deviceTelemetry.gsmIp);
        }
    } else {
        // Fully blocked tunnel: force a full radio reset (AT+CFUN=0/1) on the next cycle.
        lastGsmRadioReset = millis() - 120001UL;
    }
}

// Flushes the accumulated GPS/ping records over TCP. GNSS is powered off first and back on
// afterwards, since the SIM7080G cannot run GNSS and LTE/TCP at the same time (shared RF).
// Runs the periodic GNSS-off/cellular-registration window. Always executed while GSM is
// enabled (even with no queued records) so the modem gets a chance to register/attach -
// otherwise, in WiFi-station/LAN mode, pendingRecords never fills up and GNSS would hog the
// shared RF forever, permanently blocking cellular registration.
static void sendPendingRecordsBatch()
{
    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.print("[BATCH] Disabling GNSS to free the shared RF path, ");
        Serial.print(pendingRecords.size());
        Serial.println(" record(s) queued");
    }

    transactAT(SerialAT, "AT+CGNSPWR=0", 2000, 150);
    gnssEnabled = false;
    delay(300);

    refreshInternalStatus(true);

    if (!isRegistrationStableForTelemetry()) {
        if (AppConfig::ENABLE_VERBOSE_DEBUG) {
            Serial.println("[BATCH] Registration not stable yet, keeping records for next window");
        }
    } else {
        if (!deviceTelemetry.gsmDataConnected) {
            deviceTelemetry.gsmDataConnected = connectGprsDirect();
        }

        if (!deviceTelemetry.gsmDataConnected) {
            if (AppConfig::ENABLE_VERBOSE_DEBUG) {
                Serial.println("[BATCH] PDP activation failed, keeping records for next window");
            }
        } else if (!pendingRecords.empty()) {
            closeTcpSocketRaw();
            delay(300);

            if (!openTcpSocketRaw()) {
                if (AppConfig::ENABLE_VERBOSE_DEBUG) {
                    Serial.println("[BATCH] CAOPEN failed, keeping records for next window");
                }
                if (!waitModemResponsive(5000)) {
                    lastGsmRadioReset = millis() - 120001UL;
                    recoverGsmRegistration();
                    deviceTelemetry.gsmDataConnected = false;
                    deviceTelemetry.gsmIp = "0.0.0.0";
                }
            } else {
                size_t sentCount = 0;
                for (const String &record : pendingRecords) {
                    if (!sendTcpPayloadRaw(record)) {
                        break;
                    }
                    sentCount++;
                }
                readTcpAckRaw(2000);
                closeTcpSocketRaw();

                if (AppConfig::ENABLE_VERBOSE_DEBUG) {
                    Serial.print("[BATCH] Sent ");
                    Serial.print(sentCount);
                    Serial.print("/");
                    Serial.print(pendingRecords.size());
                    Serial.println(" record(s)");
                }

                if (sentCount > 0) {
                    pendingRecords.erase(pendingRecords.begin(), pendingRecords.begin() + sentCount);
                    triggerTxFlash();
                }
            }
        }
    }

    transactAT(SerialAT, "AT+CGNSPWR=1", 2000, 150);
    gnssEnabled = true;
}

// Pushes a single record over the ESP32's own WiFi radio to a telemetry server on the LAN
// the device joined (wifiStationMode). Independent of the cellular modem, so no GNSS conflict.
static bool sendRecordToLan(const String &payload)
{
    if (WiFi.status() != WL_CONNECTED) {
        Serial.print("[LAN] Skipped send: WiFi not connected (status=");
        Serial.print(wifiStatusToString(WiFi.status()));
        Serial.println(")");
        return false;
    }

    Serial.print("[LAN] Connecting to ");
    Serial.print(lanServerHost);
    Serial.print(":");
    Serial.print(lanServerPort);
    Serial.print(" from ");
    Serial.print(WiFi.localIP());
    Serial.println(" ...");

    WiFiClient client;
    if (!client.connect(lanServerHost.c_str(), lanServerPort)) {
        Serial.println("[LAN] Connection to telemetry server failed (refused/unreachable/timeout)");
        return false;
    }
    Serial.println("[LAN] Connected, sending record");
    client.print(payload);
    client.flush();
    delay(20);
    client.stop();
    Serial.println("[LAN] Record sent, connection closed");
    triggerTxFlash();
    return true;
}

static void refreshGpsStatus()
{
    String response = transactAT(SerialAT, "AT+CGNSINF", 2500, 150);
    int infoPos = response.indexOf("+CGNSINF:");
    if (infoPos < 0) {
        deviceTelemetry.gpsFix = false;
        deviceTelemetry.gpsTimestamp = "unknown";
        return;
    }

    String payload = response.substring(infoPos + 9);
    payload.trim();

    int firstComma = payload.indexOf(',');
    int secondComma = (firstComma >= 0) ? payload.indexOf(',', firstComma + 1) : -1;
    int thirdComma = (secondComma >= 0) ? payload.indexOf(',', secondComma + 1) : -1;
    int fourthComma = (thirdComma >= 0) ? payload.indexOf(',', thirdComma + 1) : -1;
    int fifthComma = (fourthComma >= 0) ? payload.indexOf(',', fourthComma + 1) : -1;

    if (firstComma < 0 || secondComma < 0 || thirdComma < 0 || fourthComma < 0) {
        deviceTelemetry.gpsFix = false;
        deviceTelemetry.gpsTimestamp = "unknown";
        return;
    }

    String runStatus = payload.substring(0, firstComma);
    String fixStatus = payload.substring(firstComma + 1, secondComma);
    String timestamp = payload.substring(secondComma + 1, thirdComma);
    String latitude = payload.substring(thirdComma + 1, fourthComma);
    String longitude = payload.substring(fourthComma + 1, (fifthComma >= 0 ? fifthComma : payload.length()));

    deviceTelemetry.gpsFix = (fixStatus == "1" || fixStatus == "2" || fixStatus == "3");
    deviceTelemetry.gpsTimestamp = (timestamp.length() ? timestamp : "unknown");
    deviceTelemetry.latitude = latitude.toDouble();
    deviceTelemetry.longitude = longitude.toDouble();

    if (runStatus == "0") {
        deviceTelemetry.gpsFix = false;
        // GNSS engine got powered off (e.g. after a modem reset or a PDP activation); turn it back on.
        transactAT(SerialAT, "AT+CGNSPWR=1", 1500, 120);
    }
}

static void logLine(const String &tag, const String &msg)
{
    if (!AppConfig::ENABLE_VERBOSE_DEBUG) {
        return;
    }
    Serial.print("[");
    Serial.print(tag);
    Serial.print("] ");
    Serial.println(msg);
}

static void appendCapped(String &dst, char c, size_t maxLen = 4096)
{
    if (dst.length() >= maxLen) {
        size_t drop = dst.length() - maxLen + 1;
        dst.remove(0, drop);
    }
    dst += c;
}

static void debugPrint(const String &text)
{
    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.print(text);
    }
}

static void debugPrintln(const String &text)
{
    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.println(text);
    }
}

static void debugWrite(char c)
{
    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.write(c);
    }
}

static String jsonEscape(const String &in)
{
    String out;
    out.reserve(in.length() + 16);
    for (size_t i = 0; i < in.length(); ++i) {
        char c = in[i];
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

static String transactRaw(Stream &port, const String &cmd, uint32_t timeoutMs = 3000)
{
    while (port.available()) {
        port.read();
    }

    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.println();
        Serial.print("[RAW TX] ");
        Serial.println(cmd);
    }

    port.print(cmd);

    String response;
    response.reserve(256);
    unsigned long start = millis();
    unsigned long lastRx = start;
    unsigned long candidateAt = 0;
    bool candidateSeen = false;

    while (millis() - start < timeoutMs) {
        while (port.available()) {
            char c = static_cast<char>(port.read());
            response += c;
            lastRx = millis();
            debugWrite(c);

            // Detectar respuestas acústicas completas: #R...T..., #A...V..., o #TO
            if ((response.indexOf("#R") >= 0 && response.indexOf('T') > 0) ||
                (response.indexOf("#A") >= 0 && response.indexOf('V') > 0) ||
                (response.indexOf("#TO") >= 0)) {
                if (!candidateSeen) {
                    candidateSeen = true;
                    candidateAt = millis();
                }
            } else {
                candidateSeen = false;
            }
        }

        if (candidateSeen && (millis() - candidateAt) > 250) {
            if (AppConfig::ENABLE_VERBOSE_DEBUG) {
                Serial.println();
                Serial.print("[RAW RX] ");
                Serial.println(response);
            }
            return response;
        }

        if (response.length() > 0 && (millis() - lastRx) > 400) {
            break;
        }

        delay(2);
    }

    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.println();
        Serial.print("[RAW RX] ");
        Serial.println(response);
    }
    return response;
}

// AT+COPS=? can emit an unrelated URC (e.g. "SMS Ready") early, then go quiet for a long
// while as it keeps scanning bands internally before the real operator list arrives - the
// usual quiet-gap detection in transactAT() would return way too soon, leaving the modem still
// busy mid-scan and unresponsive to every command sent right after. This waits for an actual
// terminal OK/ERROR instead.
static String transactUntilTerminal(Stream &port, const String &cmd, uint32_t timeoutMs)
{
    while (port.available()) {
        port.read();
    }

    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.println();
        Serial.print("[AT TX] ");
        Serial.println(cmd);
    }

    port.print(cmd);
    port.print("\r\n");

    String response;
    response.reserve(1024);
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        while (port.available()) {
            char c = static_cast<char>(port.read());
            response += c;
            debugWrite(c);
        }
        String trimmed = response;
        trimmed.trim();
        if (trimmed.endsWith("OK") || trimmed.indexOf("ERROR") >= 0) {
            break;
        }
        delay(20);
    }

    if (response.length() == 0) {
        response = "(no response)";
    }

    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.println();
        Serial.print("[AT RX] ");
        Serial.println(response);
    }
    return response;
}

// Wrapper para transacciones con modem ACÚSTICO (SerialUser/Serial2)
// Guarda automáticamente en lastExternalCommand/lastExternalResponse
static String transactExternalRaw(const String &cmd, uint32_t timeoutMs)
{
    lastExternalCommand = cmd;
    String resp = transactRaw(SerialUser, cmd, timeoutMs);
    lastExternalResponse = resp;
    return resp;
}

// Configura la dirección propia del módem acústico ($A<3 dígitos>); reintenta con ACK
// (#A<3 dígitos>) porque el módem puede tardar unos segundos en estar listo tras BLDO2.
static void configureAcousticModemAddress()
{
    char addr[4];
    snprintf(addr, sizeof(addr), "%03u", AppConfig::MODEM_ADDRESS % 1000);
    String cmd = "$A" + String(addr);
    String ack = "#A" + String(addr);

    uint32_t deadline = millis() + 8000UL;
    while (millis() < deadline) {
        String resp = transactExternalRaw(cmd, AppConfig::ACOUSTIC_TIMEOUT_MS);
        if (resp.indexOf(ack) >= 0) {
            return;
        }
        delay(300);
    }
    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.println("[MODEM] Acoustic address not confirmed after retries");
    }
}

// Wrapper para transacciones con modem INTERNO (SerialAT/Serial1)
// Guarda automáticamente en lastInternalCommand/lastInternalResponse
static String transactInternalAT(const String &cmd, uint32_t timeoutMs, uint32_t quietMs)
{
    lastInternalCommand = cmd;
    String resp = transactAT(SerialAT, cmd, timeoutMs, quietMs);
    lastInternalResponse = resp;
    return resp;
}

static String transactAT(Stream &port, const String &cmd, uint32_t timeoutMs = 3000, uint32_t quietMs = 200)
{
    while (port.available()) {
        port.read();
    }

    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.println();
        Serial.print("[AT TX] ");
        Serial.println(cmd);
    }

    port.print(cmd);
    port.print("\r\n");

    String response;
    response.reserve(512);
    unsigned long start = millis();
    unsigned long lastRx = start;
    bool gotAny = false;

    while (millis() - start < timeoutMs) {
        while (port.available()) {
            char c = static_cast<char>(port.read());
            response += c;
            gotAny = true;
            lastRx = millis();
            debugWrite(c);
        }

        if (gotAny && (millis() - lastRx) > quietMs) {
            break;
        }
        delay(2);
    }

    if (response.length() == 0) {
        response = "(no response)";
    }

    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.println();
        Serial.print("[AT RX] ");
        Serial.println(response);
    }
    return response;
}

static void logModemInitSequence()
{
    if (!AppConfig::ENABLE_VERBOSE_DEBUG) {
        return;
    }

    Serial.println("\n=== [MODEM INIT DEBUG] ===");

    const String atCommands[] = {
        "AT",
        "ATE0",
        "AT+CPIN?",
        "AT+CCID",
        "AT+CSQ",
        "AT+CFUN?",
        "AT+CEREG?",
        "AT+CGREG?",
        "AT+CMEE=2",
        "AT+COPS?",
        "AT+CGNSPWR=1",
        "AT+CGNSINF"
    };

    for (const auto &cmd : atCommands) {
        String resp = transactAT(SerialAT, cmd, 2500, 150);
        if (AppConfig::ENABLE_VERBOSE_DEBUG) {
            Serial.print("[INIT STEP] ");
            Serial.print(cmd);
            Serial.print(" => ");
            Serial.println(resp);
        }
    }
}

static void sendAcousticPingDebug()
{
    // Si los pings acústicos están deshabilitados, solo procesamos broadcasts asincronos
    if (!acousticPingEnabled) {
        processSubmergedBroadcasts();
        return;
    }

    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.println("\n=== [ACOUSTIC PING DEBUG] ===");
    }

    // PASO 1: Health check del modem con $?
    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.println("[HEALTH CHECK] Verificando modem con $?...");
    }
    
    String healthCmd = "$?";
    String healthResp = transactRaw(SerialUser, healthCmd, 1000);
    
    int modemAddr = -1;
    int modemVoltageRaw = -1;
    float modemVoltageV = 0.0f;
    bool modemHealthy = parseAcousticIdentity(healthResp, modemAddr, modemVoltageRaw, modemVoltageV);
    
    deviceTelemetry.acousticModemHealthy = modemHealthy;
    deviceTelemetry.acousticModemAddress = modemAddr;
    deviceTelemetry.acousticModemVoltageV = modemVoltageV;
    
    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        if (modemHealthy) {
            Serial.print("[HEALTH CHECK] ✓ Modem OK - Address: ");
            Serial.print(modemAddr);
            Serial.print(", Voltage: ");
            Serial.print(modemVoltageV, 2);
            Serial.println("V");
        } else {
            Serial.println("[HEALTH CHECK] ✗ Modem NO RESPONDE");
        }
    }
    
    // PASO 2: Si modem no está listo, no hacer ping
    if (!modemHealthy) {
        if (AppConfig::ENABLE_VERBOSE_DEBUG) {
            Serial.println("[PING SKIPPED] Modem not healthy, skipping ping");
        }
        deviceTelemetry.pingOk = false;
        deviceTelemetry.pingReply = "modem_not_ready";
        applyLedState();
        return;
    }

    // PASO 3: Hacer el ping
    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.println("[PING] Enviando ping acústico...");
    }

    String cmd = "$P" + String(AppConfig::ACOUSTIC_TARGET_ADDRESS);
    String resp = transactRaw(SerialUser, cmd, AppConfig::ACOUSTIC_TIMEOUT_MS);

    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.print("[PING RAW] ");
        Serial.println(resp);
    }

    int target = 0;
    int rttMs = -1;
    float distanceM = 0.0f;
    bool valid = parseAcousticReply(resp, target, rttMs, distanceM);

    // PASO 4: Detectar timeout del ping (#TO)
    if (resp.indexOf("#TO") >= 0) {
        if (AppConfig::ENABLE_VERBOSE_DEBUG) {
            Serial.println("[PING] Timeout - target no respondió");
        }
        valid = false;
        resp = "#TO";
    }

    deviceTelemetry.pingReply = resp;
    deviceTelemetry.pingTarget = target;
    deviceTelemetry.pingRttMs = rttMs;
    deviceTelemetry.pingDistanceM = distanceM;
    deviceTelemetry.pingOk = valid;

    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        if (valid) {
            Serial.print("[PING PARSED] target=");
            Serial.print(target);
            Serial.print(" rtt_ms=");
            Serial.print(rttMs);
            Serial.print(" distance_m=");
            Serial.println(distanceM);
        } else {
            Serial.println("[PING PARSED] no valid acoustic reply");
        }
    }

    // Procesar broadcasts asincronos del nodo sumergido
    processSubmergedBroadcasts();

    applyLedState();
}

// Polls plain "AT" until the modem answers OK again, instead of assuming it died.
static bool waitModemResponsive(uint32_t timeoutMs)
{
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        String resp = transactAT(SerialAT, "AT", 1000, 100);
        if (resp.indexOf("OK") >= 0) {
            return true;
        }
        delay(300);
    }
    return false;
}

static bool recoverGsmRegistration()
{
    if ((millis() - lastGsmRadioReset) < 120000UL) {
        return false;
    }

    // Set before the reset attempt so a slow/failed reboot still enforces the 2-minute cooldown.
    lastGsmRadioReset = millis();

    // PWRKEY toggles an already-running SIM7080 OFF instead of resetting it (it replies
    // "NORMAL POWER DOWN"), so only power-cycle if the modem is truly unresponsive.
    String aliveCheck = transactAT(SerialAT, "AT", 1000, 150);
    if (aliveCheck.indexOf("OK") >= 0) {
        if (AppConfig::ENABLE_VERBOSE_DEBUG) {
            Serial.println("[MODEM] Registration not stable, forcing operator rescan...");
        }
        transactAT(SerialAT, "AT+CFUN=0", 5000, 200);
        delay(2000); // Let the radio actually power down before requesting CFUN=1, or the modem stops responding
        // CNMP/CMNB only take effect on the next radio (re)start, so they must be set while CFUN=0
        transactAT(SerialAT, "AT+CNMP=38", 2000, 150);
        // transactAT(SerialAT, "AT+CMNB=3", 2000, 150); // revertido: no enlazaba, config previa (solo CNMP=38) funcionaba con Movistar
        transactAT(SerialAT, "AT+CFUN=1", 10000, 500);
        delay(2000);
        waitSimReady(8000);
        selectOperator();
        transactAT(SerialAT, "AT+CEREG=2", 2000, 150);
        transactAT(SerialAT, "AT+CGREG=2", 2000, 150);
        return isGsmRegisteredNow();
    }

    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.println("[MODEM] Modem unresponsive, power-cycling...");
    }

    auto pulseModemPowerKey = []() {
        digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
        delay(100);
        digitalWrite(BOARD_MODEM_PWR_PIN, HIGH);
        delay(1200);
        digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
    };

    // PWRKEY is a toggle on this board: the first pulse powers down a stuck
    // modem, the second pulse powers it back up.
    pulseModemPowerKey();
    delay(3000);
    if (waitModemResponsive(3000)) {
        return isGsmRegisteredNow();
    }

    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.println("[MODEM] Modem is off after first PWRKEY pulse, powering it on...");
    }
    pulseModemPowerKey();

    if (!waitModemResponsive(40000)) {
        if (AppConfig::ENABLE_VERBOSE_DEBUG) {
            Serial.println("[MODEM] Modem did not come back after power-cycle");
        }
        return false;
    }
    transactAT(SerialAT, "AT+CFUN=0", 10000, 500);
    delay(2000); // Let the radio actually power down before requesting CFUN=1, or the modem stops responding
    // CNMP/CMNB only take effect on the next radio (re)start, so they must be set while CFUN=0
    transactAT(SerialAT, "AT+CNMP=38", 2000, 150);
    // transactAT(SerialAT, "AT+CMNB=3", 2000, 150); // revertido: no enlazaba, config previa (solo CNMP=38) funcionaba con Movistar
    transactAT(SerialAT, "AT+CFUN=1", 10000, 500);
    delay(1000);
    waitSimReady(8000);
    selectOperator();
    transactAT(SerialAT, "AT+CEREG=2", 2000, 150);
    transactAT(SerialAT, "AT+CGREG=2", 2000, 150);

    // A full power-cycle also resets the GNSS engine; turn it back on or GPS silently stops.
    transactAT(SerialAT, "AT+CGNSPWR=1", 2000, 150);

    ensureSimUnlocked();
    return isGsmRegisteredNow();
}

static void refreshInternalStatus(bool force)
{
    if (!force && millis() - lastStatusProbe < 4000) {
        return;
    }

    statusCPIN = transactAT(SerialAT, "AT+CPIN?", 1200, 120);

    // Anything other than a clean "+CPIN: READY" means the SIM needs re-unlocking;
    // this also covers bare ERROR replies while the SIM subsystem is still booting.
    if (statusCPIN.indexOf("CPIN: READY") < 0) {
        ensureSimUnlocked();
        statusCPIN = transactAT(SerialAT, "AT+CPIN?", 1200, 120);
    }

    statusCSQ = transactAT(SerialAT, "AT+CSQ", 1200, 120);
    statusCEREG = transactAT(SerialAT, "AT+CEREG?", 1200, 120);
    statusCGREG = transactAT(SerialAT, "AT+CGREG?", 1200, 120);

    deviceTelemetry.gsmSignalRssi = parseCsqRssi(statusCSQ);
    bool signalUsable = (deviceTelemetry.gsmSignalRssi != 99 && deviceTelemetry.gsmSignalRssi > 0);

    deviceTelemetry.gsmRegistered = (statusCEREG.indexOf(",1") >= 0) || (statusCEREG.indexOf(",5") >= 0) ||
                                    (statusCGREG.indexOf(",1") >= 0) || (statusCGREG.indexOf(",5") >= 0);

    if (!deviceTelemetry.gsmRegistered) {
        // Losing registration while GNSS is on is expected (shared RF, time-muxed with LTE);
        // only force a radio reset once we're actually trying to use the network (GNSS off).
        if (!gnssEnabled) {
            recoverGsmRegistration();
        }
        deviceTelemetry.gsmDataConnected = false;
        deviceTelemetry.gsmIp = "0.0.0.0";
    } else {
        lastGsmRadioReset = millis();
    }

    if (deviceTelemetry.gsmDataConnected) {
        deviceTelemetry.gsmState = "data_connected";
    } else if (deviceTelemetry.gsmRegistered && signalUsable) {
        deviceTelemetry.gsmState = "registered_no_data";
    } else if (deviceTelemetry.gsmRegistered) {
        deviceTelemetry.gsmState = "registered_weak_signal";
    } else if (!signalUsable) {
        deviceTelemetry.gsmState = "searching_no_signal";
    } else {
        deviceTelemetry.gsmState = "searching";
    }

    lastStatusProbe = millis();
}

static bool initializeGsmAtBoot()
{
    // Always printed, independent of registration/data success, so the actual GSM/APN config
    // in use is visible in the log even when the boot never reaches connectGprsDirect().
    logLine("CFG", "NODE_ID=" + String(AppConfig::NODE_ID));
    logLine("CFG", "SIM_PIN=" + String(String(AppConfig::SIM_PIN).length() > 0 ? "(set)" : "(vacio, no se envia CPIN)"));
    logLine("CFG", "APN=" + String(AppConfig::APN) + " APN_USER=" + String(AppConfig::APN_USER) +
                       " APN_PASS=" + String(String(AppConfig::APN_PASS).length() > 0 ? "(set)" : "(vacio)"));
    logLine("CFG", "SERVER_HOST=" + String(AppConfig::SERVER_HOST));
    logLine("CFG", "SERVER_PORT=" + String(AppConfig::SERVER_PORT));

    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.println("[GSM] Starting boot initialization");
    }

    transactAT(SerialAT, "ATE0", 2000, 150);
    if (!ensureSimUnlocked()) {
        Serial.println("[GSM] SIM unlock failed");
        return false;
    }

    // Sent here unconditionally (not gated behind a successful registration) so the APN config
    // is always visible in the log, even if the modem never actually attaches to a network.
    logLine("APN", "Configurando APN=" + String(AppConfig::APN) + " user=" + String(AppConfig::APN_USER));
    transactAT(SerialAT, "AT+CGDCONT=1,\"IP\",\"" + String(AppConfig::APN) + "\"", 2000, 150);
    logLine("APN", "CGDCONT? => " + transactAT(SerialAT, "AT+CGDCONT?", 2000, 150));
    // ip_type=0 (IPv4, coincide con el contexto "IP" de CGDCONT); authtype=0 (sin auth) salvo
    // que haya usuario/password configurados.
    bool hasApnAuth = String(AppConfig::APN_USER).length() > 0 || String(AppConfig::APN_PASS).length() > 0;
    String cncfg = "AT+CNCFG=0,0,\"" + String(AppConfig::APN) + "\",\"" +
                   String(AppConfig::APN_USER) + "\",\"" + String(AppConfig::APN_PASS) + "\"," +
                   (hasApnAuth ? "3" : "0");
    transactAT(SerialAT, cncfg, 2000, 150);
    logLine("APN", "CNCFG? => " + transactAT(SerialAT, "AT+CNCFG?", 2000, 150));

    transactAT(SerialAT, "AT+CFUN=0", 5000, 200);
    delay(2000); // Let the radio actually power down before requesting CFUN=1, or the modem stops responding
    // CNMP/CMNB only take effect on the next radio (re)start, so they must be set while CFUN=0
    transactAT(SerialAT, "AT+CNMP=38", 2000, 150); // LTE-only (Cat-M/NB-IoT); this SIM/location has no real 2G coverage
    // Cat-M1 solo no registraba: la torre puede dar LTE normal sin Cat-M1, asi que se habilita
    // tambien NB-IoT como alternativa (ver AT+CBANDCFG? mas abajo para las bandas buscadas).
    transactAT(SerialAT, "AT+CMNB=3", 2000, 150);
    logLine("BAND", "CBANDCFG? => " + transactAT(SerialAT, "AT+CBANDCFG?", 2000, 150));
    transactAT(SerialAT, "AT+CFUN=1", 10000, 500);
    delay(2000);
    waitSimReady(8000);

    if (AppConfig::SCAN_OPERATORS_AT_BOOT) {
        Serial.println("[GSM] Escaneando operadores visibles (AT+COPS=?), puede tardar ~1-2 min...");
        String opsList = transactUntilTerminal(SerialAT, "AT+COPS=?", 240000);
        Serial.println("[GSM] Operadores visibles:");
        Serial.println(opsList);
    }

    selectOperator();
    transactAT(SerialAT, "AT+CEREG=2", 2000, 150);
    transactAT(SerialAT, "AT+CGREG=2", 2000, 150);

    uint32_t deadline = millis() + 30000UL;
    bool registered = false;
    while (millis() < deadline) {
        if (isGsmRegisteredNow()) {
            registered = true;
            break;
        }
        delay(2000);
    }
    if (!registered) {
        Serial.println("[GSM] Boot network wait failed; periodic recovery remains enabled");
        return false;
    }

    deviceTelemetry.gsmRegistered = true;
    if (!connectGprsDirect()) {
        Serial.println("[GSM] Boot data attach failed; periodic retry remains enabled");
        return false;
    }

    Serial.print("[GSM] Boot data connected, IP=");
    Serial.println(deviceTelemetry.gsmIp);

    // Tear the PDP context down before enabling GNSS: they share RF on the SIM7080G and
    // cannot run concurrently (SIMCom Hardware Design Manual / board README).
    transactAT(SerialAT, "AT+CNACT=0,0", 10000, 300);
    deviceTelemetry.gsmDataConnected = false;
    deviceTelemetry.gsmIp = "0.0.0.0";
    deviceTelemetry.gsmState = "registered_no_data";

    logModemInitSequence();
    return true;
}


static void handleRoot()
{
    IPAddress ip = wifiStationMode ? WiFi.localIP() : WiFi.softAPIP();
    String html = "<html><head><meta charset='UTF-8'><title>Surface Node " + String(AppConfig::NODE_ID) + "</title>";
    html += "<style>body{font-family:Arial,sans-serif;padding:20px;} table{border-collapse:collapse;margin:12px 0;} td{border:1px solid #444;padding:8px 10px;} code{background:#f2f2f2;padding:2px 6px;border-radius:4px;} pre{background:#111;color:#eee;padding:12px;border-radius:8px;overflow:auto;} button{padding:6px 14px;margin-right:8px;} input{margin:2px 6px 2px 0;} </style></head><body>";
    html += "<h1>Surface Node " + String(AppConfig::NODE_ID) + "</h1>";
    html += "<p><strong>GSM</strong>: " + String(gsmFeatureEnabled ? "ENABLED" : "DISABLED") + "</p>";
    html += "<p><a href=\"/api/gsm?enabled=1\"><button>Enable GSM</button></a><a href=\"/api/gsm?enabled=0\"><button>Disable GSM</button></a></p>";
    html += "<p><strong>Acoustic Ping</strong>: " + String(acousticPingEnabled ? "ENABLED" : "DISABLED") + "</p>";
    html += "<p><a href=\"/api/ping?enabled=1\"><button>Enable Acoustic Ping</button></a><a href=\"/api/ping?enabled=0\"><button>Disable Acoustic Ping</button></a></p>";
    html += "<h2>Power Management</h2>";
    html += "<p><strong>Mode:</strong> Software-only Light Sleep</p>";
    html += "<p>The ESP32 enters Light Sleep while the acoustic modem remains active. "
            "Incoming acoustic-modem UART activity on GPIO44 wakes the ESP32.</p>";
    html += "<p><a href=\"/api/sleep\"><button style='background:#222;color:#fff;padding:10px 18px;'>ENTER LIGHT SLEEP</button></a></p>";
    html += "<p><small>Test safety: if no acoustic wake occurs, the node wakes automatically after 30 seconds.</small></p>";
    html += "<h2>Remote Control - Submerged Node</h2>";
    html += "<p><strong>Submerged Ping Status</strong>: (from broadcasts or last ACK)</p>";
    html += "<p>Last Command: <strong>" + String(deviceTelemetry.lastRemoteCommand) + "</strong> | ";
    html += "Status: <strong>" + String(deviceTelemetry.lastRemoteCommandStatus == 2 ? "ACK'd" : (deviceTelemetry.lastRemoteCommandStatus == 1 ? "Sent" : "None")) + "</strong>";
    if (deviceTelemetry.lastRemoteCommandRtt >= 0) {
        html += " | RTT: " + String(deviceTelemetry.lastRemoteCommandRtt) + "ms";
    }
    html += "</p>";
    html += "<p><a href=\"/api/submerged?ping=enable\"><button>ENABLE Submerged Pings</button></a><a href=\"/api/submerged?ping=disable\"><button>DISABLE Submerged Pings</button></a></p>";
    html += "<table>";
    html += "<tr><td>GPS</td><td>" + String(deviceTelemetry.gpsFix ? "FIX" : "NO FIX") + "</td></tr>";
    html += "<tr><td>Timestamp</td><td>" + jsonEscape(deviceTelemetry.gpsTimestamp) + "</td></tr>";
    html += "<tr><td>Lat</td><td>" + String(deviceTelemetry.latitude, 6) + "</td></tr>";
    html += "<tr><td>Lon</td><td>" + String(deviceTelemetry.longitude, 6) + "</td></tr>";
    html += "<tr><td>Ping</td><td>" + String(deviceTelemetry.pingOk ? "OK" : "NO OK") + "</td></tr>";
    html += "<tr><td>RTT</td><td>" + String(deviceTelemetry.pingRttMs) + " ms</td></tr>";
    html += "<tr><td>Acoustic Modem</td><td>" + String(deviceTelemetry.acousticModemHealthy ? "HEALTHY" : "ERROR") + "</td></tr>";
    html += "<tr><td>Modem Address</td><td>" + String(deviceTelemetry.acousticModemAddress) + "</td></tr>";
    html += "<tr><td>Modem Voltage</td><td>" + String(deviceTelemetry.acousticModemVoltageV, 2) + "V</td></tr>";
    html += "<tr><td colspan='2'><strong>Submerged Node Broadcast</strong></td></tr>";
    html += "<tr><td>Submerged RX OK</td><td>" + String(deviceTelemetry.submergedBroadcastOk ? "YES" : "NO") + "</td></tr>";
    html += "<tr><td>Source Address</td><td>" + String(deviceTelemetry.submergedSourceAddress) + "</td></tr>";
    html += "<tr><td>RTT A</td><td>" + String(deviceTelemetry.submergedRttA_ms >= 0 ? String(deviceTelemetry.submergedRttA_ms) + " ms" : "N/A") + "</td></tr>";
    html += "<tr><td>RTT B</td><td>" + String(deviceTelemetry.submergedRttB_ms >= 0 ? String(deviceTelemetry.submergedRttB_ms) + " ms" : "N/A") + "</td></tr>";
    html += "<tr><td>RTT C</td><td>" + String(deviceTelemetry.submergedRttC_ms >= 0 ? String(deviceTelemetry.submergedRttC_ms) + " ms" : "N/A") + "</td></tr>";
    html += "<tr><td>Distance A</td><td>" + String(deviceTelemetry.submergedDistanceA_m >= 0 ? String(deviceTelemetry.submergedDistanceA_m, 2) + " m" : "N/A") + "</td></tr>";
    html += "<tr><td>Distance B</td><td>" + String(deviceTelemetry.submergedDistanceB_m >= 0 ? String(deviceTelemetry.submergedDistanceB_m, 2) + " m" : "N/A") + "</td></tr>";
    html += "<tr><td>Distance C</td><td>" + String(deviceTelemetry.submergedDistanceC_m >= 0 ? String(deviceTelemetry.submergedDistanceC_m, 2) + " m" : "N/A") + "</td></tr>";
    html += "<tr><td>GSM registered</td><td>" + String(deviceTelemetry.gsmRegistered ? "YES" : "NO") + "</td></tr>";
    html += "<tr><td>GSM signal (RSSI)</td><td>" + String(deviceTelemetry.gsmSignalRssi) + (deviceTelemetry.gsmSignalRssi == 99 ? " (no signal)" : "") + "</td></tr>";
    html += "<tr><td>GSM state</td><td>" + jsonEscape(deviceTelemetry.gsmState) + "</td></tr>";
    html += "<tr><td>GSM data</td><td>" + String(deviceTelemetry.gsmDataConnected ? "UP" : "DOWN") + "</td></tr>";
    html += "<tr><td>GSM IP</td><td>" + jsonEscape(deviceTelemetry.gsmIp) + "</td></tr>";
    html += "<tr><td>State</td><td>" + jsonEscape(deviceTelemetry.state) + "</td></tr>";
    html += "</table>";
    html += "<p><strong>GET API</strong>: <code>/api/state</code></p>";
    html += "<p>Current local endpoint: <code>http://" + ip.toString() + "/api/state</code></p>";
    if (deviceTelemetry.gsmIp != "0.0.0.0") {
        html += "<p>Cellular PDP IP (informational only, not a reachable endpoint - the modem has no TCP server and the operator IP is usually behind CGNAT): <code>" + jsonEscape(deviceTelemetry.gsmIp) + "</code></p>";
    }
    html += "<pre>{\n  \"wifiApIp\": \"" + jsonEscape(ip.toString()) + "\",\n  \"gsmFeatureEnabled\": " + String(gsmFeatureEnabled ? "true" : "false") + ",\n  \"gsmRegistered\": " + String(deviceTelemetry.gsmRegistered ? "true" : "false") + ",\n  \"gsmDataConnected\": " + String(deviceTelemetry.gsmDataConnected ? "true" : "false") + ",\n  \"gsmSignalRssi\": " + String(deviceTelemetry.gsmSignalRssi) + ",\n  \"gsmState\": \"" + jsonEscape(deviceTelemetry.gsmState) + "\",\n  \"gsmIp\": \"" + jsonEscape(deviceTelemetry.gsmIp) + "\",\n  \"webUrl\": \"http://" + jsonEscape(ip.toString()) + "\",\n  \"state\": \"" + jsonEscape(deviceTelemetry.state) + "\"\n}</pre>";

    html += "<h2>WiFi</h2>";
    html += "<table>";
    html += "<tr><td>Mode</td><td>" + String(wifiStationMode ? "STATION (joined external AP)" : "ACCESS POINT") + "</td></tr>";
    html += "<tr><td>External AP SSID</td><td>" + jsonEscape(wifiStaSsid) + "</td></tr>";
    html += "<tr><td>LAN telemetry server</td><td>" + jsonEscape(lanServerHost) + ":" + String(lanServerPort) + "</td></tr>";
    html += "<tr><td>Current IP</td><td>" + ip.toString() + "</td></tr>";
    html += "</table>";
    html += "<p><a href=\"/api/wifi?mode=ap\"><button>Switch to AP mode</button></a><a href=\"/api/wifi?mode=station\"><button>Switch to Station mode</button></a></p>";
    html += "<form method=\"GET\" action=\"/api/wifi\">";
    html += "SSID: <input name=\"ssid\" value=\"" + jsonEscape(wifiStaSsid) + "\">";
    html += "Pass: <input name=\"pass\" type=\"password\" value=\"" + jsonEscape(wifiStaPass) + "\">";
    html += "LAN host: <input name=\"lanHost\" value=\"" + jsonEscape(lanServerHost) + "\">";
    html += "LAN port: <input name=\"lanPort\" value=\"" + String(lanServerPort) + "\">";
    html += "<button type=\"submit\">Save WiFi config</button>";
    html += "</form>";

    html += "<h2>API reference</h2>";
    html += "<pre>GET  /api/state                       -> full status JSON\n"
            "GET|POST /api/gsm?enabled=1|0         -> enable/disable cellular (saved to NVS)\n"
            "GET|POST /api/ping?enabled=1|0        -> enable/disable acoustic pings (saved to NVS)\n"
            "GET|POST /api/submerged?ping=enable|disable -> remote control submerged pings\n"
            "GET|POST /api/sleep                    -> enter Light Sleep; acoustic RX on GPIO44 wakes ESP32\n"
            "GET|POST /api/wifi?mode=ap|station&ssid=..&pass=..&lanHost=..&lanPort=..\n"
            "                                       -> configure WiFi (saved to NVS, any param optional)\n\n"
            "curl \"http://" + ip.toString() + "/api/state\"\n"
            "curl -X POST \"http://" + ip.toString() + "/api/gsm?enabled=0\"\n"
            "curl -X POST \"http://" + ip.toString() + "/api/ping?enabled=0\"\n"
            "curl -X POST \"http://" + ip.toString() + "/api/submerged?ping=enable\"\n"
            "curl -X POST \"http://" + ip.toString() + "/api/sleep\"\n"
            "curl -X POST \"http://" + ip.toString() + "/api/wifi?mode=station&ssid=MOVISTAR_F9FE&pass=XXXX&lanHost=192.168.1.62&lanPort=2100\"\n"
            "curl -X POST \"http://" + ip.toString() + "/api/wifi?mode=ap\"</pre>";

    html += "</body></html>";
    server.send(200, "text/html", html);
}

static void handleStatus()
{
    refreshInternalStatus(true);
    refreshGpsStatus();
    server.send(200, "application/json", buildStateJson());
}

static void handleState()
{
    refreshInternalStatus(true);
    refreshGpsStatus();
    server.send(200, "application/json", buildStateJson());
}

static void handleSend()
{
    String target = server.hasArg("target") ? server.arg("target") : "";
    String cmd = server.hasArg("cmd") ? server.arg("cmd") : "";

    if (!cmd.length()) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing cmd\"}");
        return;
    }

    String response;
    if (target == "int") {
        lastInternalCommand = cmd;
        response = transactAT(SerialAT, cmd);
        lastInternalResponse = response;
    } else if (target == "ext") {
        lastExternalCommand = cmd;
        response = transactAT(SerialUser, cmd);
        lastExternalResponse = response;
    } else {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"target must be int or ext\"}");
        return;
    }

    String body = "{\"ok\":true,\"response\":\"" + jsonEscape(response) + "\"}";
    server.send(200, "application/json", body);
}

// GET/POST /api/gsm?enabled=0|1 - lets an external app toggle the cellular feature at runtime.
static void handleGsmToggle()
{
    String enabledParam = server.hasArg("enabled") ? server.arg("enabled") : "";
    if (enabledParam.length() == 0) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing enabled param (0/1)\"}");
        return;
    }

    bool wasEnabled = gsmFeatureEnabled;
    gsmFeatureEnabled = (enabledParam == "1" || enabledParam == "true");
    saveRuntimeConfig();
    Serial.print("[GSM] Feature toggled via API: ");
    Serial.println(gsmFeatureEnabled ? "ENABLED" : "DISABLED");

    if (gsmFeatureEnabled && !wasEnabled) {
        // Force the next periodic tick (<=10s away) to immediately run the GNSS-off/registration
        // window instead of waiting up to TELEMETRY_INTERVAL_MS - without this, re-enabling GSM
        // from the web would silently do nothing until the next scheduled batch window.
        lastTelemetrySend = 0;
        Serial.println("[GSM] Forcing immediate registration attempt on next tick");
    }

    String body = "{\"ok\":true,\"gsmFeatureEnabled\":" + String(gsmFeatureEnabled ? "true" : "false") + "}";
    server.send(200, "application/json", body);
}

// GET/POST /api/ping?enabled=0|1 - lets an external app toggle acoustic pings at runtime.
static void handleAcousticPingToggle()
{
    String enabledParam = server.hasArg("enabled") ? server.arg("enabled") : "";
    if (enabledParam.length() == 0) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing enabled param (0/1)\"}");
        return;
    }

    acousticPingEnabled = (enabledParam == "1" || enabledParam == "true");
    saveRuntimeConfig();
    Serial.print("[ACOUSTIC] Ping toggled via API: ");
    Serial.println(acousticPingEnabled ? "ENABLED" : "DISABLED");

    String body = "{\"ok\":true,\"acousticPingEnabled\":" + String(acousticPingEnabled ? "true" : "false") + "}";
    server.send(200, "application/json", body);
}

// GET/POST /api/submerged?ping=enable|disable - send remote control command to submerged node
static void handleSubmergedControl()
{
    String pingParam = server.hasArg("ping") ? server.arg("ping") : "";
    if (pingParam.length() == 0) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing ping param (enable/disable)\"}");
        return;
    }

    bool enableRemote = (pingParam == "enable" || pingParam == "1" || pingParam == "true");
    
    Serial.print("[API] Enviando comando de control remoto al nodo sumergido: ");
    Serial.println(enableRemote ? "ENABLE PING" : "DISABLE PING");

    sendUnicastCommand(enableRemote);

    String statusStr;
    if (deviceTelemetry.lastRemoteCommandStatus == 2) {
        statusStr = "acked";
    } else if (deviceTelemetry.lastRemoteCommandStatus == 1) {
        statusStr = "sent";
    } else {
        statusStr = "failed";
    }

    String body = "{\"ok\":true,\"command\":\"" + String(enableRemote ? "enable" : "disable") + 
                  "\",\"status\":\"" + statusStr + 
                  "\",\"rttMs\":" + String(deviceTelemetry.lastRemoteCommandRtt) + "}";
    server.send(200, "application/json", body);
}

// Applies wifiStationMode/wifiStaSsid/wifiStaPass to the radio; called at boot and whenever
// /api/wifi changes them. HTTP routes stay registered across calls, only the radio is redone.
static const char *wifiStatusToString(wl_status_t status)
{
    switch (status) {
    case WL_IDLE_STATUS:     return "IDLE";
    case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED";
    case WL_CONNECTED:       return "CONNECTED";
    case WL_CONNECT_FAILED:  return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED:    return "DISCONNECTED";
    default:                 return "UNKNOWN";
    }
}

static void enterAcousticLightSleep()
{
    Serial.println();
    Serial.println("=================================");
    Serial.println("[SLEEP] Entering Light Sleep");
    Serial.println("[SLEEP] Wake source: acoustic modem RX on GPIO44");
    Serial.println("[SLEEP] Safety wake timer: 30 seconds");
    Serial.println("=================================");

    /*
     * Existing acoustic-modem UART connection:
     *   modem TX -> ESP32 GPIO44 (Serial2 RX)
     *   ESP32 GPIO43 (Serial2 TX) -> modem RX
     *
     * No additional electronics are used. The acoustic modem remains powered.
     * We do NOT reconfigure GPIO44 with pinMode(), because Serial2 already owns it.
     */

    // If RX is already LOW, a LOW-level wake source would wake immediately.
    // Abort the sleep attempt and print a diagnostic instead.
    if (digitalRead(USER_UART_RXD_PIN) == LOW) {
        Serial.println("[SLEEP] Cancelled: GPIO44 is already LOW.");
        Serial.println("[SLEEP] Check acoustic modem UART idle level / connection.");
        return;
    }

    // Remove wake sources left by any earlier test.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    // UART RX normally idles HIGH. Incoming serial data contains a LOW start bit.
    esp_err_t gpioWakeResult = gpio_wakeup_enable(
        (gpio_num_t)USER_UART_RXD_PIN,
        GPIO_INTR_LOW_LEVEL
    );
    esp_err_t sleepWakeResult = esp_sleep_enable_gpio_wakeup();

    if (gpioWakeResult != ESP_OK || sleepWakeResult != ESP_OK) {
        Serial.print("[SLEEP] ERROR configuring GPIO wake. gpio=");
        Serial.print((int)gpioWakeResult);
        Serial.print(" sleep=");
        Serial.println((int)sleepWakeResult);

        gpio_wakeup_disable((gpio_num_t)USER_UART_RXD_PIN);
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
        return;
    }

    // Temporary fallback for the first bench test only.
    // If no acoustic UART activity arrives, wake after 30 s.
    esp_err_t timerResult = esp_sleep_enable_timer_wakeup(30ULL * 1000000ULL);
    if (timerResult != ESP_OK) {
        Serial.print("[SLEEP] WARNING: safety timer setup failed: ");
        Serial.println((int)timerResult);
    }

    // In this AP-mode test the Wi-Fi network intentionally disappears during sleep.
    // The ESP32 is not powered off; the radio is simply disabled before Light Sleep.
    WiFi.mode(WIFI_OFF);
    delay(150);

    Serial.println("[SLEEP] NOW");
    Serial.flush();

    // Execution pauses here and continues below after a wake event.
    esp_err_t sleepResult = esp_light_sleep_start();
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    Serial.println();
    Serial.println("=================================");
    Serial.println("[WAKE] ESP32 is awake");

    if (sleepResult != ESP_OK) {
        Serial.print("[WAKE] esp_light_sleep_start returned error: ");
        Serial.println((int)sleepResult);
    }

    if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        Serial.println("[WAKE] Cause: ACOUSTIC RX activity on GPIO44");
    } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
        Serial.println("[WAKE] Cause: 30 s safety timer");
    } else {
        Serial.print("[WAKE] Cause code: ");
        Serial.println((int)cause);
    }

    Serial.println("=================================");

    // Clean up the temporary wake configuration.
    gpio_wakeup_disable((gpio_num_t)USER_UART_RXD_PIN);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    // Restore Node-C Wi-Fi using the existing config.h settings.
    configureWifiRadio();

    // Routes remain registered. Restart the listener after the Wi-Fi interface returns.
    server.begin();
    delay(500);

    Serial.println("[WAKE] Wi-Fi restored");
}
static void configureWifiRadio()
{
    if (wifiStationMode) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(wifiStaSsid.c_str(), wifiStaPass.c_str());

        Serial.print("[WIFI] Connecting to \"");
        Serial.print(wifiStaSsid);
        Serial.println("\"...");

        uint32_t deadline = millis() + 20000UL;
        wl_status_t lastStatus = (wl_status_t)-1;
        while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
            wl_status_t st = WiFi.status();
            if (st != lastStatus) {
                Serial.print("[WIFI] status=");
                Serial.print(st);
                Serial.print(" (");
                Serial.print(wifiStatusToString(st));
                Serial.println(")");
                lastStatus = st;
            }
            delay(500);
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.print("[WIFI] Connected, IP: ");
            Serial.print(WiFi.localIP());
            Serial.print(", RSSI: ");
            Serial.print(WiFi.RSSI());
            Serial.println(" dBm");
            Serial.print("[WIFI] Will push each record to ");
            Serial.print(lanServerHost);
            Serial.print(":");
            Serial.println(lanServerPort);
        } else {
            Serial.print("[WIFI] Failed to join external AP after timeout, last status=");
            Serial.print(WiFi.status());
            Serial.print(" (");
            Serial.print(wifiStatusToString(WiFi.status()));
            Serial.println("); local API will be unreachable until connected");
        }
    } else {
        WiFi.mode(WIFI_AP);

        IPAddress apIp(AppConfig::WIFI_AP_IP);
        IPAddress gateway(AppConfig::WIFI_AP_GATEWAY);
        IPAddress subnet(AppConfig::WIFI_AP_SUBNET);
        WiFi.softAPConfig(apIp, gateway, subnet);
        WiFi.softAP(AppConfig::WIFI_AP_SSID, AppConfig::WIFI_AP_PASS);

        if (AppConfig::ENABLE_VERBOSE_DEBUG) {
            Serial.println();
            Serial.println("[WEB] Access point started");
            Serial.print("[WEB] SSID: ");
            Serial.println(AppConfig::WIFI_AP_SSID);
            Serial.print("[WEB] PASS: ");
            Serial.println(AppConfig::WIFI_AP_PASS);
            Serial.print("[WEB] IP  : ");
            Serial.println(WiFi.softAPIP());
        }
    }
}

// GET/POST /api/wifi - configures WiFi mode/credentials/LAN server at runtime (saved to NVS).
// Any parameter is optional; only provided ones are updated. Switching mode drops the current
// connection, so the response is sent before the radio is reconfigured.
static void handleSleep()
{
    server.send(
        200,
        "application/json",
        "{\"ok\":true,\"message\":\"Entering Light Sleep; wake on acoustic GPIO44 or 30 s safety timer\"}"
    );

    // Do not sleep inside the HTTP callback. Let the response reach the browser,
    // then the main loop enters Light Sleep.
    sleepRequested = true;
}
static void handleWifiConfig()
{
    bool changed = false;

    if (server.hasArg("mode")) {
        String m = server.arg("mode");
        bool newMode = (m == "station" || m == "sta" || m == "1");
        if (newMode != wifiStationMode) {
            wifiStationMode = newMode;
            changed = true;
        }
    }
    if (server.hasArg("ssid")) {
        wifiStaSsid = server.arg("ssid");
        changed = true;
    }
    if (server.hasArg("pass")) {
        wifiStaPass = server.arg("pass");
        changed = true;
    }
    if (server.hasArg("lanHost")) {
        lanServerHost = server.arg("lanHost");
        changed = true;
    }
    if (server.hasArg("lanPort")) {
        lanServerPort = static_cast<uint16_t>(server.arg("lanPort").toInt());
        changed = true;
    }

    if (changed) {
        saveRuntimeConfig();
    }

    String body = "{\"ok\":true,\"wifiMode\":\"" + String(wifiStationMode ? "station" : "ap") +
                  "\",\"ssid\":\"" + jsonEscape(wifiStaSsid) + "\",\"lanHost\":\"" + jsonEscape(lanServerHost) +
                  "\",\"lanPort\":" + String(lanServerPort) + "}";
    server.send(200, "application/json", body);

    if (changed) {
        delay(200);
        configureWifiRadio();
    }
}

static void startWebAP()
{
    configureWifiRadio();

    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/send", HTTP_GET, handleSend);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/state", HTTP_GET, handleState);
    server.on("/api/gsm", HTTP_GET, handleGsmToggle);
    server.on("/api/gsm", HTTP_POST, handleGsmToggle);
    server.on("/api/ping", HTTP_GET, handleAcousticPingToggle);
    server.on("/api/ping", HTTP_POST, handleAcousticPingToggle);
    server.on("/api/submerged", HTTP_GET, handleSubmergedControl);
    server.on("/api/submerged", HTTP_POST, handleSubmergedControl);
    server.on("/api/wifi", HTTP_GET, handleWifiConfig);
    server.on("/api/wifi", HTTP_POST, handleWifiConfig);
    server.on("/api/sleep", HTTP_GET, handleSleep);
    server.on("/api/sleep", HTTP_POST, handleSleep);
    server.begin();
}

static void processConsoleLine(const String &line)
{
    if (!line.length()) {
        return;
    }

    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.print("[CONSOLE] ");
        Serial.println(line);
    }

    if (line[0] == '$') {
        String cmd = line.substring(1);
        if (AppConfig::ENABLE_VERBOSE_DEBUG) {
            Serial.print("[EXT TX RAW] ");
            Serial.println(cmd);
        }
        lastExternalCommand = cmd;
        lastExternalResponse = transactRaw(SerialUser, cmd, AppConfig::ACOUSTIC_TIMEOUT_MS);
        if (AppConfig::ENABLE_VERBOSE_DEBUG) {
            Serial.print("[EXT RX RAW] ");
            Serial.println(lastExternalResponse);
        }

        int target = 0;
        int rttMs = -1;
        float distanceM = 0.0f;
        deviceTelemetry.pingOk = parseAcousticReply(lastExternalResponse, target, rttMs, distanceM);
        deviceTelemetry.pingTarget = target;
        deviceTelemetry.pingRttMs = rttMs;
        deviceTelemetry.pingDistanceM = distanceM;
        deviceTelemetry.pingReply = lastExternalResponse;
        applyLedState();
        return;
    }

    lastInternalCommand = line;
    lastInternalResponse = transactAT(SerialAT, line, 3000, 200);
    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.print("[INT RX] ");
        Serial.println(lastInternalResponse);
    }
}

void setup()
{
    Serial.begin(115200);
    loadRuntimeConfig();

    if (!PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C_SDA, I2C_SCL)) {
        if (AppConfig::ENABLE_VERBOSE_DEBUG) {
            Serial.println("Failed to initialize power.....");
        }
        assert(0);
    }

    PMU.setChargingLedMode(XPOWERS_CHG_LED_ON);
    PMU.disableDC2();
    PMU.disableDC4();
    PMU.disableDC5();
    PMU.disableALDO1();
    PMU.disableALDO2();
    PMU.disableALDO3();
    PMU.disableALDO4();
    PMU.disableBLDO2();
    PMU.disableCPUSLDO();
    PMU.disableDLDO1();
    PMU.disableDLDO2();

    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED) {
        PMU.disableDC3();
        delay(200);
    }

    PMU.setBLDO1Voltage(3300);
    PMU.enableBLDO1();
    PMU.setDC3Voltage(3000);
    PMU.enableDC3();
    PMU.setBLDO2Voltage(3300);
    PMU.enableBLDO2();

    // WiFi AP activity can briefly starve the UART1 ISR; a bigger RX buffer avoids losing/
    // corrupting AT replies (e.g. CAOPEN) that arrive during that contention.
    Serial1.setRxBufferSize(1024);
    Serial1.begin(115200, SERIAL_8N1, BOARD_MODEM_RXD_PIN, BOARD_MODEM_TXD_PIN);
    SerialUser.begin(USER_UART_BAUD, SERIAL_8N1, USER_UART_RXD_PIN, USER_UART_TXD_PIN);
    pinMode(BOARD_MODEM_PWR_PIN, OUTPUT);
    configureAcousticModemAddress();

    int retry = 0;
    while (transactAT(SerialAT, "AT", 1000, 150).indexOf("OK") < 0) {
        if (AppConfig::ENABLE_VERBOSE_DEBUG) {
            Serial.print("[MODEM] testAT retry ");
            Serial.println(retry);
        }
        if (retry++ > 10) {
            digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
            delay(100);
            digitalWrite(BOARD_MODEM_PWR_PIN, HIGH);
            delay(1000);
            digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
            retry = 0;
            if (AppConfig::ENABLE_VERBOSE_DEBUG) {
                Serial.println("[MODEM] power-cycle done");
            }
        }
    }

    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.println("[MODEM] modem ready");
    }
    lastGsmRadioReset = millis();

    // Forced unconditionally at boot: doesn't depend on SIM unlock/registration succeeding.
    transactAT(SerialAT, "AT+CCID", 2000, 150);

    if (gsmFeatureEnabled) {
        initializeGsmAtBoot();
    } else {
        // GSM feature disabled: skip network registration entirely, GNSS can stay on permanently.
        transactAT(SerialAT, "ATE0", 2000, 150);
        transactAT(SerialAT, "AT+CGNSPWR=1", 2000, 150);
        gnssEnabled = true;
        if (AppConfig::ENABLE_VERBOSE_DEBUG) {
            Serial.println("[GSM] Feature disabled at boot; GNSS-only mode");
        }
    }

    startWebAP();

    refreshGpsStatus();
    refreshInternalStatus(true);
    applyLedState();

    if (AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.println("\n=== [SYSTEM READY] ===");
    }
    sendAcousticPingDebug();
}

void loop()
{
    updateStatusLed();
    server.handleClient();

    if (sleepRequested) {
        sleepRequested = false;

        // Give the HTTP response time to leave before Node-C Wi-Fi is disabled.
        delay(400);

        enterAcousticLightSleep();
    }
    if (SerialAT.available() && AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.print("[INT ASYNC] ");
    }
    while (SerialAT.available()) {
        char c = static_cast<char>(SerialAT.read());
        appendCapped(lastInternalAsync, c);
        if (AppConfig::ENABLE_VERBOSE_DEBUG) {
            Serial.write(c);
        }
    }

    if (SerialUser.available() && AppConfig::ENABLE_VERBOSE_DEBUG) {
        Serial.print("[EXT ASYNC] ");
    }
    while (SerialUser.available()) {
        char c = static_cast<char>(SerialUser.read());
        appendCapped(lastExternalAsync, c);
        if (AppConfig::ENABLE_VERBOSE_DEBUG) {
            Serial.write(c);
        }
    }

    while (Serial.available()) {
        char ch = static_cast<char>(Serial.read());
        if (ch == '\r') continue;
        if (ch == '\n') {
            processConsoleLine(consoleLine);
            consoleLine = "";
            continue;
        }
        consoleLine += ch;
    }

    static uint32_t lastPeriodic = 0;
    if (millis() - lastPeriodic > 5000) {
        lastPeriodic = millis();
        
        sendAcousticPingDebug();

        if (wifiStationMode && WiFi.status() != WL_CONNECTED) {
            Serial.print("[WIFI] Not connected (status=");
            Serial.print(wifiStatusToString(WiFi.status()));
            Serial.println("), attempting reconnect...");
            WiFi.reconnect();
        }

        if (!gsmFeatureEnabled) {
            // Cellular fully disabled: no PDP/TCP will ever be attempted, so GNSS just stays on.
            if (!gnssEnabled) {
                transactAT(SerialAT, "AT+CGNSPWR=1", 2000, 150);
                gnssEnabled = true;
            }
            refreshGpsStatus();
            //sendAcousticPingDebug();
            if (wifiStationMode) {
                sendRecordToLan(buildStateJson() + "\n");
            }
        } else if (millis() - lastTelemetrySend >= AppConfig::TELEMETRY_INTERVAL_MS) {
            lastTelemetrySend = millis();
            sendPendingRecordsBatch();
        } else if (gnssEnabled) {
            // GNSS phase: read GPS/ping and accumulate a snapshot; no network/PDP activity here
            // since GNSS and LTE/TCP share RF on the SIM7080G (SIMCom Hardware Design Manual).
            refreshGpsStatus();
            refreshInternalStatus();

            // Ping acústico siempre, con o sin fix GPS / registro GSM.
            //sendAcousticPingDebug();

            // Newline-delimited so the server (which reads one JSON object per line) can
            // split consecutive records sent back-to-back over the same socket.
            String record = buildStateJson() + "\n";
            if (wifiStationMode) {
                // LAN mode sends immediately, one at a time - no GNSS/cellular contention over WiFi.
                sendRecordToLan(record);
            }
            // Always queue for the cellular batch window too, independently of WiFi mode -
            // otherwise enabling GSM while in station mode would never actually reach the
            // cellular telemetry server.
            if (pendingRecords.size() >= AppConfig::MAX_PENDING_RECORDS) {
                pendingRecords.erase(pendingRecords.begin());
            }
            pendingRecords.push_back(record);
        }

        applyLedState();
    }

    delay(1);
}

