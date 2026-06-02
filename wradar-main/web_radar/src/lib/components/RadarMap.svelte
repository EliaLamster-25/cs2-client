<script lang="ts">
  import type { Player, Score, BombInfo } from "$lib/types";
  import { MAP_WIDTH, MAP_HEIGHT } from "$lib/radarUtils";
  import RadarControls from "./RadarControls.svelte";
  import RadarOverlay from "./RadarOverlay.svelte";

  interface Props {
    map: string;
    players: Player[];
    bombs: BombInfo[];
    posX: number;
    posY: number;
    scale: number;
    showSettings: boolean;
    showBomb: boolean;
    showFovCones: boolean;
    playerMarkerSize: number;
    fovConeLength: number;
    highlightedPlayerId: number | null;
    highlightColor: string;
    mapOnlyView: boolean;
    onToggleSettings: () => void;
    onToggleMapOnly: () => void;
  }

  let {
    map,
    players,
    bombs,
    posX,
    posY,
    scale,
    showSettings,
    showBomb,
    showFovCones,
    playerMarkerSize,
    fovConeLength,
    highlightedPlayerId,
    highlightColor,
    mapOnlyView,
    onToggleSettings,
    onToggleMapOnly
  }: Props = $props();

  let radarContainer: HTMLDivElement | undefined = $state();
  let radarWidth = $state(MAP_WIDTH);
  let radarHeight = $state(MAP_HEIGHT);
  let isFullScreen = $state(false);

  let overlay = $derived.by(() => {
    if (!radarContainer) {
      return { width: radarWidth, height: radarHeight, top: 0, left: 0 };
    }

    if (isFullScreen || mapOnlyView) {
      const containerWidth = radarContainer.clientWidth;
      const containerHeight = radarContainer.clientHeight;
      const containerRatio = containerWidth / containerHeight;
      const imageRatio = MAP_WIDTH / MAP_HEIGHT;

      if (containerRatio > imageRatio) {
        const height = containerHeight;
        const width = height * imageRatio;
        return {
          width,
          height,
          top: 0,
          left: (containerWidth - width) / 2
        };
      } else {
        const width = containerWidth;
        const height = width / imageRatio;
        return {
          width,
          height,
          left: 0,
          top: (containerHeight - height) / 2
        };
      }
    }

    return { width: radarWidth, height: radarHeight, top: 0, left: 0 };
  });

  function toggleFullScreen() {
    if (!document.fullscreenElement) {
      radarContainer?.requestFullscreen();
    } else {
      document.exitFullscreen();
    }
  }

  function handleFullScreenChange() {
    isFullScreen = !!document.fullscreenElement;
  }

  $effect(() => {
    document.addEventListener("fullscreenchange", handleFullScreenChange);
    return () => {
      document.removeEventListener("fullscreenchange", handleFullScreenChange);
    };
  });
</script>

<div
  class="relative w-full rounded-lg shadow-lg overflow-hidden border-2 border-slate-700 group"
  class:aspect-square={!isFullScreen && !mapOnlyView}
  class:h-full={mapOnlyView || isFullScreen}
  class:bg-black={isFullScreen || mapOnlyView}
  bind:clientWidth={radarWidth}
  bind:clientHeight={radarHeight}
  bind:this={radarContainer}
>
  <img
    src="/{map}_radar_psd.png"
    alt="Radar map for {map}"
    class="absolute inset-0 w-full h-full z-0"
    class:object-cover={!isFullScreen && !mapOnlyView}
    class:object-contain={isFullScreen || mapOnlyView}
  />

  <RadarControls
    {showSettings}
    {mapOnlyView}
    {isFullScreen}
    {onToggleSettings}
    onToggleMapOnly={onToggleMapOnly}
    onToggleFullScreen={toggleFullScreen}
  />

  <div
    class="absolute z-10 pointer-events-none"
    style="width:{overlay.width}px; height:{overlay.height}px; top:{overlay.top}px; left:{overlay.left}px;"
  >
    <RadarOverlay
      {players}
      {bombs}
      {posX}
      {posY}
      {scale}
      overlayWidth={overlay.width}
      overlayHeight={overlay.height}
      {showBomb}
      {showFovCones}
      {playerMarkerSize}
      {fovConeLength}
      {highlightedPlayerId}
      {highlightColor}
    />
  </div>
</div>
