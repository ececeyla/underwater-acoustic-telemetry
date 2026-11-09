/**
 * SubmergedNode.ino
 * 
 * Firmware para nodo sumergido que:
 * 1. Hace 3 pings acústicos secuenciales a los nodos de superficie A, B, C
 * 2. Recopila los tiempos de viaje (RTT)
 * 3. Formatea un mensaje: Axxxx,Byyyy,Czzzz (max 64 bytes)
 * 4. Envía por canal acústico: $Bnnnddd... (donde nnn = byte count, ddd = mensaje)
 * 
 * El formato de ping/respuesta es:
 *   - Enviar: $P<TARGET_ADDRESS>
 *   - Recibir: #R<TARGET_ADDRESS>T<RTT_MS>
 *   
 * Ejemplo: 
 *   Envío: $P110
 *   Respuesta: #R110T00500
 *   (ping a nodo 110, tiempo de viaje 500ms)
 */

#include <Arduino.h>
#include "config.h"

// Configuración de puertos serie
#define SerialUser Serial2  // Modem acústico en Serial2
#define USER_UART_TXD_PIN 43
#define USER_UART_RXD_PIN 44
#define USER_UART_BAUD 9600

// Estructura para almacenar resultado de un ping acústico
struct PingResult {
    uint16_t target;      // Dirección del nodo (110, 110, 120)
    int rttMs;            // Round-trip time en milisegundos (-1 si fallo)
    float distanceM;      // Distancia calculada en metros
    bool valid;           // true si el ping fue exitoso
};

// Variables globales
unsigned long lastPingCycle = 0;
char pingBuffer[256];
String asyncBuffer = "";  // Buffer para mensajes asincronos del modem

// ============================================================================
// UTILIDADES DE COMUNICACIÓN
// ============================================================================

/**
 * Envía un comando y espera respuesta con timeout.
 * Estrategia robusta: limpia agresivamente, descarta echo, captura respuesta.
 * @param cmd Comando a enviar (ej: "$P110", "$?")
 * @param timeoutMs Tiempo máximo de espera TOTAL
 * @return String con la respuesta recibida (sin el echo del comando)
 */
static String transactRaw(const String &cmd, uint32_t timeoutMs)
{
    // PASO 1: Limpiar el buffer AGRESIVAMENTE
    unsigned long cleanStart = millis();
    while (millis() - cleanStart < 200) {
        while (SerialUser.available()) {
            SerialUser.read();
            delayMicroseconds(10);
        }
        delay(10);
    }
    
    delay(100);  // Pausa para estabilizar
    
    // PASO 2: Enviar el comando
    SerialUser.print(cmd);
    SerialUser.print("\r\n");
    SerialUser.flush();
    
    // PASO 3: ESPERAR EL '#' (marca el inicio de la respuesta del modem)
    // Este timeout es MÁS LARGO porque esperamos que el modem procese el comando
    // El modem SIEMPRE debe responder con algo que empiece con '#'
    // (respuesta válida, timeout acústico #TO, etc)
    unsigned long hashWaitStart = millis();
    unsigned long hashWaitTimeout = timeoutMs + 1000;  // Espera extra para encontrar '#'
    
    while (millis() - hashWaitStart < hashWaitTimeout) {
        if (SerialUser.available()) {
            char c = SerialUser.read();
            // Cuando vemos '#', ese es el inicio de la respuesta real
            if (c == '#') {
                // Comenzar a capturar la respuesta
                String response = "#";
                unsigned long responseStart = millis();
                
                // PASO 4: Capturar la respuesta completa
                while (millis() - responseStart < timeoutMs) {
                    if (SerialUser.available()) {
                        char rc = SerialUser.read();
                        response += rc;
                        
                        // Detectar fin de respuesta
                        // #TO = fin inmediato (3 chars)
                        if (response == "#TO") {
                            return response;
                        }
                        
                        // #R...T... = respuesta de ping, esperar a terminar dígitos
                        if (response.startsWith("#R") && response.indexOf('T') > 0) {
                            int tPos = response.indexOf('T');
                            int digitCount = 0;
                            unsigned long digitStart = millis();
                            while (millis() - digitStart < 150 && digitCount < 5) {
                                if (SerialUser.available()) {
                                    char dc = SerialUser.read();
                                    if (isdigit(dc)) {
                                        response += dc;
                                        digitCount++;
                                    } else {
                                        return response;
                                    }
                                }
                            }
                            return response;
                        }
                        
                        // #A...V... = respuesta de identidad, esperar a terminar dígitos
                        if (response.startsWith("#A") && response.indexOf('V') > 0) {
                            int vPos = response.indexOf('V');
                            int digitCount = 0;
                            unsigned long digitStart = millis();
                            while (millis() - digitStart < 150 && digitCount < 5) {
                                if (SerialUser.available()) {
                                    char dc = SerialUser.read();
                                    if (isdigit(dc)) {
                                        response += dc;
                                        digitCount++;
                                    } else {
                                        return response;
                                    }
                                }
                            }
                            return response;
                        }
                    }
                }
                
                // Timeout en captura de respuesta
                return response;
            }
        }
    }
    
    // Timeout esperando '#' - esto significa el modem no respondió
    return "";
}

