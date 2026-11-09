#!/usr/bin/env python3
"""Web viewer (mapa 2D + escena 3D) para los logs de los nodos de superficie.

Lee server/logs/<NODE>.log (generados por telemetry_server.py) y expone:
  - GET /                 -> página web (static/index.html)
  - GET /viewer.js, /viewer.css -> assets estáticos
  - GET /api/tracks       -> JSON con la trayectoria de cada nodo y, si hay
                             datos recientes de al menos 3 nodos, la posición
                             triangulada del dispositivo sumergido.

El navegador consulta /api/tracks cada pocos segundos (ver viewer.js) y
redibuja el mapa y la escena 3D con los datos nuevos.

Uso:
    python3 viewer_server.py --host 0.0.0.0 --port 8080
    (luego abre http://localhost:8080/ en el navegador)
"""
import argparse
import csv
import io
import json
import math
import os
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qs

LOG_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs")
STATIC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "static")

DEFAULT_NODES = ["A", "B", "C"]
# Cuántas líneas finales de cada log se leen como máximo por petición (evita que
# el coste crezca sin límite si el log lleva semanas acumulando datos).
MAX_LINES_PER_NODE = 2000
# Un nodo sin datos nuevos durante más de esto se marca como "stale" en la UI.
STALE_AFTER_SECONDS = 120
# La separación real entre nodos/vehículo es de unas pocas decenas de metros como
# mucho; una lectura de ping muy por encima de esto es casi seguro ruido/eco, no
# una distancia real, y se descarta como candidata para triangular.
DEFAULT_MAX_RANGE_M = 200.0
EARTH_RADIUS_M = 6371000.0


def latlon_to_local_xy(lat, lon, ref_lat, ref_lon):
    """Aproximación equirectangular: válida para distancias cortas (hasta unos
    pocos km), que es el rango típico de un enlace acústico submarino."""
    x = math.radians(lon - ref_lon) * EARTH_RADIUS_M * math.cos(math.radians(ref_lat))
    y = math.radians(lat - ref_lat) * EARTH_RADIUS_M
    return x, y


def local_xy_to_latlon(x, y, ref_lat, ref_lon):
    lat = ref_lat + math.degrees(y / EARTH_RADIUS_M)
    lon = ref_lon + math.degrees(x / (EARTH_RADIUS_M * math.cos(math.radians(ref_lat))))
    return lat, lon


def trilaterate(anchors):
    """anchors: lista de (x, y, distancia) en metros, len >= 3. Devuelve (x, y)
    o None si la geometría es degenerada (anclas alineadas/coincidentes).

    Linealiza las ecuaciones de intersección de circunferencias restando la
    del primer ancla a las demás, y resuelve el sistema resultante (n-1
    ecuaciones, 2 incógnitas) por mínimos cuadrados con la regla de Cramer
    (sin numpy: con 3 anclas son solo 2 ecuaciones, sistema 2x2 directo).
    """
    if len(anchors) < 3:
        return None

    x1, y1, d1 = anchors[0]
    rows = []
    rhs = []
    for xi, yi, di in anchors[1:]:
        rows.append((2 * (xi - x1), 2 * (yi - y1)))
        rhs.append((xi * xi - x1 * x1) + (yi * yi - y1 * y1) - (di * di - d1 * d1))

    a11 = sum(r[0] * r[0] for r in rows)
    a12 = sum(r[0] * r[1] for r in rows)
    a22 = sum(r[1] * r[1] for r in rows)
    b1 = sum(r[0] * v for r, v in zip(rows, rhs))
    b2 = sum(r[1] * v for r, v in zip(rows, rhs))

    det = a11 * a22 - a12 * a12
    if abs(det) < 1e-9:
        return None

    x = (b1 * a22 - a12 * b2) / det
    y = (a11 * b2 - b1 * a12) / det
    return x, y

