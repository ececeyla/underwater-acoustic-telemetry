(() => {
  "use strict";

  const NODE_COLORS = { A: "#e74c3c", B: "#2ecc71", C: "#3498db" };
  const TARGET_COLOR = "#9b59b6";
  const IMPORTED_COLOR = "#1abc9c";
  const POLL_MS = 2000;
  const EARTH_RADIUS_M = 6371000;

  const DEPTH_SOURCE_LABEL = { sensor: "sensor del vehículo", manual: "indicada manualmente" };
  function depthText(target) {
    if (target.depthM == null) return "desconocida";
    const source = DEPTH_SOURCE_LABEL[target.depthSource] || "";
    return `${target.depthM.toFixed(1)} m${source ? ` (${source})` : ""}`;
  }

  // ---------------------------------------------------------------------
  // Mapa 2D (Leaflet)
  // ---------------------------------------------------------------------
  const map = L.map("map").setView([0, 0], 2);
  L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
    attribution: "&copy; OpenStreetMap",
    maxZoom: 20,
  }).addTo(map);

  const mapLayers = { A: null, B: null, C: null, target: null, imported: null };
  let lastFitBounds = null;

  // Trayectoria importada por el usuario (Choose File): solo se dibuja, no se
  // compara ni se envía al servidor - vive únicamente en el navegador.
  let importedTrajectory = [];

  function renderMapImported() {
    if (mapLayers.imported) {
      map.removeLayer(mapLayers.imported);
      mapLayers.imported = null;
    }
    if (importedTrajectory.length === 0) return;
    mapLayers.imported = L.polyline(importedTrajectory.map((p) => [p.lat, p.lon]), {
      color: IMPORTED_COLOR, weight: 2, opacity: 0.8, dashArray: "6 4",
    })
      .bindPopup("Trayectoria importada")
      .addTo(map);
  }

  function renderMapNode(nodeId, node) {
    if (mapLayers[nodeId]) {
      map.removeLayer(mapLayers[nodeId]);
      mapLayers[nodeId] = null;
    }
    if (!node || !node.points || node.points.length === 0) return;

    const color = NODE_COLORS[nodeId] || "#999";
    const group = L.layerGroup();
    const latlngs = node.points.map((p) => [p.lat, p.lon]);

    L.polyline(latlngs, { color, weight: 3, opacity: 0.7 }).addTo(group);

    const last = node.lastFix;
    if (last) {
      L.circleMarker([last.lat, last.lon], {
        radius: 9,
        color: "#fff",
        weight: 2,
        fillColor: color,
        fillOpacity: 1,
      })
        .bindPopup(`Nodo ${nodeId}<br>${last.t}<br>lat ${last.lat.toFixed(6)}, lon ${last.lon.toFixed(6)}`)
        .addTo(group);
    }

    group.addTo(map);
    mapLayers[nodeId] = group;
  }

  function renderMapTarget(target) {
    if (mapLayers.target) {
      map.removeLayer(mapLayers.target);
      mapLayers.target = null;
    }
    if (!target || !target.available) return;

    const group = L.layerGroup();
    if (target.points && target.points.length > 0) {
      // NUEVO: Filtramos los puntos nulos para que Leaflet no explote
      const validPts = target.points.filter(p => p.lat !== null && p.lon !== null);
      if (validPts.length > 0) {
        L.polyline(validPts.map((p) => [p.lat, p.lon]), {
          color: TARGET_COLOR, weight: 3, opacity: 0.7, dashArray: "4 4",
        }).addTo(group);
      }
    }

    const icon = L.divIcon({
      className: "",
      html: `<div style="width:16px;height:16px;border-radius:50%;background:${TARGET_COLOR};border:2px solid #fff;box-shadow:0 0 8px ${TARGET_COLOR}"></div>`,
      iconSize: [16, 16],
      iconAnchor: [8, 8],
    });
    const depthTxt = depthText(target);
    L.marker([target.lat, target.lon], { icon })
      .bindPopup(`Vehículo sumergido (estimado)<br>profundidad: ${depthTxt}<br>anclas: ${target.usedAnchors.join(", ")}`)
      .addTo(group);

    group.addTo(map);
    mapLayers.target = group;
  }

  function maybeFitBounds(nodes, target) {
    const pts = [];
    Object.values(nodes).forEach((n) => {
      if (n && n.lastFix) pts.push([n.lastFix.lat, n.lastFix.lon]);
    });
    if (target && target.available) pts.push([target.lat, target.lon]);
    if (importedTrajectory.length > 0) {
      pts.push([importedTrajectory[0].lat, importedTrajectory[0].lon]);
      pts.push([importedTrajectory.at(-1).lat, importedTrajectory.at(-1).lon]);
    }
    if (pts.length === 0) return;

    // Solo reencuadra si hay algún punto fuera de lo que ya se ve (p. ej. el
    // objetivo sumergido aparece más tarde que A/B/C y a más distancia) - así
    // no se resetea el zoom/paneo manual del usuario en cada actualización.
    if (lastFitBounds && pts.every((p) => lastFitBounds.contains(p))) return;

    map.fitBounds(pts.length === 1 ? [pts[0], pts[0]] : pts, { maxZoom: 18, padding: [30, 30] });
    if (pts.length === 1) map.setZoom(18);
    lastFitBounds = map.getBounds();
  }

  // ---------------------------------------------------------------------
  // Escena 3D (Three.js) - coordenadas locales en metros (plano ENU)
  // ---------------------------------------------------------------------
  const scene3dEl = document.getElementById("scene3d");
  const scene = new THREE.Scene();
  scene.background = new THREE.Color(0x0b1220);

  const camera = new THREE.PerspectiveCamera(55, 1, 0.1, 5000);
  camera.position.set(40, -60, 40);
  camera.up.set(0, 0, 1);

  const renderer = new THREE.WebGLRenderer({ antialias: true });
  scene3dEl.appendChild(renderer.domElement);

  const controls = new THREE.OrbitControls(camera, renderer.domElement);
  controls.target.set(0, 0, 0);

  scene.add(new THREE.AmbientLight(0xffffff, 0.9));
  const grid = new THREE.GridHelper(200, 20, 0x2a3550, 0x1a2338);
  grid.rotation.x = Math.PI / 2; // el grid por defecto es horizontal en XZ; lo giramos al plano XY (superficie del mar)
  scene.add(grid);
  scene.add(new THREE.AxesHelper(15));

  function resize3d() {
    const w = scene3dEl.clientWidth || 1;
    const h = scene3dEl.clientHeight || 1;
    renderer.setSize(w, h);
    camera.aspect = w / h;
    camera.updateProjectionMatrix();
  }
  window.addEventListener("resize", resize3d);
  // El tamaño real del contenedor puede no estar listo todavía cuando se llama a
  // resize3d() por primera vez (layout flex aún resolviéndose) - un solo "resize"
  // de window no lo detecta, así que se observa el contenedor directamente.
  new ResizeObserver(resize3d).observe(scene3dEl);

  const node3dMeshes = {};
  function nodeMesh(color) {
    const mesh = new THREE.Mesh(
      new THREE.SphereGeometry(1, 16, 16),
      new THREE.MeshStandardMaterial({ color })
    );
    scene.add(mesh);
    return mesh;
  }
  Object.keys(NODE_COLORS).forEach((id) => {
    node3dMeshes[id] = nodeMesh(NODE_COLORS[id]);
    node3dMeshes[id].visible = false;
  });
  const targetMesh = new THREE.Mesh(
    new THREE.SphereGeometry(1.3, 16, 16),
    new THREE.MeshStandardMaterial({ color: TARGET_COLOR })
  );
  targetMesh.visible = false;
  scene.add(targetMesh);

  const node3dTracks = {};
  function trackLine(color) {
    const line = new THREE.Line(new THREE.BufferGeometry(), new THREE.LineBasicMaterial({ color, transparent: true, opacity: 0.7 }));
    line.visible = false;
    scene.add(line);
    return line;
  }
  Object.keys(NODE_COLORS).forEach((id) => { node3dTracks[id] = trackLine(NODE_COLORS[id]); });
  const targetTrack = trackLine(TARGET_COLOR);
  const importedTrack = trackLine(IMPORTED_COLOR);

  // Sustituye la geometría de una línea por una nueva a partir de puntos [x,y,z];
  // el número de puntos cambia en cada actualización, así que no se puede reusar.
  function updateTrackLine(line, points3d) {
    if (points3d.length < 2) {
      line.visible = false;
      return;
    }
    line.geometry.dispose();
    line.geometry = new THREE.BufferGeometry().setFromPoints(
      points3d.map(([x, y, z]) => new THREE.Vector3(x, y, z))
    );
    line.visible = true;
  }

  // Línea discontinua vertical para cuando se desconoce la profundidad real
  const depthLineGeom = new THREE.BufferGeometry().setFromPoints([new THREE.Vector3(), new THREE.Vector3()]);
  const depthLine = new THREE.Line(
    depthLineGeom,
    new THREE.LineDashedMaterial({ color: TARGET_COLOR, dashSize: 1, gapSize: 0.5 })
  );
  depthLine.visible = false;
  scene.add(depthLine);

  // Referencia local fija (se calcula una vez con los primeros datos, y se
  // reutiliza siempre para que la escena no “salte” entre peticiones).
  let ref = null; // {lat, lon}
  let camera3dCenter = null; // [cx, cy] del último encuadre
  let camera3dRadius = 0;
  function latLonToXY(lat, lon) {
    const x = ((lon - ref.lon) * Math.PI) / 180 * EARTH_RADIUS_M * Math.cos((ref.lat * Math.PI) / 180);
    const y = ((lat - ref.lat) * Math.PI) / 180 * EARTH_RADIUS_M;
    return [x, y];
  }

  // Reencuadra la cámara cuando aparecen puntos fuera de lo ya encuadrado (p. ej.
  // el objetivo sumergido llega más tarde que A/B/C y a mucha más distancia) -
  // así no se pierde de vista, pero tampoco se resetea la vista en cada tick.
  function frameCamera3d(points) {
    if (points.length === 0) return;
    const cx = points.reduce((s, p) => s + p[0], 0) / points.length;
    const cy = points.reduce((s, p) => s + p[1], 0) / points.length;
    const spread = Math.max(...points.map((p) => Math.hypot(p[0] - cx, p[1] - cy)), 5);

    if (
      camera3dCenter &&
      Math.hypot(cx - camera3dCenter[0], cy - camera3dCenter[1]) < camera3dRadius * 0.5 &&
      spread <= camera3dRadius
    ) {
      return; // todo sigue dentro de lo ya encuadrado, no tocar la cámara del usuario
    }

    const radius = spread * 1.6; // margen para que nada quede pegado al borde
    camera3dCenter = [cx, cy];
    camera3dRadius = radius;

    controls.target.set(cx, cy, 0);
    camera.position.set(cx + radius * 0.8, cy - radius * 1.1, radius * 0.9);
    camera.near = Math.max(radius / 500, 0.05);
    camera.far = radius * 50;
    camera.updateProjectionMatrix();

    grid.scale.setScalar(radius / 100);
    const markerScale = Math.max(radius / 40, 0.3);
    Object.values(node3dMeshes).forEach((m) => m.scale.setScalar(markerScale));
    targetMesh.scale.setScalar(markerScale);
  }

  function render3d(nodes, target) {
    if (!ref) {
      const first = Object.values(nodes).find((n) => n && n.lastFix);
      if (first) ref = { lat: first.lastFix.lat, lon: first.lastFix.lon };
      else if (target && target.available) ref = { lat: target.lat, lon: target.lon };
      else if (importedTrajectory.length > 0) ref = { lat: importedTrajectory[0].lat, lon: importedTrajectory[0].lon };
      else return; // todavía no hay ninguna posición conocida
    }

    const framePoints = [];

    Object.keys(NODE_COLORS).forEach((id) => {
      const n = nodes[id];
      const mesh = node3dMeshes[id];
      if (n && n.lastFix) {
        const [x, y] = latLonToXY(n.lastFix.lat, n.lastFix.lon);
        mesh.position.set(x, y, 0);
        mesh.visible = true;
        framePoints.push([x, y]);
      } else {
        mesh.visible = false;
      }

      const trackPoints = (n && n.points ? n.points : []).map((p) => {
        const [px, py] = latLonToXY(p.lat, p.lon);
        return [px, py, 0];
      });
      updateTrackLine(node3dTracks[id], trackPoints);
    });
    if (target && target.available) framePoints.push(latLonToXY(target.lat, target.lon));

    const targetTrackPoints = (target && target.points ? target.points : [])
      .filter((p) => p.lat !== null && p.lon !== null) // <-- NUEVO FILTRO
      .map((p) => {
        const [px, py] = latLonToXY(p.lat, p.lon);
        const pz = p.depthM != null ? -Math.abs(p.depthM) : 0;
        return [px, py, pz];
      });
    updateTrackLine(targetTrack, targetTrackPoints);

    const importedPoints = (importedTrajectory || []).map((p) => {
      const [px, py] = latLonToXY(p.lat, p.lon);
      const pz = p.depthM != null ? -Math.abs(p.depthM) : 0;
      return [px, py, pz];
    });
    updateTrackLine(importedTrack, importedPoints);
    if (importedPoints.length > 0) {
      framePoints.push([importedPoints[0][0], importedPoints[0][1]]);
      framePoints.push([importedPoints.at(-1)[0], importedPoints.at(-1)[1]]);
    }
    frameCamera3d(framePoints);
    if (target && target.available) {
      const [x, y] = latLonToXY(target.lat, target.lon);
      const z = target.depthM != null ? -Math.abs(target.depthM) : 0;
      targetMesh.position.set(x, y, z);
      targetMesh.visible = true;

      if (target.depthM == null) {
        // Profundidad desconocida: se dibuja en superficie con una línea
        // discontinua hacia abajo para indicar visualmente la incertidumbre.
        const positions = depthLineGeom.attributes.position;
        positions.setXYZ(0, x, y, 0);
        positions.setXYZ(1, x, y, -12);
        positions.needsUpdate = true;
        depthLine.computeLineDistances();
        depthLine.visible = true;
      } else {
        depthLine.visible = false;
      }
    } else {
      targetMesh.visible = false;
      depthLine.visible = false;
    }
  }

  function animate3d() {
    requestAnimationFrame(animate3d);
    controls.update();
    renderer.render(scene, camera);
  }

  // ---------------------------------------------------------------------
  // Tarjetas de estado + barra superior
  // ---------------------------------------------------------------------
  function fmtAgo(iso) {
    if (!iso) return "sin datos";
    const s = (Date.now() - new Date(iso).getTime()) / 1000;
    if (s < 60) return `hace ${Math.round(s)}s`;
    if (s < 3600) return `hace ${Math.round(s / 60)} min`;
    return `hace ${Math.round(s / 3600)} h`;
  }

  function renderCards(nodes, target) {
    Object.keys(NODE_COLORS).forEach((id) => {
      const card = document.getElementById(`card-${id}`);
      if (!card) return;
      const n = nodes[id];
      card.classList.toggle("stale", !n || n.stale);

      const statusEl = card.querySelector('[data-field="status"]');
      const posEl = card.querySelector('[data-field="pos"]');
      const pingEl = card.querySelector('[data-field="ping"]');

      if (!n || !n.lastReceivedAt) {
        statusEl.textContent = "sin datos";
        posEl.textContent = "—";
        pingEl.textContent = "—";
        return;
      }
      statusEl.textContent = `${n.stale ? "sin conexión" : "en línea"} (${fmtAgo(n.lastReceivedAt)})`;
      posEl.textContent = n.lastFix ? `${n.lastFix.lat.toFixed(6)}, ${n.lastFix.lon.toFixed(6)}` : "sin fix GPS";
      pingEl.textContent = n.lastFix && n.lastFix.pingOk
        ? `${n.lastFix.pingDistanceM.toFixed(2)} m`
        : "sin respuesta";
    });

    const targetCard = document.getElementById("card-target");
    const tPos = targetCard.querySelector('[data-field="pos"]');
    const tDepth = targetCard.querySelector('[data-field="depth"]');
    const tAnchors = targetCard.querySelector('[data-field="anchors"]');
    const badge = document.getElementById("target-status");

    if (target && target.available) {
      tPos.textContent = `${target.lat.toFixed(6)}, ${target.lon.toFixed(6)}`;
      tDepth.textContent = depthText(target);
      tAnchors.textContent = target.usedAnchors.join(", ");
      badge.textContent = "Objetivo sumergido: posición estimada";
      badge.className = "badge badge-ok";
    } else {
      tPos.textContent = "—";
      tDepth.textContent = "—";
      tAnchors.textContent = target ? (target.usedAnchors || []).join(", ") || "ninguna" : "—";
      const reasons = {
        no_recent_pings: "sin pings recientes",
        insufficient_anchors: "faltan nodos con ping+GPS simultáneos",
        geometry_degenerate: "nodos alineados, geometría no resoluble",
      };
      badge.textContent = `Objetivo sumergido: ${reasons[target && target.reason] || "sin datos"}`;
      badge.className = "badge badge-warn";
    }
  }

  // ---------------------------------------------------------------------
  // Controles (ventana de sincronización, rango máximo, descarga CSV)
  // ---------------------------------------------------------------------
  const syncEnabledEl = document.getElementById("sync-enabled");
  const syncSecondsEl = document.getElementById("sync-seconds");
  const maxRangeEl = document.getElementById("max-range");
  const knownDepthEl = document.getElementById("known-depth");
  const downloadCsvEl = document.getElementById("download-csv");

  function currentQueryParams() {
    const params = new URLSearchParams();
    params.set("syncWindow", syncEnabledEl.checked ? syncSecondsEl.value : "off");
    params.set("maxRange", maxRangeEl.value);
    if (knownDepthEl.value !== "") params.set("depth", knownDepthEl.value);
    return params;
  }

  function updateCsvLink() {
    downloadCsvEl.href = `/export.csv?${currentQueryParams().toString()}`;
  }
  [syncEnabledEl, syncSecondsEl, maxRangeEl, knownDepthEl].forEach((el) => {
    el.addEventListener("change", () => {
      updateCsvLink();
      tick();
    });
  });
  updateCsvLink();

  // ---------------------------------------------------------------------
  // Importar trayectoria (JSON local, solo se dibuja - no se compara ni se
  // envía al servidor). Acepta el mismo formato que target_trajectory.json
  // ({"samples": [{lat, lon, depth_m, ...}, ...]}) o un array plano de puntos.
  // ---------------------------------------------------------------------
  const importFileEl = document.getElementById("import-trajectory");
  const clearImportedEl = document.getElementById("clear-imported");
  const importedLegendEl = document.getElementById("imported-legend");

  function parseTrajectoryFile(text) {
    const data = JSON.parse(text);
    const samples = Array.isArray(data) ? data : (data.samples || []);
    return samples
      .map((s) => ({
        lat: typeof s.lat === "number" ? s.lat : null,
        lon: typeof s.lon === "number" ? s.lon : null,
        depthM: s.depth_m != null ? s.depth_m : (s.depthM != null ? s.depthM : null),
      }))
      .filter((p) => p.lat != null && p.lon != null);
  }

  importFileEl.addEventListener("change", async () => {
    const file = importFileEl.files[0];
    if (!file) return;
    try {
      const points = parseTrajectoryFile(await file.text());
      if (points.length === 0) throw new Error("el archivo no tiene puntos con lat/lon válidos");
      importedTrajectory = points;
      importedLegendEl.style.display = "";
      clearImportedEl.style.display = "";
      lastFitBounds = null; // fuerza reencuadrar el mapa para incluir la nueva trayectoria
      renderMapImported();
    } catch (err) {
      alert(`No se pudo importar el archivo: ${err.message}`);
    }
    importFileEl.value = "";
  });

  clearImportedEl.addEventListener("click", (ev) => {
    ev.preventDefault();
    importedTrajectory = [];
    importedLegendEl.style.display = "none";
    clearImportedEl.style.display = "none";
    renderMapImported();
  });

  // ---------------------------------------------------------------------
  // Bucle de actualización
  // ---------------------------------------------------------------------
  
  // ---------------------------------------------------------------------
  // Tabla de Tríadas
  // ---------------------------------------------------------------------
  function renderTable(target) {
    const tbody = document.querySelector("#triads-table tbody");
    if (!target || !target.points || target.points.length === 0) {
      tbody.innerHTML = '<tr><td colspan="7" style="text-align:center">Sin datos válidos</td></tr>';
      return;
    }

    // Cortamos a los últimos 50 puntos y le damos la vuelta para ver los más nuevos arriba
    const recentPoints = target.points.slice(-50).reverse();

    const formatTime = (isoStr) => {
      if (!isoStr) return "—";
      return new Date(isoStr).toISOString().split('T')[1].substring(0, 8); // Extrae HH:MM:SS
    };

    const rows = recentPoints.map(p => {
      // Extraemos tiempos (si el nodo no está en anchorsT, será nulo)
      const tA = p.anchorsT && p.anchorsT['A'] ? new Date(p.anchorsT['A']) : null;
      const tB = p.anchorsT && p.anchorsT['B'] ? new Date(p.anchorsT['B']) : null;
      const tC = p.anchorsT && p.anchorsT['C'] ? new Date(p.anchorsT['C']) : null;
      
      const times = [tA, tB, tC].filter(t => t !== null);
      let deltaStr = "—";
      let deltaColor = "var(--text)";

      // Calculamos la diferencia entre la marca más antigua y la más nueva
      if (times.length > 0) {
        const maxT = Math.max(...times);
        const minT = Math.min(...times);
        const deltaS = (maxT - minT) / 1000;
        deltaStr = deltaS.toFixed(2);
        
        // Si el delta se acerca al límite, lo pintamos de naranja/rojo como aviso visual
        const limit = parseFloat(syncSecondsEl.value);
        if (syncEnabledEl.checked && deltaS > limit * 0.8) deltaColor = "#f0b35c";
      }

      // Detectamos si la triangulación fue imposible (coordenadas nulas)
      const isValid = p.lat !== null && p.lon !== null;
      
      // Si no es válido, pintamos la fila de rojo tenue
      const rowStyle = isValid ? "" : 'style="color: #e74c3c; background: rgba(231, 76, 60, 0.1);" title="Geometría imposible"';
      
      // En vez de coordenadas, mostramos "Error"
      const latStr = isValid ? p.lat.toFixed(6) : "Error físico";
      const lonStr = isValid ? p.lon.toFixed(6) : "Error físico";

      return `<tr ${rowStyle}>
        <td>${formatTime(p.t)}</td>
        <td>${latStr}</td>
        <td>${lonStr}</td>
        <td>${p.anchorsT && p.anchorsT['A'] ? formatTime(p.anchorsT['A']) : "—"}</td>
        <td>${p.anchorsT && p.anchorsT['B'] ? formatTime(p.anchorsT['B']) : "—"}</td>
        <td>${p.anchorsT && p.anchorsT['C'] ? formatTime(p.anchorsT['C']) : "—"}</td>
        <td style="color:${deltaColor}"><strong>${deltaStr}</strong></td>
      </tr>`; 
    });

    tbody.innerHTML = rows.join('');
  }
  
  async function  tick() {
    try {
      const res = await fetch(`/api/tracks?${currentQueryParams().toString()}`, { cache: "no-store" });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const data = await res.json();

      document.getElementById("last-update").textContent =
        `Última actualización: ${new Date(data.generatedAt).toLocaleTimeString()}`;

      Object.keys(NODE_COLORS).forEach((id) => renderMapNode(id, data.nodes[id]));
      renderMapTarget(data.target);
      renderMapImported();
      maybeFitBounds(data.nodes, data.target);
      render3d(data.nodes, data.target);
      renderCards(data.nodes, data.target);
      renderTable(data.target);
    } catch (err) {
      document.getElementById("last-update").textContent = `Error al leer /api/tracks: ${err.message}`;
    }
  }

  resize3d();
  animate3d();
  tick();
  setInterval(tick, POLL_MS);
})();
