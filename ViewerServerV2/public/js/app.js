/**
 * Viewer Server V2 - Frontend Application
 * Dashboard para monitorización y control de nodos acústicos
 */

// CONFIGURACIÓN
const API_BASE = 'http://localhost:8080';
const REFRESH_INTERVAL = 5000; // 5 segundos
const DISCOVER_DEBOUNCE = 30000; // No descubrir más de una vez cada 30s

// ESTADO GLOBAL
let nodesList = {};
let lastDiscovery = 0;
let refreshTimer = null;
let lastRequestInfo = {}; // { nodeIp: {cmd, endpoint, params, response, timestamp} }

// ============================================================================
// UTILIDADES DE API
// ============================================================================

/**
 * Realiza un GET a la API
 */
async function apiGet(endpoint) {
    try {
        const response = await fetch(`${API_BASE}/api${endpoint}`);
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        return await response.json();
    } catch (error) {
        console.error(`API GET ${endpoint}:`, error);
        throw error;
    }
}

/**
 * Realiza un POST a la API
 */
async function apiPost(endpoint, body = null) {
    try {
        const response = await fetch(`${API_BASE}/api${endpoint}`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: body ? JSON.stringify(body) : null
        });
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        return await response.json();
    } catch (error) {
        console.error(`API POST ${endpoint}:`, error);
        throw error;
    }
}

/**
 * Realiza un PUT a la API
 */
async function apiPut(endpoint, body = null) {
    try {
        const response = await fetch(`${API_BASE}/api${endpoint}`, {
            method: 'PUT',
            headers: { 'Content-Type': 'application/json' },
            body: body ? JSON.stringify(body) : null
        });
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        return await response.json();
    } catch (error) {
        console.error(`API PUT ${endpoint}:`, error);
        throw error;
    }
}

/**
 * Realiza un DELETE a la API
 */
async function apiDelete(endpoint) {
    try {
        const response = await fetch(`${API_BASE}/api${endpoint}`, {
            method: 'DELETE'
        });
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        return await response.json();
    } catch (error) {
        console.error(`API DELETE ${endpoint}:`, error);
        throw error;
    }
}

// ============================================================================
// FUNCIONES DE NEGOCIO
// ============================================================================

/**
 * Descubre nodos en la LAN
 */
async function discoverNodes() {
    const now = Date.now();
    if (now - lastDiscovery < DISCOVER_DEBOUNCE) {
        console.log('⏳ Descubrimiento en progreso, espera...');
        return;
    }
    
    lastDiscovery = now;
    updateStatus('🔍 Descubriendo nodos...', 'scanning');
    
    try {
        await apiPost('/discover');
        // Dar tiempo a que el servidor escanee
        setTimeout(() => {
            refreshNodesList();
        }, 2000);
    } catch (error) {
        updateStatus('❌ Error en descubrimiento', 'error');
    }
}

/**
 * Refresca la lista de nodos desde el servidor
 */
async function refreshNodesList() {
    try {
        const result = await apiGet('/nodes');
        nodesList = {};
        
        for (const node of result.nodes) {
            nodesList[node.ip] = node;
        }
        
        renderNodes();
        updateStatus(`✅ ${result.count} nodos detectados`, 'ok');
    } catch (error) {
        updateStatus('❌ Error al obtener lista de nodos', 'error');
    }
}

/**
 * Refresca el estado de todos los nodos
 */
async function refreshAllNodes() {
    if (Object.keys(nodesList).length === 0) return;
    
    try {
        const result = await apiGet('/nodes');
        nodesList = {};
        
        for (const node of result.nodes) {
            nodesList[node.ip] = node;
        }
        
        renderNodes();
    } catch (error) {
        console.error('Error refrescando nodos:', error);
    }
}

/**
 * Añade un nodo manualmente
 */
async function addNodeManually(ip) {
    try {
        const result = await apiPut(`/node/${ip}`);
        nodesList[ip] = result.node;
        renderNodes();
        showModal(false);
        updateStatus(`✅ Nodo ${ip} añadido`, 'ok');
    } catch (error) {
        alert(`Error: ${error.message}`);
    }
}