def read_node_points(node_id):
    """Devuelve los puntos parseados del log para el nodo especificado.
    Soporta logs separados (<NODE>.log) y/o logs unificados (unknown.log / unknown server.log),
    fusionando y deduplicando por timestamp cronológicamente."""
    candidate_files = []
    node_path = os.path.join(LOG_DIR, f"{node_id}.log")
    if os.path.isfile(node_path):
        candidate_files.append((node_path, False))
    for unified_name in ("unknown.log", "unknown server.log"):
        unified_path = os.path.join(LOG_DIR, unified_name)
        if os.path.isfile(unified_path):
            candidate_files.append((unified_path, True))
            break

    if not candidate_files:
        return []

    points = []
    seen_timestamps = set()

    for path, is_unified in candidate_files:
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                # Si el archivo es unificado puede tener mensajes de múltiples nodos
                line_limit = (MAX_LINES_PER_NODE * 3) if is_unified else MAX_LINES_PER_NODE
                lines = f.readlines()[-line_limit:]
        except OSError:
            continue

        for line in lines:
            line = line.strip()
            if not line:
                continue

            # 1. Buscamos dónde empieza la llave del JSON por si hay texto delante
            start_idx = line.find("{")
            if start_idx == -1:
                continue

            try:
                raw_data = json.loads(line[start_idx:])
            except json.JSONDecodeError:
                continue

            # 2. Soportar tanto formato telemetry_server (con "data" y "receivedAt")
            # como salida copiada de la terminal (con "gpsTimestamp")
            if "data" in raw_data and "receivedAt" in raw_data:
                data = raw_data.get("data", {})
                raw_ts = raw_data.get("receivedAt")
                if not raw_ts:
                    continue
                try:
                    received_at = datetime.fromisoformat(raw_ts)
                except ValueError:
                    continue
            else:
                data = raw_data
                gps_ts = data.get("gpsTimestamp", "unknown")
                if not gps_ts or gps_ts == "unknown":
                    continue
                try:
                    received_at = datetime.strptime(gps_ts[:14], "%Y%m%d%H%M%S")
                except ValueError:
                    continue

            if received_at.tzinfo is None:
                received_at = received_at.replace(tzinfo=timezone.utc)

            # 3. Filtrar por el nodo que toca (soporta "nodeId" y "node")
            log_node = data.get("nodeId") or data.get("node")
            if log_node:
                if log_node != node_id:
                    continue
            elif is_unified:
                # En log unificado es obligatorio que declare el nodo
                continue

            # 4. Evitar duplicados entre archivos
            t_iso = received_at.isoformat()
            if t_iso in seen_timestamps:
                continue
            seen_timestamps.add(t_iso)

            # 5. Calcular distancia acústica (usa pingDistanceM si existe, sino calcula con RTT)
            rtt_ms = data.get("pingRttMs", -1)
            dist_m = data.get("pingDistanceM")
            if dist_m is None or dist_m <= 0:
                dist_m = (rtt_ms * 0.75) if (rtt_ms is not None and rtt_ms > 0) else None

            # 6. Coordenadas (soporta "latitude"/"longitude" y "lat"/"lon", descarta (0.0, 0.0))
            lat = data.get("latitude") if data.get("latitude") is not None else data.get("lat")
            lon = data.get("longitude") if data.get("longitude") is not None else data.get("lon")
            if lat == 0.0 and lon == 0.0:
                lat, lon = None, None

            points.append({
                "t": t_iso,
                "_dt": received_at,
                "lat": lat,
                "lon": lon,
                "gpsFix": bool(data.get("gpsFix")),
                "pingOk": bool(data.get("pingOk")),
                "pingRttMs": rtt_ms,
                "pingDistanceM": dist_m,
                "pingTargetDepthM": data.get("pingTargetDepthM"),
                "gsmState": "Connected" if data.get("gsmDataConnected") else ("Registered" if data.get("gsmRegistered") else "Offline"),
                "wifiMode": data.get("wifiMode"),
                "acousticModemHealthy": data.get("acousticModemHealthy"),
            })

    points.sort(key=lambda p: p["_dt"])
    return points[-MAX_LINES_PER_NODE:]
def valid_ping_points(points, max_range_m):
    """Filtra a los puntos usables como ancla para triangular: con fix GPS,
    con respuesta de ping, y con una distancia dentro de lo físicamente
    plausible (max_range_m) - una lectura absurdamente grande suele ser un eco
    o un RTT mal parseado, no una distancia real."""
    out = []
    for p in points:
        if not p["gpsFix"] or not p["pingOk"]:
            continue
        if p["lat"] is None or p["lon"] is None:
            continue
        dist = p.get("pingDistanceM")
        if dist is None or dist <= 0 or dist > max_range_m:
            continue
        out.append(p)
    return out