/**
 * Configura la dirección propia del módem acústico ($A<3 dígitos>), enviado una vez al arranque.
 */
static void configureAcousticModemAddress()
{
    char addr[4];
    snprintf(addr, sizeof(addr), "%03u", AppConfig::SUBMERGED_ADDRESS % 1000);
    String cmd = "$A" + String(addr);
    transactRaw(cmd, AppConfig::ACOUSTIC_TIMEOUT_MS);
}

/**
 * Parsea una respuesta de identidad acústica del formato #A<ADDRESS>V<VOLTAGE_RAW>
 * @param raw String con respuesta cruda
 * @param address Referencia donde se guardará la dirección del modem
 * @param voltageRaw Referencia donde se guardará el valor bruto de voltaje (16-bit)
 * @param voltageV Referencia donde se guardará el voltaje real en Voltios
 * @return true si el parseo fue exitoso, false si falló
 */
static bool parseAcousticIdentity(const String &raw, uint16_t &address, uint16_t &voltageRaw, float &voltageV)
{
    String clean = raw;
    clean.replace("\r", " ");
    clean.replace("\n", " ");
    clean.trim();
    
    // Buscar el patrón #A
    int replyStart = clean.lastIndexOf("#A");
    if (replyStart < 0) {
        return false;
    }
    
    String reply = clean.substring(replyStart);
    reply.trim();
    
    // Buscar la V que separa address de voltage
    int addressEnd = reply.indexOf('V');
    if (!reply.startsWith("#A") || addressEnd <= 0) {
        return false;
    }
    
    String addressField = reply.substring(2, addressEnd);
    addressField.trim();
    
    if (addressField.length() == 0) {
        return false;
    }
    
    address = (uint16_t)addressField.toInt();
    
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
    voltageRaw = (uint16_t)voltageField.toInt();
    voltageV = (float)voltageRaw * 15.0f / 65536.0f;  // Convertir a Voltios
    
    return true;
}

/**
 * Parsea una respuesta acústica del formato #R<TARGET>T<RTT_VALUE>
 * @param raw String con respuesta cruda
 * @param result Estructura PingResult a llenar
 * @return true si el parseo fue exitoso, false si falló
 */
static bool parseAcousticReply(const String &raw, PingResult &result)
{
    String clean = raw;
    clean.replace("\r", " ");
    clean.replace("\n", " ");
    clean.trim();
    
    // Buscar el patrón #R
    int replyStart = clean.lastIndexOf("#R");
    if (replyStart < 0) {
        result.valid = false;
        return false;
    }
    
    String reply = clean.substring(replyStart);
    reply.trim();
    
    // Buscar la T que separa target de RTT
    int targetEnd = reply.indexOf('T');
    if (!reply.startsWith("#R") || targetEnd <= 0) {
        result.valid = false;
        return false;
    }
    
    String targetField = reply.substring(2, targetEnd);
    String rttField = reply.substring(targetEnd + 1);
    targetField.trim();
    rttField.trim();
    
    if (targetField.length() == 0 || rttField.length() == 0) {
        result.valid = false;
        return false;
    }
    
    result.target = (uint16_t)targetField.toInt();
    result.rttMs = rttField.toInt();
    // Fórmula de distancia: distancia = rtt_value * sound_velocity * 3.125e-5
    result.distanceM = (float)result.rttMs * AppConfig::SPEED_OF_SOUND_M_PER_S * 3.125e-5f;
    result.valid = true;
    
    return true;
}

