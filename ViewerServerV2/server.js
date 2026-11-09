#!/usr/bin/env node

const express = require('express');
const axios = require('axios');
const cors = require('cors');
require('dotenv').config();
const path = require('path');
const os = require('os');

const app = express();
const PORT = process.env.PORT || 8080;

/**
 * Detecta la subred (primeros 3 octetos) de la primera interfaz IPv4 no interna
 */
function detectLocalSubnet() {
  const interfaces = os.networkInterfaces();
  for (const ifaceList of Object.values(interfaces)) {
    for (const iface of ifaceList) {
      if (iface.family === 'IPv4' && !iface.internal) {
        return iface.address.split('.').slice(0, 3).join('.');
      }
    }
  }
  return null;
}

const DETECTED_SUBNET = detectLocalSubnet();
const LAN_SUBNET = process.env.LAN_SUBNET || DETECTED_SUBNET || '192.168.0';
const POLL_INTERVAL = parseInt(process.env.POLL_INTERVAL) || 5000;
const NODE_TIMEOUT = parseInt(process.env.NODE_TIMEOUT) || 8000;
// El discovery lanza 255 peticiones en paralelo: necesita más margen que un refresh puntual
// antes de descartar una IP como "sin nodo".
const DISCOVER_TIMEOUT = parseInt(process.env.DISCOVER_TIMEOUT) || Math.max(NODE_TIMEOUT, 8000);

// Middleware
app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

// Estado global: nodos detectados y sus últimos datos
let nodes = {}; // { '192.168.1.10': { id: 'A', data: {...}, lastUpdate: timestamp, status: 'online'|'offline' } }
let discoveredNodes = []; // { ip, nodeId }

/**
 * Realiza un curl a un nodo para obtener su estado
 */
async function fetchNodeState(nodeIp, timeout = NODE_TIMEOUT) {
  try {
    const url = `http://${nodeIp}/api/state`;
    const response = await axios.get(url, { timeout });
    return response.data;
  } catch (error) {
    return null;
  }
}

/**
 * Envía un comando POST a un nodo
 */
async function sendNodeCommand(nodeIp, endpoint, params = {}) {
  try {
    const url = `http://${nodeIp}${endpoint}`;
    const response = await axios.post(url, null, { 
      params,
      timeout: NODE_TIMEOUT 
    });
    return { success: true, data: response.data };
  } catch (error) {
    return { success: false, error: error.message };
  }
}

/**
 * Descubre nodos en la LAN haciendo pings
 */
async function discoverNodes() {
  const startTime = Date.now();
  const stats = { found: 0, refused: 0, timeout: 0, other: 0 };
  console.log(`[DISCOVER] Escaneando ${LAN_SUBNET}.1-255 (timeout=${DISCOVER_TIMEOUT}ms)...`);

  const promises = [];

  // Barrido de IPs: 192.168.0.1 a 192.168.0.255
  for (let i = 1; i <= 255; i++) {
    const ip = `${LAN_SUBNET}.${i}`;
    promises.push(
      (async () => {
        try {
          const url = `http://${ip}/api/state`;
          const response = await axios.get(url, { timeout: DISCOVER_TIMEOUT });
          const data = response.data;
          if (data && data.nodeId) {
            stats.found++;
            console.log(`[DISCOVER] ✅ ${ip} → nodo ${data.nodeId}`);
            return { ip, nodeId: data.nodeId, data, status: 'online' };
          }
        } catch (error) {
          const code = error.code || error.message;
          if (code === 'ECONNREFUSED') {
            stats.refused++;
          } else if (code === 'ECONNABORTED' || code === 'ETIMEDOUT') {
            stats.timeout++;
            console.log(`[DISCOVER] ⏱ ${ip} → timeout`);
          } else {
            stats.other++;
            console.log(`[DISCOVER] ⚠️  ${ip} → ${code}`);
          }
        }
        return null;
      })()
    );
  }

  const results = await Promise.all(promises);
  const found = results.filter(r => r !== null);
  
  // Actualizar estado global
  discoveredNodes = found.map(n => ({ ip: n.ip, nodeId: n.nodeId }));
  
  for (const node of found) {
    nodes[node.ip] = {
      id: node.nodeId,
      data: node.data,
      lastUpdate: Date.now(),
      status: 'online'
    };
  }

  const elapsed = Date.now() - startTime;
  console.log(`[DISCOVER] Escaneo completo en ${elapsed}ms → encontrados=${stats.found} rechazados=${stats.refused} timeouts=${stats.timeout} otros=${stats.other}`);

  return discoveredNodes;
}

/**
 * Refresca el estado de todos los nodos conocidos
 */
async function refreshAllNodes() {
  const ips = Object.keys(nodes);
  
  for (const ip of ips) {
    try {
      const data = await fetchNodeState(ip);
      if (data) {
        nodes[ip].data = data;
        nodes[ip].lastUpdate = Date.now();
        nodes[ip].status = 'online';
      } else {
        nodes[ip].status = 'offline';
      }
    } catch (e) {
      nodes[ip].status = 'offline';
    }
  }
}

// ============================================================================
// RUTAS API
// ============================================================================

/**
 * GET /api/nodes
 * Retorna lista de nodos descubiertos con su último estado
 */