def solve_fix(triplet, manual_depth_m=None):
    """triplet: dict node_id -> punto válido (>=3 entradas). Devuelve
    {lat, lon, depthM} o None si la geometría es degenerada."""
    ref_lat = sum(p["lat"] for p in triplet.values()) / len(triplet)
    ref_lon = sum(p["lon"] for p in triplet.values()) / len(triplet)

    # Prioridad para la profundidad: (1) si algún nodo reporta pingTargetDepthM
    # (de un futuro sensor en el vehículo) se usa la media de esos valores; si no,
    # (2) se usa la profundidad indicada manualmente por el operador en la web; si
    # tampoco hay eso, (3) no se corrige (se asume profundidad ~0).
    depths = [p["pingTargetDepthM"] for p in triplet.values() if p.get("pingTargetDepthM") is not None]
    if depths:
        depth_m, depth_source = sum(depths) / len(depths), "sensor"
    elif manual_depth_m is not None:
        depth_m, depth_source = manual_depth_m, "manual"
    else:
        depth_m, depth_source = None, None

    anchors = []
    for p in triplet.values():
        x, y = latlon_to_local_xy(p["lat"], p["lon"], ref_lat, ref_lon)
        dist = p["pingDistanceM"]
        if depth_m is not None:
            dist = math.sqrt(max(dist * dist - depth_m * depth_m, 0.0))
        anchors.append((x, y, dist))

    solved = trilaterate(anchors)
    if solved is None:
        return None
    x, y = solved
    lat, lon = local_xy_to_latlon(x, y, ref_lat, ref_lon)
    return {"lat": lat, "lon": lon, "depthM": depth_m, "depthSource": depth_source}


def iter_target_fixes(nodes_points, node_ids, sync_window_s, max_range_m, manual_depth_m=None):
    """
    Genera estimaciones alineando en el tiempo los pings de cada nodo.
    Usa un algoritmo de punteros múltiples para encontrar tríadas donde la
    diferencia de tiempo (spread) entre los 3 nodos sea la mínima posible.
    """
    # 1. La función valid_ping_points ya se encarga de exigir gpsFix=True, 
    # pingOk=True y una distancia físicamente plausible.
    valid = {nid: valid_ping_points(nodes_points.get(nid, []), max_range_m) for nid in node_ids}
    arrays = [valid.get(nid, []) for nid in node_ids]
    
    if any(len(arr) == 0 for arr in arrays):
        return  # Si algún nodo no tiene datos válidos, no hay tríada posible
        
    # Ponemos un puntero al inicio de cada array de datos (los más antiguos)
    indices = [0] * len(node_ids)
    last_yielded_ids = None

    while True:
        # Extraemos los 3 puntos que señalan los punteros ahora mismo
        current_points = [arrays[i][indices[i]] for i in range(len(node_ids))]
        times = [p["_dt"] for p in current_points]
        
        min_t = min(times)
        max_t = max(times)
        min_idx = times.index(min_t)
        
        diff_s = (max_t - min_t).total_seconds()
        
        # Si la diferencia entre el ping más viejo y el más nuevo entra en la ventana
        if sync_window_s is None or diff_s <= sync_window_s:
            triplet = {node_ids[i]: current_points[i] for i in range(len(node_ids))}
            key = tuple(id(p) for p in current_points)
            
            # Evitamos recalcular y enviar coordenadas duplicadas si la tríada no cambia
            # Evitamos recalcular y enviar coordenadas duplicadas si la tríada no cambia
            if key != last_yielded_ids:
                fix = solve_fix(triplet, manual_depth_m)
                # Ahora hacemos yield SIEMPRE, incluso si fix es None (geometría imposible)
                yield {
                    "timestamp": max_t.isoformat(),
                    "anchors": triplet,
                    "fix": fix,
                }
                last_yielded_ids = key
        # MAGIA: Avanzamos solo el puntero del nodo que tiene el punto MÁS ANTIGUO.
        # Esto hace que el algoritmo "cierre la brecha" de tiempo en la siguiente
        # iteración, buscando siempre la sincronización perfecta de la triada.
        indices[min_idx] += 1
        if indices[min_idx] >= len(arrays[min_idx]):
            break  # Hemos consumido todo el array de uno de los nodos