// ============================================================================
// VERIFICACIÓN Y HEALTH CHECK
// ============================================================================

/**
 * Verifica que el modem acústico está vivo y obtiene su información.
 * Envía $? y espera respuesta #AxxxVyyyyy
 * @return true si el modem respondió correctamente, false en caso contrario
 */
static bool healthCheckAcousticModem()
{
    Serial.println("\n=== [HEALTH CHECK] ===");
    Serial.println("Verificando modem acústico con comando $?...");
    
    String response = transactRaw("$?", 2000);  // Timeout más corto para health check
    
    Serial.print("Respuesta: ");
    Serial.println(response.isEmpty() ? "(vacía)" : response);
    
    uint16_t address, voltageRaw;
    float voltageV;
    
    if (parseAcousticIdentity(response, address, voltageRaw, voltageV)) {
        Serial.print("✓ Modem detectado:");
        Serial.print("  - Dirección: ");
        Serial.println(address);
        Serial.print("  - Voltaje: ");
        Serial.print(voltageV, 2);
        Serial.print("V (raw: ");
        Serial.print(voltageRaw);
        Serial.println(")");
        return true;
    } else {
        Serial.println("✗ No se detectó respuesta válida del modem");
        Serial.println("  Verifica: conexión física, baud rate (9600), direccionamiento");
        return false;
    }
}

// ============================================================================
// LÓGICA DE PING ACÚSTICO
// ============================================================================

/**
 * Hace un ping acústico a un objetivo específico con reintentos.
 * Antes de cada ping, verifica que el modem está listo.
 * Si no hay respuesta, intenta recuperar el modem antes de reintentar.
 * @param target Dirección del nodo (ej: 110, 110, 120)
 * @return PingResult con el resultado del ping
 */
static PingResult performPing(uint16_t target)
{
    PingResult result;
    result.target = target;
    result.rttMs = -1;
    result.distanceM = 0.0f;
    result.valid = false;
    
    // Intentar hasta MAX_PING_RETRIES veces
    for (uint8_t attempt = 0; attempt < AppConfig::MAX_PING_RETRIES; attempt++) {
        
        // PASO 1: Verificar que el modem está listo con $?
        Serial.print("  [Pre-check] Verificando modem con $?...");
        String healthResponse = transactRaw("$?", 1000);
        
        uint16_t checkAddr, checkVoltageRaw;
        float checkVoltageV;
        
        if (!parseAcousticIdentity(healthResponse, checkAddr, checkVoltageRaw, checkVoltageV)) {
            Serial.println(" ✗ Modem no está listo, saltando ping");
            continue;  // Ir al siguiente intento
        }
        Serial.println(" ✓ Modem listo");
        
        // PASO 2: Enviar ping
        String cmd = "$P" + String(target);
        
        Serial.print("  [Ping] Enviando: ");
        Serial.print(cmd);
        Serial.print(" (intento ");
        Serial.print(attempt + 1);
        Serial.println(")");
        
        String response = transactRaw(cmd, AppConfig::ACOUSTIC_TIMEOUT_MS);
        
        Serial.print("    → Respuesta: ");
        Serial.println(response.isEmpty() ? "(vacía)" : response);
        
        // PASO 3: Procesar respuesta
        
        // Si respuesta está vacía, significa NO vimos '#' en todo el timeout
        // El modem está en un estado desconocido, intentar recuperarlo
        if (response.isEmpty()) {
            Serial.println("    ⚠️  Sin respuesta del modem (sin #), intentando recuperar...");
            // Intentar despertar el modem con otro $?
            delay(200);
            String recoveryResponse = transactRaw("$?", 1000);
            if (!recoveryResponse.isEmpty()) {
                Serial.println("    ✓ Modem recuperado, reintentando ping");
            } else {
                Serial.println("    ✗ Modem no responde, fallo crítico");
            }
            continue;  // Reintentar el ping
        }
        
        // Si recibimos #TO, el nodo target no respondió
        if (response.indexOf("#TO") >= 0) {
            Serial.println("    ⏱ Timeout: el nodo target no respondió");
            continue;  // Reintentar
        }
        
        // Si recibimos #R...T..., procesar como éxito
        if (parseAcousticReply(response, result)) {
            Serial.print("    ✓ Éxito: RTT=");
            Serial.print(result.rttMs);
            Serial.print("ms, Distancia=");
            Serial.print(result.distanceM, 1);
            Serial.println("m");
            return result;
        }
        
        // Respuesta no fue vacía, vimos '#', pero no pudimos parsear
        Serial.println("    ⚠️  Respuesta recibida pero no válida, reintentando");
        
        // Esperar un poco antes de reintentar
        if (attempt < AppConfig::MAX_PING_RETRIES - 1) {
            delay(200);
        }
    }
    
    Serial.println("    ✗ Fallo: no se recibió respuesta válida después de reintentos");
    return result;
}