/**
 * Elimina un nodo de la lista
 */
async function removeNode(ip) {
    if (!confirm(`¿Eliminar nodo ${ip}?`)) return;
    
    try {
        await apiDelete(`/node/${ip}`);
        delete nodesList[ip];
        renderNodes();
        updateStatus('✅ Nodo eliminado', 'ok');
    } catch (error) {
        alert(`Error: ${error.message}`);
    }
}

/**
 * Envía un comando a un nodo
 */
async function sendCommand(nodeIp, commandType, action, value = null) {
    console.log(`[COMANDO] Enviando: ${commandType}/${action} a ${nodeIp}`);
    
    try {
        const body = { action };
        if (value) body.value = value;
        
        const endpoint = `/node/${nodeIp}/command/${commandType}`;
        console.log(`[COMANDO] POST ${endpoint}`, body);
        
        // Guardar información de la solicitud
        const curlCmd = `curl -X POST http://localhost:8080/api${endpoint} -H "Content-Type: application/json" -d '${JSON.stringify(body)}'`;
        
        const result = await apiPost(endpoint, body);
        console.log(`[COMANDO] Respuesta:`, result);
        
        // Guardar en historial
        lastRequestInfo[nodeIp] = {
            cmd: curlCmd,
            endpoint: endpoint,
            params: body,
            response: result,
            timestamp: new Date().toLocaleTimeString()
        };
        
        if (result.success) {
            updateStatus(`✅ ${commandType.toUpperCase()} ${action}: OK`, 'ok');
            console.log(`[COMANDO] ✅ Éxito`);
            
            // Actualizar datos locales inmediatamente si es respuesta de comando
            if (nodesList[nodeIp] && result.data) {
                // Mapear campos de respuesta a datos locales
                if (result.data.gsmFeatureEnabled !== undefined) {
                    if (!nodesList[nodeIp].data) nodesList[nodeIp].data = {};
                    nodesList[nodeIp].data.gsmFeatureEnabled = result.data.gsmFeatureEnabled;
                }
                if (result.data.acousticPingEnabled !== undefined) {
                    if (!nodesList[nodeIp].data) nodesList[nodeIp].data = {};
                    nodesList[nodeIp].data.acousticPingEnabled = result.data.acousticPingEnabled;
                }
                // Re-renderizar inmediatamente
                renderNodes();
            }
            
            // Refrescar agresivamente después
            setTimeout(() => {
                refreshNodeDataWithRetries(nodeIp, 3);
            }, 500);
        } else {
            updateStatus(`❌ Error: ${result.error}`, 'error');
            console.error(`[COMANDO] ❌ Error:`, result.error);
        }
    } catch (error) {
        updateStatus(`❌ Error al enviar comando: ${error.message}`, 'error');
        console.error(`[COMANDO] ❌ Excepción:`, error);
    }
}

/**
 * Refresca los datos de un nodo con reintentos automáticos
 */
async function refreshNodeDataWithRetries(nodeIp, maxRetries = 3) {
    for (let attempt = 1; attempt <= maxRetries; attempt++) {
        try {
            console.log(`[REFRESH] Intento ${attempt}/${maxRetries} para ${nodeIp}`);
            const result = await apiGet(`/node/${nodeIp}/refresh`);
            if (nodesList[nodeIp]) {
                nodesList[nodeIp].data = result.data;
                nodesList[nodeIp].lastUpdate = Date.now();
                renderNodes();
                console.log(`[REFRESH] ✅ Actualizado en intento ${attempt}`);
                return; // Éxito, salir
            }
        } catch (error) {
            console.error(`[REFRESH] Error en intento ${attempt}:`, error);
            if (attempt < maxRetries) {
                // Esperar antes del siguiente intento
                await new Promise(resolve => setTimeout(resolve, 1000));
            }
        }
    }
    console.error(`[REFRESH] ❌ Falló después de ${maxRetries} intentos`);
}

/**
 * Refresca los datos de un nodo específico
 */