def compute_live_target(nodes_points, node_ids, sync_window_s, max_range_m, manual_depth_m=None):
    """
    Calcula el estado actual basándose en la última triada óptima encontrada
    por el algoritmo de alineación temporal.
    """
    valid = {nid: valid_ping_points(nodes_points.get(nid, []), max_range_m) for nid in node_ids}
    candidates = {nid: points[-1] for nid, points in valid.items() if points}

    # Faltan nodos con señal
    if len(candidates) < len(node_ids):
        return {
            "available": False,
            "reason": "insufficient_anchors",
            "usedAnchors": sorted(candidates.keys()),
        }

    # Obtenemos todas las estimaciones generadas con nuestro algoritmo de punteros
    fixes = list(iter_target_fixes(nodes_points, node_ids, sync_window_s, max_range_m, manual_depth_m))
    newest_dt = max(p["_dt"] for p in candidates.values())
    
    # Si la lista vuelve vacía pero teníamos candidatos, significa que o bien están
    # muy desfasados en tiempo (falla la ventana de sync) o la geometría era plana.
    if not fixes:
        oldest_dt = min(p["_dt"] for p in candidates.values())
        if sync_window_s is not None and (newest_dt - oldest_dt).total_seconds() > sync_window_s:
            return {
                "available": False, 
                "reason": "sync_window_exceeded", 
                "usedAnchors": sorted(candidates.keys()),
                "timestamp": newest_dt.isoformat()
            }
        return {
            "available": False, 
            "reason": "geometry_degenerate", 
            "usedAnchors": sorted(candidates.keys()),
            "timestamp": newest_dt.isoformat()
        }

    last_event = fixes[-1]
    last_fix = last_event["fix"]
    if not last_fix:
        return {
            "available": False,
            "reason": "geometry_degenerate",
            "usedAnchors": sorted(last_event["anchors"].keys()),
            "timestamp": last_event["timestamp"],
        }
    return {
        "available": True,
        "lat": last_fix["lat"],
        "lon": last_fix["lon"],
        "depthM": last_fix["depthM"],
        "depthSource": last_fix["depthSource"],
        "usedAnchors": sorted(last_event["anchors"].keys()),
        "timestamp": last_event["timestamp"],
    }


def _public(point):
    if point is None:
        return None
    return {k: v for k, v in point.items() if not k.startswith("_")}


def parse_sync_window(raw, default):
    """raw viene de un parámetro de query string (str) o None. "off"/"none"/
    "false" desactivan el límite temporal (usar siempre el último dato de cada
    nodo, aunque haya pasado mucho tiempo entre ellos); cualquier otro valor se
    interpreta como segundos."""
    if raw is None or raw == "":
        return default
    if raw.strip().lower() in ("off", "none", "false"):
        return None
    try:
        return max(float(raw), 0.0)
    except ValueError:
        return default


class ViewerHandler(BaseHTTPRequestHandler):
    server_version = "SurfaceNodeViewer/1.0"

    # Solo se sirven estas rutas exactas: nada de acceso genérico al sistema de
    # ficheros, para no abrir una vulnerabilidad de path traversal.
    STATIC_ROUTES = {
        "/": ("index.html", "text/html; charset=utf-8"),
        "/viewer.js": ("viewer.js", "application/javascript; charset=utf-8"),
        "/viewer.css": ("viewer.css", "text/css; charset=utf-8"),
    }

    def log_message(self, fmt, *args):
        pass  # silencia el log por request; usa print() propio si hace falta depurar

    def do_GET(self):
        parsed = urlsplit(self.path)
        params = parse_qs(parsed.query)

        if parsed.path in self.STATIC_ROUTES:
            filename, content_type = self.STATIC_ROUTES[parsed.path]
            self._send_file(filename, content_type)
            return

        if parsed.path == "/api/tracks":
            self._handle_tracks(params)
            return

        if parsed.path == "/export.csv":
            self._handle_export_csv(params)
            return

        self.send_error(404)

    def _request_settings(self, params):
        sync_window_s = parse_sync_window(params.get("syncWindow", [None])[0], self.server.sync_window_s)
        try:
            max_range_m = float(params["maxRange"][0])
        except (KeyError, ValueError):
            max_range_m = self.server.max_range_m
        try:
            manual_depth_m = float(params["depth"][0])
        except (KeyError, ValueError):
            manual_depth_m = self.server.default_depth_m
        return sync_window_s, max_range_m, manual_depth_m

    def _send_file(self, filename, content_type):
        try:
            with open(os.path.join(STATIC_DIR, filename), "rb") as f:
                body = f.read()
        except FileNotFoundError:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _handle_tracks(self, params):
        sync_window_s, max_range_m, manual_depth_m = self._request_settings(params)
        nodes_points = {nid: read_node_points(nid) for nid in self.server.node_ids}

        now = datetime.now(timezone.utc)
        nodes_payload = {}
        for node_id, points in nodes_points.items():
            fix_points = [p for p in points if p["gpsFix"] and p["lat"] is not None and p["lon"] is not None]
            last_received = points[-1] if points else None
            stale = True
            if last_received:
                stale = (now - last_received["_dt"]).total_seconds() > STALE_AFTER_SECONDS

            nodes_payload[node_id] = {
                "points": [_public(p) for p in fix_points[-500:]],
                "lastFix": _public(fix_points[-1]) if fix_points else None,
                "lastReceivedAt": last_received["t"] if last_received else None,
                "stale": stale,
            }

        target = compute_live_target(nodes_points, self.server.node_ids, sync_window_s, max_range_m, manual_depth_m)
        if target is None:
            target = {"available": False, "reason": "no_recent_pings", "usedAnchors": []}
        target["points"] = []
        for event in list(iter_target_fixes(
            nodes_points, self.server.node_ids, sync_window_s, max_range_m, manual_depth_m
        ))[-500:]:
            fix = event["fix"]
            target["points"].append({
                "t": event["timestamp"], 
                "lat": fix["lat"] if fix else None, 
                "lon": fix["lon"] if fix else None,
                "depthM": fix["depthM"] if fix else None,
                "anchorsT": {nid: event["anchors"][nid]["t"] for nid in event["anchors"]}
            })

        body = json.dumps({
            "generatedAt": now.isoformat(),
            "settings": {"syncWindowS": sync_window_s, "maxRangeM": max_range_m, "manualDepthM": manual_depth_m},
            "nodes": nodes_payload,
            "target": target,
        }, ensure_ascii=False).encode("utf-8")

        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _handle_export_csv(self, params):
        sync_window_s, max_range_m, manual_depth_m = self._request_settings(params)
        nodes_points = {nid: read_node_points(nid) for nid in self.server.node_ids}

        buf = io.StringIO()
        writer = csv.writer(buf)
        header = ["timestamp_ref"]
        for nid in self.server.node_ids:
            header += [f"{nid}_t", f"{nid}_lat", f"{nid}_lon", f"{nid}_pingDistanceM"]
        header += ["target_lat", "target_lon", "target_depthM", "target_depthSource"]
        writer.writerow(header)

        for event in iter_target_fixes(nodes_points, self.server.node_ids, sync_window_s, max_range_m, manual_depth_m):
            row = [event["timestamp"]]
            for nid in self.server.node_ids:
                p = event["anchors"][nid]
                row += [p["t"], p["lat"], p["lon"], p["pingDistanceM"]]

            if event["fix"]:
                row += [event["fix"]["lat"], event["fix"]["lon"], event["fix"]["depthM"], event["fix"]["depthSource"]]
            else:
                row += ["Error", "Error", "", ""]
            writer.writerow(row)

        body = buf.getvalue().encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/csv; charset=utf-8")
        self.send_header("Content-Disposition", 'attachment; filename="target_fixes.csv"')
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)