/**
 * Ejecuta un ciclo completo de 3 pings secuenciales a A, B, C.
 * @return Array con 3 PingResult (uno por cada nodo)
 */
static void performPingCycle(PingResult results[3])
{
    Serial.println("\n=== [PING CYCLE] ===");
    Serial.println("Haciendo 3 pings secuenciales...");
    
    // Ping a Nodo A 
    Serial.println("1. Nodo A:");
    results[0] = performPing(AppConfig::ACOUSTIC_TARGET_A);
    delay(AppConfig::DELAY_BETWEEN_PINGS_MS);
    
    // Ping a Nodo B 
    Serial.println("2. Nodo B:");
    results[1] = performPing(AppConfig::ACOUSTIC_TARGET_B);
    delay(AppConfig::DELAY_BETWEEN_PINGS_MS);
    
    // Ping a Nodo C 
    Serial.println("3. Nodo C:");
    results[2] = performPing(AppConfig::ACOUSTIC_TARGET_C);
    
    Serial.println("Ciclo completado.\n");
}

// ============================================================================
// FORMATEO Y ENVÍO DEL MENSAJE
// ============================================================================

/**
 * Formatea los 3 resultados de ping en el payload del mensaje.
 * Formato: Axxxx,Byyyy,Czzzz
 * donde xxxx, yyyy, zzzz son los tiempos RTT en ms (sin padding)
 * 
 * @param results Array de 3 PingResult
 * @param payload Buffer donde se escribirá el payload formateado
 * @param maxLen Tamaño máximo del buffer
 * @return Número de bytes del payload (0 si error)
 */
static uint16_t formatPayload(const PingResult results[3], char *payload, size_t maxLen)
{
    // Limpiar buffer
    memset(payload, 0, maxLen);
    
    // Construir payload: Axxxx,Byyyy,Czzzz
    // Si un ping falló, usar -1 (ej: A-1,B500,C600)
    int len = snprintf(payload, maxLen, "A%d,B%d,C%d",
                       results[0].valid ? results[0].rttMs : -1,
                       results[1].valid ? results[1].rttMs : -1,
                       results[2].valid ? results[2].rttMs : -1);
    
    if (len < 0 || len >= (int)maxLen) {
        Serial.println("[ERROR] Payload buffer overflow");
        return 0;
    }
    
    Serial.print("[Format] Payload: ");
    Serial.print(payload);
    Serial.print(" (");
    Serial.print(len);
    Serial.println(" bytes)");
    
    return (uint16_t)len;
}

/**
 * Parsea una confirmación de envío del formato $Bnn
 * @param raw String con respuesta cruda
 * @param confirmedBytes Referencia donde guardar el número de bytes confirmados
 * @return true si el parseo fue exitoso, false si falló
 */
static bool parseAcousticConfirmation(const String &raw, uint16_t &confirmedBytes)
{
    String clean = raw;
    clean.replace("\r", "");
    clean.replace("\n", "");
    clean.trim();
    
    // Formato: $Bnn (donde nn es el número de bytes)
    if (!clean.startsWith("$B") || clean.length() < 3) {
        return false;
    }
    
    String byteCountStr = clean.substring(2);
    byteCountStr.trim();
    
    if (byteCountStr.length() == 0) {
        return false;
    }
    
    confirmedBytes = (uint16_t)byteCountStr.toInt();
    return true;
}

