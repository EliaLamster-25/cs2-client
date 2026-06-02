<script lang="ts">
  import type { Player, BombInfo } from "$lib/types";
  import { worldToRadar, MAP_WIDTH, MAP_HEIGHT } from "$lib/radarUtils";

  interface Props {
    players: Player[];
    bombs: BombInfo[];
    posX: number;
    posY: number;
    scale: number;
    overlayWidth: number;
    overlayHeight: number;
    showBomb: boolean;
    showFovCones: boolean;
    playerMarkerSize: number;
    fovConeLength: number;
    highlightedPlayerId: number | null;
    highlightColor: string;
  }

  let {
    players,
    bombs,
    posX,
    posY,
    scale,
    overlayWidth,
    overlayHeight,
    showBomb,
    showFovCones,
    playerMarkerSize,
    fovConeLength,
    highlightedPlayerId,
    highlightColor
  }: Props = $props();

  function getRadarPos(worldX: number, worldY: number) {
    return worldToRadar(
      worldX,
      worldY,
      posX,
      posY,
      scale,
      overlayWidth,
      overlayHeight,
      MAP_WIDTH,
      MAP_HEIGHT
    );
  }

  function getPlayerColor(player: Player): string {
    if (player.id === highlightedPlayerId) return highlightColor;
    return player.team === 2 ? "#ef4444" : "#38bdf8";
  }
</script>

{#if bombs.length > 0 && bombs[0].planted && showBomb}
  {@const bombPos = getRadarPos(bombs[0].x, bombs[0].y)}
  <svg
    class="absolute w-4 h-4 md:w-8 md:h-8 text-red-600 z-20 bomb-marker pointer-events-auto"
    style="left: {bombPos.x}px; top: {bombPos.y}px;"
    viewBox="0 0 35 35"
    fill="currentColor"
  >
    <path d="M9.9674 ... Z" fill="white" />
  </svg>
{/if}

{#each players as player (player.id)}
  {#if player.position.length > 0 && player.angle.length > 0}
    {@const radarPos = getRadarPos(player.position[0].x, player.position[0].y)}
    <svg
      class="absolute z-10 pointer-events-none"
      style="left:0; top:0; width:{overlayWidth}px; height:{overlayHeight}px;"
    >
      {#if player.id === highlightedPlayerId}
        <circle
          cx={radarPos.x}
          cy={radarPos.y}
          r={playerMarkerSize * 1.2}
          fill="none"
          style="stroke: {highlightColor};"
          class="highlight-ring"
        />
      {/if}

      <circle
        cx={radarPos.x}
        cy={radarPos.y}
        r={playerMarkerSize}
        fill={getPlayerColor(player)}
        stroke="white"
        stroke-width="1.5"
        filter="url(#glow)"
      />

      {#if showFovCones}
        <path
          d="M {radarPos.x} {radarPos.y}
          L {radarPos.x + Math.cos(((player.angle[0].y - 30) * Math.PI) / 180) * fovConeLength}
            {radarPos.y - Math.sin(((player.angle[0].y - 30) * Math.PI) / 180) * fovConeLength}
          A {fovConeLength} {fovConeLength} 0 0 1
            {radarPos.x + Math.cos(((player.angle[0].y + 30) * Math.PI) / 180) * fovConeLength}
            {radarPos.y - Math.sin(((player.angle[0].y + 30) * Math.PI) / 180) * fovConeLength}
          Z"
          fill={player.team === 2 ? "url(#terroristGradient)" : "url(#ctGradient)"}
          stroke="rgba(255,255,255,0.3)"
          stroke-width="1"
          filter="url(#blurGlow)"
        />
      {/if}

      <defs>
        <radialGradient id="terroristGradient" cx="50%" cy="50%" r="100%">
          <stop offset="0%" stop-color="rgba(239,68,68,0.6)" />
          <stop offset="100%" stop-color="rgba(239,68,68,0)" />
        </radialGradient>
        <radialGradient id="ctGradient" cx="50%" cy="50%" r="100%">
          <stop offset="0%" stop-color="rgba(56,189,248,0.6)" />
          <stop offset="100%" stop-color="rgba(56,189,248,0)" />
        </radialGradient>
        <filter id="blurGlow" x="-50%" y="-50%" width="200%" height="200%">
          <feGaussianBlur in="SourceGraphic" stdDeviation="3" result="blur" />
          <feMerge>
            <feMergeNode in="blur" />
            <feMergeNode in="SourceGraphic" />
          </feMerge>
        </filter>
        <filter id="glow" x="-50%" y="-50%" width="200%" height="200%">
          <feGaussianBlur stdDeviation="2.5" result="coloredBlur" />
          <feMerge>
            <feMergeNode in="coloredBlur" />
            <feMergeNode in="SourceGraphic" />
          </feMerge>
        </filter>
      </defs>
    </svg>
  {/if}
{/each}

<style>
  .bomb-marker {
    transform: translate(-50%, -50%);
    animation: bomb-pulse 1s infinite ease-in-out;
  }
  @keyframes bomb-pulse {
    0%, 100% {
      transform: scale(1) translate(-50%, -50%);
      filter: drop-shadow(0 0 2px #ff0000);
    }
    50% {
      transform: scale(1.1) translate(-50%, -50%);
      filter: drop-shadow(0 0 10px #ff0000);
    }
  }

  @keyframes highlight-pulse {
    0% {
      stroke-width: 3px;
      transform: scale(0.8);
      opacity: 1;
    }
    100% {
      stroke-width: 0;
      transform: scale(2.5);
      opacity: 0;
    }
  }
  .highlight-ring {
    transform-origin: center;
    animation: highlight-pulse 1.5s infinite ease-out;
    transform-box: fill-box;
  }
</style>
