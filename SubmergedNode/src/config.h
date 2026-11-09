#pragma once

namespace AppConfig {

  // Identidad del nodo sumergido
  inline constexpr const char *NODE_ID = "SUBMERGED";

  // Direcciones acústicas de los 3 nodos de superficie
  // El nodo sumergido hace pings secuenciales a estos 3 objetivos
  inline constexpr uint16_t ACOUSTIC_TARGET_A = 111;  // Nodo A
  inline constexpr uint16_t ACOUSTIC_TARGET_B = 222;  // Nodo B
  inline constexpr uint16_t ACOUSTIC_TARGET_C = 333;  // Nodo C

  // Timeout y intervalo de ping acústico
  inline constexpr uint32_t ACOUSTIC_TIMEOUT_MS = 5000;   // Esperar respuesta max 5s por ping
  inline constexpr uint32_t PING_INTERVAL_MS = 30000;     // Cada 30s hace 3 pings y envía el mensaje
  inline constexpr uint32_t DELAY_BETWEEN_PINGS_MS = 1000; // Pequeña pausa entre pings para evitar colisiones

  // Velocidad del sonido en agua de mar (~1500 m/s)
  // Usado para convertir RTT (round-trip time en ms) a distancia
  inline constexpr float SPEED_OF_SOUND_M_PER_S = 1500.0f;

  // Dirección acústica del nodo sumergido para recibir respuestas
  // (aunque es asimétrico, el código actual no parece usarlo explícitamente)
  inline constexpr uint16_t SUBMERGED_ADDRESS = 123;

  // Máximo de intentos por ping en caso de timeout
  inline constexpr uint8_t MAX_PING_RETRIES = 1;

}