/**
 * Envía el mensaje formateado a través del modem acústico y espera confirmación.
 * Formato envío: $Bnnnddd...
 * Formato confirmación: $Bnn (donde nn es el número de bytes recibidos)
 * 
 * @param payload String con el payload (ej: "A500,B600,C700")
 * @return true si se envió y confirmó correctamente
 */
static bool sendAcousticMessage(const char *payload)
{
    uint16_t payloadLen = strlen(payload);
    
    // Verificar límite de 64 bytes
    if (payloadLen > 64 - 4) {  // Restar $Bnnn (4 chars)
        Serial.println("[ERROR] Payload demasiado grande (max 60 bytes)");
        return false;
    }
    
    // Construir mensaje: $Bnnnddd...
    char message[256];
    int len = snprintf(message, sizeof(message), "$B%d%s", payloadLen, payload);
    
    if (len < 0 || len >= (int)sizeof(message)) {
        Serial.println("[ERROR] Message buffer overflow");
        return false;
    }
    
    Serial.print("[Envío] Mensaje: ");
    Serial.print(message);
    Serial.print(" (total: ");
    Serial.print(len);
    Serial.println(" bytes)");
    
    // PASO 1: Enviar el mensaje por el canal acústico
    // Usar transactRaw para enviar y esperar confirmación de forma robusta
    String confirmation = transactRaw(message, 3000);
    
    // PASO 2: Esperar y validar confirmación
    Serial.print("[Confirmación] Respuesta: ");
    Serial.println(confirmation.isEmpty() ? "(vacía)" : confirmation);
    
    uint16_t confirmedBytes = 0;
    if (parseAcousticConfirmation(confirmation, confirmedBytes)) {
        // Verificar que coincida con lo que enviamos
        if (confirmedBytes == payloadLen) {
            Serial.print("[Confirmación] ✓ Éxito: ");
            Serial.print(confirmedBytes);
            Serial.println(" bytes confirmados por el modem");
            return true;
        } else {
            Serial.print("[Confirmación] ✗ Mismatch: enviamos ");
            Serial.print(payloadLen);
            Serial.print(" pero modem confirmó ");
            Serial.print(confirmedBytes);
            Serial.println(" bytes");
            return false;
        }
    } else {
        Serial.println("[Confirmación] ✗ No se recibió confirmación válida del modem");
        return false;
    }
}

/**
 * Procesa comandos remotos recibidos asincronamente del nodo de superficie.
 * Formato esperado: #U02EP (enable ping) o #U02DP (disable ping)
 * Donde:
 *   #U = unicast data received marker
 *   02 = longitud de datos (2 bytes)
 *   EP = enable ping (o DP = disable ping)
 * 
 * Estrategia: acumular líneas completas (hasta \n), luego procesar solo las que contienen #U
 */
static void processRemoteCommands()
{
    // Leer datos asincronos del modem
    while (SerialUser.available()) {
        char c = SerialUser.read();
        asyncBuffer += c;
    }

    // Buscar línea completa (terminada con \n o \r\n)
    int lineEnd = asyncBuffer.indexOf('\n');
    if (lineEnd < 0) {
        // Aún no tenemos línea completa
        return;
    }

    // Extraer la línea
    String line = asyncBuffer.substring(0, lineEnd);
    // Limpiar terminadores de línea
    line.trim();
    
    // Remover esta línea del buffer
    asyncBuffer.remove(0, lineEnd + 1);

    // Solo procesar líneas que contengan el marcador #U (unicast)
    if (line.indexOf("#U") < 0) {
        // No es un comando remoto, ignorar
        return;
    }

    // Parsear comando: #U02EP o #U02DP
    // Formato: #U<len><data>
    // Ejemplo: #U02EP (02 = 2 bytes de data, EP = payload)
    
    int uIdx = line.indexOf("#U");
    if (uIdx < 0 || line.length() < uIdx + 5) {
        // Formato inválido
        return;
    }

    // Extraer longitud (2 dígitos después de #U)
    String lenStr = line.substring(uIdx + 2, uIdx + 4);
    int dataLen = lenStr.toInt();
    
    if (dataLen != 2 || line.length() < uIdx + 6) {
        // Longitud inválida o no hay suficientes datos
        return;
    }

    // Extraer datos (2 caracteres después de la longitud)
    String dataStr = line.substring(uIdx + 4, uIdx + 6);
    
    if (dataStr == "EP") {
        // Enable ping: activar los pings
        Serial.println("\n═══════════════════════════════════════");
        Serial.println("[REMOTE CMD] ✓ Recibido: ENABLE PING");
        Serial.println("═══════════════════════════════════════\n");
        // La bandera pingEnabled ya está activa por defecto
        
    } else if (dataStr == "DP") {
        // Disable ping: desactivar los pings
        Serial.println("\n═══════════════════════════════════════");
        Serial.println("[REMOTE CMD] ✓ Recibido: DISABLE PING");
        Serial.println("═══════════════════════════════════════\n");
    } else {
        Serial.print("[REMOTE CMD] ✗ Comando desconocido: ");
        Serial.println(dataStr);
    }
}