async function refreshNodeData(nodeIp) {
    try {
        const result = await apiGet(`/node/${nodeIp}/refresh`);
        if (nodesList[nodeIp]) {
            nodesList[nodeIp].data = result.data;
            renderNodes();
        }
    } catch (error) {
        console.error(`Error refrescando ${nodeIp}:`, error);
    }
}

// ============================================================================
// RENDERIZADO
// ============================================================================

/**
 * Actualiza el estado general en la barra
 */
function updateStatus(text, type = 'info') {
    const statusText = document.getElementById('statusText');
    const statusCount = document.getElementById('statusCount');
    
    statusText.textContent = text;
    statusCount.textContent = `${Object.keys(nodesList).length} nodos`;
    
    if (Object.keys(nodesList).length === 0) {
        document.getElementById('emptyState').style.display = 'block';
        document.getElementById('nodesList').style.display = 'none';
    } else {
        document.getElementById('emptyState').style.display = 'none';
        document.getElementById('nodesList').style.display = 'grid';
    }
}

/**
 * Renderiza todas las tarjetas de nodos
 */
function renderNodes() {
    const container = document.getElementById('nodesList');
    container.innerHTML = '';
    
    for (const [ip, node] of Object.entries(nodesList)) {
        const card = createNodeCard(ip, node);
        container.appendChild(card);
    }
    
    updateStatus(`✅ ${Object.keys(nodesList).length} nodos`, 'ok');
}

/**
 * Crea una tarjeta HTML para un nodo
 */