def main():
    parser = argparse.ArgumentParser(description="Visor web (mapa + 3D) de los nodos de superficie")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--nodes", default=",".join(DEFAULT_NODES),
                         help="IDs de nodo separados por coma, ej: A,B,C")
    parser.add_argument("--sync-window", type=float, default=30.0,
                         help="Segundos máximos de diferencia entre los últimos pings de cada "
                              "nodo para seguir considerándolos válidos para triangular. Pasa "
                              "--sync-window=-1 para desactivar el límite por defecto (también "
                              "se puede activar/desactivar sin reiniciar desde la propia web).")
    parser.add_argument("--max-range", type=float, default=DEFAULT_MAX_RANGE_M,
                         help="Distancia máxima plausible (metros) entre un nodo y el vehículo; "
                              "lecturas de ping por encima se descartan como ruido")
    parser.add_argument("--default-depth", type=float, default=None,
                         help="Profundidad conocida del vehículo (metros) para corregir la "
                              "distancia acústica cuando ningún nodo reporta pingTargetDepthM; "
                              "también se puede indicar/cambiar desde la propia web (parámetro "
                              "depth). Si se omite, no se corrige (se asume profundidad ~0).")
    args = parser.parse_args()

    server = ThreadingHTTPServer((args.host, args.port), ViewerHandler)
    server.node_ids = [n.strip() for n in args.nodes.split(",") if n.strip()]
    server.sync_window_s = None if args.sync_window < 0 else args.sync_window
    server.max_range_m = args.max_range
    server.default_depth_m = args.default_depth

    print(f"Visor escuchando en http://{args.host}:{args.port}/")
    print(f"Leyendo logs desde: {LOG_DIR}")
    sync_txt = "sin límite" if server.sync_window_s is None else f"{server.sync_window_s}s"
    depth_txt = "desconocida" if server.default_depth_m is None else f"{server.default_depth_m}m"
    print(f"Nodos: {', '.join(server.node_ids)}  |  ventana de sincronización: {sync_txt}  |  "
          f"rango máximo plausible: {server.max_range_m}m  |  profundidad por defecto: {depth_txt}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down...")
        server.shutdown()


if __name__ == "__main__":
    main()