// ============================================================================
// SETUP Y LOOP
// ============================================================================

void setup()
{
    Serial.begin(115200);  // Serial de debugging/consola
    delay(3000);
     Serial.println("\n\n");
    Serial.println("╔════════════════════════════════════════╗");
    Serial.println("║   SubmergedNode - Nodo Sumergido       ║");
    Serial.println("║   Firmware de Pings Acústicos          ║");
    Serial.println("╚════════════════════════════════════════╝\n");
    
    // Configurar el puerto serie del modem acústico
    Serial.println("[Setup] Inicializando puerto acústico (Serial2, 9600 baud)...");
    SerialUser.begin(USER_UART_BAUD, SERIAL_8N1, USER_UART_RXD_PIN, USER_UART_TXD_PIN);
    delay(500);
    configureAcousticModemAddress();
    
    Serial.println("[Setup] ✓ Listo");
    Serial.print("[Setup] Intervalo de ciclo de pings: ");
    Serial.print(AppConfig::PING_INTERVAL_MS / 1000);
    Serial.println(" segundos");
    Serial.print("[Setup] Timeout por ping: ");
    Serial.print(AppConfig::ACOUSTIC_TIMEOUT_MS / 1000);
    Serial.println(" segundos");
    Serial.print("[Setup] Direcciones objetivo: A=");
    Serial.print(AppConfig::ACOUSTIC_TARGET_A);
    Serial.print(", B=");
    Serial.print(AppConfig::ACOUSTIC_TARGET_B);
    Serial.print(", C=");
    Serial.println(AppConfig::ACOUSTIC_TARGET_C);
    
    // HEALTH CHECK del modem acústico
    Serial.println("\n[Setup] Iniciando health check del modem acústico...");
    bool modemOk = healthCheckAcousticModem();
    
    if (!modemOk) {
        Serial.println("\n⚠️  ADVERTENCIA: El modem acústico no respondió al health check");
        Serial.println("    El firmware continuará pero probablemente los pings fallarán.");
        Serial.println("    Verifica la conexión física y reinicia.\n");
    } else {
        Serial.println("\n✓ Health check completado exitosamente\n");
    }
    
    Serial.println("Esperando primer ciclo de pings...\n");
    
    
}

void loop()
{
    unsigned long now = millis();
    
    // Procesar comandos remotos del nodo de superficie (asincronamente)
    processRemoteCommands();
    
    // Ejecutar ciclo de pings cada PING_INTERVAL_MS
    if (now - lastPingCycle >= AppConfig::PING_INTERVAL_MS) {
        lastPingCycle = now;
        
        // 1. Hacer 3 pings secuenciales
        PingResult results[3];
        performPingCycle(results);
        
        // 2. Formatear payload
        char payload[256];
        uint16_t payloadLen = formatPayload(results, payload, sizeof(payload));
        
        if (payloadLen > 0) {
            // 3. Enviar por modem acústico
            sendAcousticMessage(payload);
        } else {
            Serial.println("[ERROR] No se pudo formatear el payload");
        }
        
        // Separator visual
        Serial.println("═══════════════════════════════════════\n");
    }
    
    // Dar tiempo para que se procesen otros eventos
    delay(100);
}
