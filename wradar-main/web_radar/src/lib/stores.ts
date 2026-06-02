import { writable } from 'svelte/store';
import type { Player, Score, BombInfo } from './types';

export const players = writable<Player[]>([]);
export const scores = writable<Score[]>([]);
export const bombs = writable<BombInfo[]>([]);
export const map = writable<string>("");


export const showSettings = writable<boolean>(false);
export const highlightedPlayerId = writable<number | null>(null);

export type ConnectionStatus = 'connecting' | 'connected' | 'disconnected';
export const wsStatus = writable<ConnectionStatus>('connecting');