function createNodeCard(ip, node) {
    const card = document.createElement('div');
    card.className = `node-card ${node.status === 'offline' ? 'offline' : ''}`;
    
    const data = node.data || {};
    const online = node.status === 'online';
    
    // Extrae datos del JSON de estado
    const gpsFix = data.gpsFix ? '✅' : '❌';
    const gsmReg = data.gsmRegistered ? '✅' : '❌';
    const pingOk = data.pingOk ? '✅' : '❌';
    const modemHealthy = data.acousticModemHealthy ? '✅' : '❌';
    
    const rttMs = data.pingRttMs || '—';
    const latitude = data.latitude ? data.latitude.toFixed(6) : '—';
    const longitude = data.longitude ? data.longitude.toFixed(6) : '—';
    const modemAddress = data.acousticModemAddress || '—';
    const modemVoltage = data.acousticModemVoltageV ? data.acousticModemVoltageV.toFixed(2) : '—';
    
    // Estado del ping acústico remoto (submerged)
    const submergedOk = data.submergedBroadcastOk ? '✅' : '❌';
    const submergedRttA = data.submergedRttA_ms >= 0 ? `${data.submergedRttA_ms}ms` : '—';
    const submergedRttB = data.submergedRttB_ms >= 0 ? `${data.submergedRttB_ms}ms` : '—';
    const submergedRttC = data.submergedRttC_ms >= 0 ? `${data.submergedRttC_ms}ms` : '—';
    
    const lastUpdate = new Date(node.lastUpdate).toLocaleTimeString();
    
    card.innerHTML = `
        <!-- ENCABEZADO -->
        <div class="node-header">
            <div class="node-title">
                <h2>Nodo ${node.nodeId || '?'}</h2>
            </div>
            <div class="node-actions">
                <button class="btn btn-secondary btn-small" onclick="refreshNodeData('${ip}')">🔄</button>
                <button class="btn btn-danger btn-small" onclick="removeNode('${ip}')">🗑️</button>
            </div>
        </div>

        <!-- ESTADO DEL NODO -->
        <div class="node-ip">IP: <span style="font-weight: 600;">${ip}</span></div>
        <div style="margin-top: 8px;">
            <span class="node-status ${node.status}">
                ${node.status === 'online' ? '🟢 En línea' : '🔴 Desconectado'}
            </span>
            <span style="font-size: 11px; color: #999; margin-left: 12px;">
                Actualizado: ${lastUpdate}
            </span>
        </div>

        <!-- SECCIÓN GPS -->
        <div class="data-section">
            <div class="section-title">📍 GPS / UBICACIÓN</div>
            <div class="data-row">
                <div class="data-item">
                    <span class="data-label">Fix GPS</span>
                    <span class="data-value ${data.gpsFix ? 'good' : 'bad'}">${gpsFix}</span>
                </div>
                <div class="data-item">
                    <span class="data-label">Latitud</span>
                    <span class="data-value">${latitude}</span>
                </div>
            </div>
            <div class="data-row">
                <div class="data-item">
                    <span class="data-label">Longitud</span>
                    <span class="data-value">${longitude}</span>
                </div>
                <div class="data-item">
                    <span class="data-label">Timestamp</span>
                    <span class="data-value" style="font-size: 11px;">${data.gpsTimestamp || '—'}</span>
                </div>
            </div>
        </div>

        <!-- SECCIÓN GSM / MÓDEM CELULAR -->
        <div class="data-section">
            <div class="section-title">📡 GSM / MÓDEM CELULAR</div>
            <div class="data-row">
                <div class="data-item">
                    <span class="data-label">Registrado</span>
                    <span class="data-value ${data.gsmRegistered ? 'good' : 'bad'}">${gsmReg}</span>
                </div>
                <div class="data-item">
                    <span class="data-label">Señal (RSSI)</span>
                    <span class="data-value">${data.gsmSignalRssi || '—'}</span>
                </div>
            </div>
            <div class="data-row">
                <div class="data-item">
                    <span class="data-label">Datos</span>
                    <span class="data-value ${data.gsmDataConnected ? 'good' : 'bad'}">
                        ${data.gsmDataConnected ? 'Conectado' : 'Desconectado'}
                    </span>
                </div>
                <div class="data-item">
                    <span class="data-label">IP</span>
                    <span class="data-value" style="font-size: 11px;">${data.gsmIp || '—'}</span>
                </div>
            </div>
        </div>

        <!-- SECCIÓN PING ACÚSTICO LOCAL -->
        <div class="data-section">
            <div class="section-title">🔊 PING ACÚSTICO (Local)</div>
            <div class="data-row">
                <div class="data-item">
                    <span class="data-label">Ping</span>
                    <span class="data-value ${data.pingOk ? 'good' : 'bad'}">${pingOk}</span>
                </div>
                <div class="data-item">
                    <span class="data-label">RTT</span>
                    <span class="data-value">${rttMs} ms</span>
                </div>
            </div>
        </div>

        <!-- SECCIÓN MODEM ACÚSTICO -->
        <div class="data-section">
            <div class="section-title">🎚️ MÓDEM ACÚSTICO</div>
            <div class="data-row">
                <div class="data-item">
                    <span class="data-label">Salud</span>
                    <span class="data-value ${data.acousticModemHealthy ? 'good' : 'bad'}">${modemHealthy}</span>
                </div>
                <div class="data-item">
                    <span class="data-label">Dirección</span>
                    <span class="data-value">${modemAddress}</span>
                </div>
            </div>
            <div class="data-row">
                <div class="data-item">
                    <span class="data-label">Voltaje</span>
                    <span class="data-value">${modemVoltage}V</span>
                </div>
            </div>
        </div>

        <!-- SECCIÓN NODO SUMERGIDO -->
        <div class="data-section">
            <div class="section-title">🌊 NODO SUMERGIDO (Broadcast)</div>
            <div class="data-row">
                <div class="data-item">
                    <span class="data-label">RX OK</span>
                    <span class="data-value ${data.submergedBroadcastOk ? 'good' : 'bad'}">${submergedOk}</span>
                </div>
                <div class="data-item">
                    <span class="data-label">Dirección</span>
                    <span class="data-value">${data.submergedSourceAddress || '—'}</span>
                </div>
            </div>
            <div class="data-row">
                <div class="data-item">
                    <span class="data-label">RTT A</span>
                    <span class="data-value">${submergedRttA}</span>
                </div>
                <div class="data-item">
                    <span class="data-label">RTT B</span>
                    <span class="data-value">${submergedRttB}</span>
                </div>
            </div>
            <div class="data-row">
                <div class="data-item">
                    <span class="data-label">RTT C</span>
                    <span class="data-value">${submergedRttC}</span>
                </div>
                <div class="data-item">
                    <span class="data-label">Distancia A</span>
                    <span class="data-value">${data.submergedDistanceA_m >= 0 ? data.submergedDistanceA_m.toFixed(1) + 'm' : '—'}</span>
                </div>
            </div>
        </div>

        <!-- ESTADOS LOCALES -->
        <div class="data-section">
            <div class="section-title">🎚️ ESTADOS LOCALES</div>
            <div class="data-row">
                <div class="data-item">
                    <span class="data-label">GSM Habilitado</span>
                    <span class="data-value ${data.gsmFeatureEnabled === null || data.gsmFeatureEnabled === undefined ? 'warning' : (data.gsmFeatureEnabled ? 'good' : 'bad')}">
                        ${data.gsmFeatureEnabled === null || data.gsmFeatureEnabled === undefined ? '❓ NO DATA' : (data.gsmFeatureEnabled ? '✅ ENABLE' : '❌ DISABLE')}
                    </span>
                </div>
                <div class="data-item">
                    <span class="data-label">Ping Acústico</span>
                    <span class="data-value ${data.acousticPingEnabled === null || data.acousticPingEnabled === undefined ? 'warning' : (data.acousticPingEnabled ? 'good' : 'bad')}">
                        ${data.acousticPingEnabled === null || data.acousticPingEnabled === undefined ? '❓ NO DATA' : (data.acousticPingEnabled ? '✅ ENABLE' : '❌ DISABLE')}
                    </span>
                </div>
            </div>
        </div>

        <!-- CONTROL REMOTO -->
        <div class="data-section">
            <div class="section-title">🎮 CONTROL REMOTO</div>
            <div class="data-row">
                <div class="data-item">
                    <span class="data-label">Último comando</span>
                    <span class="data-value">${data.lastRemoteCommand || '—'}</span>
                </div>
                <div class="data-item">
                    <span class="data-label">Estado</span>
                    <span class="data-value">
                        ${data.lastRemoteCommandStatus === 2 ? '✅ ACK' : data.lastRemoteCommandStatus === 1 ? '⏳ Enviado' : '—'}
                    </span>
                </div>
            </div>
        </div>

        <!-- CONTROLES -->
        <div class="controls-group">
            <h3>⚙️ Controles</h3>
            
            <div style="margin-bottom: 12px;">
                <p style="font-size: 12px; color: #666; margin-bottom: 8px;">GSM/Módem Celular:</p>
                <div class="control-buttons">
                    <button class="btn btn-success btn-small cmd-btn" data-ip="${ip}" data-cmd="gsm" data-action="enable">✓ Activar GSM</button>
                    <button class="btn btn-danger btn-small cmd-btn" data-ip="${ip}" data-cmd="gsm" data-action="disable">✕ Desactivar GSM</button>
                </div>
            </div>

            <div style="margin-bottom: 12px;">
                <p style="font-size: 12px; color: #666; margin-bottom: 8px;">Ping Acústico:</p>
                <div class="control-buttons">
                    <button class="btn btn-success btn-small cmd-btn" data-ip="${ip}" data-cmd="ping" data-action="enable">✓ Activar Ping</button>
                    <button class="btn btn-danger btn-small cmd-btn" data-ip="${ip}" data-cmd="ping" data-action="disable">✕ Desactivar Ping</button>
                </div>
            </div>

            <div style="margin-bottom: 12px;">
                <p style="font-size: 12px; color: #666; margin-bottom: 8px;">Control Remoto (Nodo Sumergido):</p>
                <div class="control-buttons">
                    <button class="btn btn-success btn-small cmd-btn" data-ip="${ip}" data-cmd="submerged" data-action="enable">✓ Habilitar</button>
                    <button class="btn btn-danger btn-small cmd-btn" data-ip="${ip}" data-cmd="submerged" data-action="disable">✕ Deshabilitar</button>
                </div>
            </div>
        </div>

        <!-- DEBUG: ÚLTIMAS SOLICITUDES HTTP -->
        <div class="data-section" style="background: #f0f8ff; border-left: 4px solid #ff6600;">
            <div class="section-title">🐛 DEBUG - Últimas Solicitudes</div>
            <div style="font-family: monospace; font-size: 10px; line-height: 1.4; background: #fff; padding: 8px; border-radius: 4px; max-height: 250px; overflow-y: auto; border: 1px solid #ddd;">
                ${lastRequestInfo[ip] ? `
                    <div style="margin-bottom: 8px;">
                        <div style="color: #d32f2f; font-weight: bold;">📤 CURL / POST enviado:</div>
                        <div style="color: #1976d2; word-break: break-all; background: #f5f5f5; padding: 4px; border-radius: 3px; margin-top: 4px;">
                            ${lastRequestInfo[ip].cmd}
                        </div>
                    </div>
                    <div style="margin-bottom: 8px;">
                        <div style="color: #388e3c; font-weight: bold;">📥 Respuesta JSON del servidor:</div>
                        <div style="color: #333; word-break: break-all; background: #f5f5f5; padding: 4px; border-radius: 3px; margin-top: 4px; max-height: 120px; overflow-y: auto;">
                            <pre style="margin: 0; font-size: 9px;">${JSON.stringify(lastRequestInfo[ip].response, null, 2)}</pre>
                        </div>
                    </div>
                    <div>
                        <div style="color: #666; font-size: 10px;">⏱ ${lastRequestInfo[ip].timestamp}</div>
                    </div>
                ` : `
                    <div style="color: #999;">
                        Ninguna solicitud enviada aún desde el dashboard
                    </div>
                `}
            </div>
        </div>
    `;
    
    return card;
}

