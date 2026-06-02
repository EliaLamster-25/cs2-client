import type { Player, Score, CSMap, BombInfo } from "$lib/types";

export type ConnectionStatus = "connecting" | "connected" | "disconnected";

export interface MatchData {
    players: Player[];
    scores: Score[];
    bombs: BombInfo[];
    map: string;
    posX: number;
    posY: number;
    scale: number;
}

export interface WebSocketCallbacks {
    onStatusChange: (status: ConnectionStatus) => void;
    onMatchData: (data: MatchData) => void;
}

import { decode } from "@msgpack/msgpack";

export function createWebSocketConnection(
    url: string,
    maps: CSMap[],
    callbacks: WebSocketCallbacks
): { connect: () => void; close: () => void } {
    let ws: WebSocket | null = null;

    function connect() {
        if (ws && ws.readyState < 2) {
            ws.close();
        }

        callbacks.onStatusChange("connecting");
        ws = new WebSocket(url);
        ws.binaryType = "arraybuffer";

        ws.onopen = () => {
            callbacks.onStatusChange("connected");
        };

        ws.onclose = () => {
            callbacks.onStatusChange("disconnected");
        };

        ws.onerror = (err) => {
            console.error("WebSocket error:", err);
            callbacks.onStatusChange("disconnected");
        };

        ws.onmessage = (event) => {
            callbacks.onStatusChange("connected");

            let data: any;
            if (event.data instanceof ArrayBuffer) {
                data = decode(new Uint8Array(event.data));
            } else {
                data = JSON.parse(event.data);
            }

            if (!maps.length) return;

            const players: Player[] = data.players;
            players.forEach((p) => {
                p.is_alive = p.health > 0;
            });

            const csmap = maps.find((m) => m.map_name === data["map"]);

            callbacks.onMatchData({
                players,
                scores: data.score,
                bombs: data.bomb,
                map: data["map"],
                posX: csmap?.pos_x ?? 0,
                posY: csmap?.pos_y ?? 0,
                scale: csmap?.scale ?? 1
            });

            if (!csmap) {
                console.warn("Map not found:", data["map"]);
            }
        };
    }

    function close() {
        ws?.close();
    }

    return { connect, close };
}

export async function fetchMaps(): Promise<CSMap[]> {
    const res = await fetch("/maps.json");
    const data = await res.json();
    return data.maps;
}