app.get('/api/nodes', (req, res) => {
  const nodesList = Object.entries(nodes).map(([ip, info]) => ({
    ip,
    nodeId: info.id,
    status: info.status,
    lastUpdate: info.lastUpdate,
    data: info.data
  }));
  
  res.json({
    nodes: nodesList,
    count: nodesList.length,
    lastDiscovery: Math.max(...Object.values(nodes).map(n => n.lastUpdate), 0)
  });
});

/**
 * POST /api/discover
 * Inicia un escaneo de la LAN para descubrir nodos
 */
app.post('/api/discover', async (req, res) => {
  res.json({ status: 'scanning', message: 'Descubrimiento iniciado...' });
  
  // Ejecutar discovery en background
  setTimeout(async () => {
    try {
      const found = await discoverNodes();
      console.log(`[DISCOVER] Encontrados ${found.length} nodos`);
    } catch (error) {
      console.error('[DISCOVER] Error:', error.message);
    }
  }, 100);
});

/**
 * POST /api/node/:nodeIp/command/:commandType
 * Envía un comando a un nodo específico
 */
app.post('/api/node/:nodeIp/command/:commandType', async (req, res) => {
  const { nodeIp, commandType } = req.params;
  const { action, value } = req.body;

  console.log(`[CMD] ${commandType} ${action} → ${nodeIp}`);

  let endpoint = '';
  let params = {};

  // Mapear tipos de comando a endpoints
  switch (commandType) {
    case 'gsm':
      endpoint = '/api/gsm';
      params.enabled = action === 'enable' ? 1 : 0;
      break;
    case 'ping':
      endpoint = '/api/ping';
      params.enabled = action === 'enable' ? 1 : 0;
      break;
    case 'submerged':
      endpoint = '/api/submerged';
      params.ping = action === 'enable' ? 'enable' : 'disable';
      break;
    case 'wifi':
      endpoint = '/api/wifi';
      Object.assign(params, value);
      break;
    default:
      console.error(`[CMD] ❌ Comando desconocido: ${commandType}`);
      return res.status(400).json({ error: 'Comando desconocido' });
  }

  try {
    console.log(`[CMD] → POST http://${nodeIp}${endpoint}?${new URLSearchParams(params).toString()}`);
    const result = await sendNodeCommand(nodeIp, endpoint, params);
    console.log(`[CMD] ✅ Respuesta:`, result);
    res.json(result);
  } catch (error) {
    console.error(`[CMD] ❌ Error:`, error.message);
    res.status(500).json({ error: error.message });
  }
});

/**
 * PUT /api/node/:nodeIp
 * Actualiza la configuración de un nodo (añade a lista si no existe)
 */
app.put('/api/node/:nodeIp', async (req, res) => {
  const { nodeIp } = req.params;
  
  try {
    const data = await fetchNodeState(nodeIp);
    if (data && data.nodeId) {
      nodes[nodeIp] = {
        id: data.nodeId,
        data,
        lastUpdate: Date.now(),
        status: 'online'
      };
      res.json({ success: true, node: nodes[nodeIp] });
    } else {
      res.status(404).json({ error: 'Nodo no encontrado en esa IP' });
    }
  } catch (error) {
    res.status(500).json({ error: error.message });
  }
});

/**
 * GET /api/node/:nodeIp/refresh
 * Refresca el estado de un nodo específico
 */
app.get('/api/node/:nodeIp/refresh', async (req, res) => {
  const { nodeIp } = req.params;
  
  try {
    const data = await fetchNodeState(nodeIp);
    if (data) {
      if (!nodes[nodeIp]) {
        nodes[nodeIp] = { id: data.nodeId };
      }
      nodes[nodeIp].data = data;
      nodes[nodeIp].lastUpdate = Date.now();
      nodes[nodeIp].status = 'online';
      res.json({ success: true, data });
    } else {
      res.status(500).json({ error: 'No se pudo obtener estado del nodo' });
    }
  } catch (error) {
    res.status(500).json({ error: error.message });
  }
});

/**
 * DELETE /api/node/:nodeIp
 * Elimina un nodo de la lista monitoreada
 */
app.delete('/api/node/:nodeIp', (req, res) => {
  const { nodeIp } = req.params;
  
  if (nodes[nodeIp]) {
    delete nodes[nodeIp];
    res.json({ success: true, message: 'Nodo eliminado' });
  } else {
    res.status(404).json({ error: 'Nodo no encontrado' });
  }
});

// ============================================================================
// POLLING AUTOMÁTICO
// ============================================================================

setInterval(async () => {
  if (Object.keys(nodes).length > 0) {
    await refreshAllNodes();
    console.log(`[POLL] Refrescados ${Object.keys(nodes).length} nodos`);
  }
}, POLL_INTERVAL);

// ============================================================================
// INICIO DEL SERVIDOR
// ============================================================================

app.listen(PORT, () => {
  console.log(`
╔══════════════════════════════════════════════════════════╗
║    VIEWER SERVER V2 - Acoustic Modem Control Panel       ║
╠══════════════════════════════════════════════════════════╣
║  🌐 Dashboard:  http://localhost:${PORT}
║  🔍 API:        http://localhost:${PORT}/api
  ║  🎯 LAN Subnet: ${LAN_SUBNET}.1-255 ${process.env.LAN_SUBNET ? '(desde .env)' : '(auto-detectada)'}
║  ⏱  Poll Interval: ${POLL_INTERVAL}ms
║  ⏱  Discover Timeout: ${DISCOVER_TIMEOUT}ms
╚══════════════════════════════════════════════════════════╝
  `);
});