// ============================================================================
// INTERFAZ Y EVENTOS
// ============================================================================

/**
 * Muestra/oculta el modal de añadir nodo
 */
function showModal(show) {
    const modal = document.getElementById('modalAddNode');
    modal.style.display = show ? 'flex' : 'none';
}

/**
 * Inicializa los event listeners
 */
function initEventListeners() {
    // Descubrimiento
    document.getElementById('btnDiscover').addEventListener('click', () => {
        discoverNodes();
    });
    
    // Añadir nodo manual
    document.getElementById('btnAddManual').addEventListener('click', () => {
        showModal(true);
    });
    
    // Modal controls
    document.getElementById('btnCloseModal').addEventListener('click', () => {
        showModal(false);
    });
    
    document.getElementById('btnCancelModal').addEventListener('click', () => {
        showModal(false);
    });
    
    document.getElementById('btnSaveNode').addEventListener('click', () => {
        const ip = document.getElementById('inputNodeIp').value.trim();
        if (ip) {
            addNodeManually(ip);
            document.getElementById('inputNodeIp').value = '';
        } else {
            alert('Por favor ingresa una IP');
        }
    });
    
    // Enter en input del modal
    document.getElementById('inputNodeIp').addEventListener('keypress', (e) => {
        if (e.key === 'Enter') {
            document.getElementById('btnSaveNode').click();
        }
    });
    
    // Delegación de eventos para botones de comando
    document.addEventListener('click', (e) => {
        if (e.target.classList.contains('cmd-btn')) {
            const ip = e.target.dataset.ip;
            const cmd = e.target.dataset.cmd;
            const action = e.target.dataset.action;
            console.log(`[BOTÓN] ${cmd} ${action} en ${ip}`);
            sendCommand(ip, cmd, action);
        }
    });
}

// ============================================================================
// INICIALIZACIÓN
// ============================================================================

document.addEventListener('DOMContentLoaded', () => {
    initEventListeners();
    updateStatus('🚀 Iniciando...', 'info');
    
    // Cargar nodos
    setTimeout(() => {
        refreshNodesList();
    }, 500);
    
    // Polling automático
    refreshTimer = setInterval(() => {
        refreshAllNodes();
    }, REFRESH_INTERVAL);
});

// Cleanup al cerrar la página
window.addEventListener('beforeunload', () => {
    if (refreshTimer) clearInterval(refreshTimer);
});
