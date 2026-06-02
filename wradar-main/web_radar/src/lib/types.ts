export interface CSWeapon {
  id: number;
}

export interface Player {
  id: number;
  name: string;
  team: number;
  health: number;
  position: { x: number; y: number; z: number }[];
  angle: { x: number; y: number; z: number }[];
  avatar: string;
  weapons: CSWeapon[];
  is_alive: boolean;
  stats: { kills: number; assists: number; deaths: number };
}
export interface Score {
  team: string;
  score: number;
}
export interface CSMap {
  map_name: string;
  pos_x: number;
  pos_y: number;
  scale: number;
}
export interface BombInfo {
  planted: boolean;
  x: number;
  y: number;
  z: number;
}
