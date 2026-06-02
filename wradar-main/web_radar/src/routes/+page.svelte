<script lang="ts">
  import { onMount } from "svelte";
  import type { Player, Score, CSMap, BombInfo } from "$lib/types";
  import { Team } from "$lib/constants";
  import { fetchMaps, createWebSocketConnection, type MatchData, type ConnectionStatus } from "$lib/services/matchService";
  import { ConnectionModal, RadarSettings, RadarMap, Scoreboard, PageLayout } from "$lib/components";

  // Connection state
  let wsStatus: ConnectionStatus = $state("connecting");
  let wsConnection: ReturnType<typeof createWebSocketConnection> | null = null;

  // Match data
  let players: Player[] = $state([]);
  let scores: Score[] = $state([]);
  let bombs: BombInfo[] = $state([]);
  let map = $state("");
  let posX = $state(0);
  let posY = $state(0);
  let scale = $state(1);

  // UI state
  let showSettings = $state(false);
  let mapOnlyView = $state(false);
  let shareButtonText = $state("🔗 Share this Match");

  // Radar settings
  let highlightedPlayerId: number | null = $state(null);
  let showFovCones = $state(true);
  let playerMarkerSize = $state(5);
  let fovConeLength = $state(60);
  let showBomb = $state(true);
  let highlightColor = $state("#facc15");
  let showTerrorists = $state(true);
  let showCounterTerrorists = $state(true);

  // Derived state
  let visiblePlayers = $derived(
    players.filter((p) => {
      if (p.team === Team.TERRORIST && !showTerrorists) return false;
      if (p.team === Team.COUNTER_TERRORIST && !showCounterTerrorists) return false;
      return true;
    })
  );

  // Event handlers
  function handleStatusChange(status: ConnectionStatus) {
    wsStatus = status;
  }

  function handleMatchData(data: MatchData) {
    players = data.players;
    scores = data.scores;
    bombs = data.bombs;
    map = data.map;
    posX = data.posX;
    posY = data.posY;
    scale = data.scale;
  }

  function togglePlayerHighlight(id: number) {
    highlightedPlayerId = highlightedPlayerId === id ? null : id;
  }

  function toggleSettings() {
    showSettings = !showSettings;
  }

  function toggleMapOnlyView() {
    mapOnlyView = !mapOnlyView;
  }

  function reconnect() {
    wsConnection?.connect();
  }

  async function shareMatch() {
    try {
      if (navigator.share) {
        await navigator.share({
          title: "CS Live Radar",
          text: `Watching a live match on ${map}!`,
          url: window.location.href
        });
      } else {
        await navigator.clipboard.writeText(window.location.href);
        shareButtonText = "✅ Link Copied!";
        setTimeout(() => (shareButtonText = "🔗 Share this Match"), 2500);
      }
    } catch (err) {
      console.error("Error sharing:", err);
    }
  }

  // Lifecycle
  onMount(() => {
    fetchMaps().then((maps) => {
      wsConnection = createWebSocketConnection(
        "ws://127.0.0.1:8080",
        maps,
        { onStatusChange: handleStatusChange, onMatchData: handleMatchData }
      );
      wsConnection.connect();
    });

    return () => wsConnection?.close();
  });
</script>

<div
  class="bg-gradient-to-b from-slate-900 to-slate-950 text-slate-200"
  class:min-h-screen={!mapOnlyView}
  class:h-screen={mapOnlyView}
  class:p-2={!mapOnlyView}
  class:sm:p-4={!mapOnlyView}
  class:lg:p-8={!mapOnlyView}
>
  <div
    class="w-full mx-auto bg-slate-800/50"
    class:max-w-7xl={!mapOnlyView}
    class:rounded-2xl={!mapOnlyView}
    class:shadow-2xl={!mapOnlyView}
    class:p-4={!mapOnlyView}
    class:border={!mapOnlyView}
    class:border-slate-700={!mapOnlyView}
    class:h-full={mapOnlyView}
  >
    <ConnectionModal status={wsStatus} onRetry={reconnect} />

    {#if mapOnlyView}
      <RadarMap
        {map}
        players={visiblePlayers}
        {bombs}
        {posX}
        {posY}
        {scale}
        {showSettings}
        {showBomb}
        {showFovCones}
        {playerMarkerSize}
        {fovConeLength}
        {highlightedPlayerId}
        {highlightColor}
        {mapOnlyView}
        onToggleSettings={toggleSettings}
        onToggleMapOnly={toggleMapOnlyView}
      />
    {:else}
      <PageLayout {map} {shareButtonText} onShare={shareMatch}>
        <RadarMap
          {map}
          players={visiblePlayers}
          {bombs}
          {posX}
          {posY}
          {scale}
          {showSettings}
          {showBomb}
          {showFovCones}
          {playerMarkerSize}
          {fovConeLength}
          {highlightedPlayerId}
          {highlightColor}
          {mapOnlyView}
          onToggleSettings={toggleSettings}
          onToggleMapOnly={toggleMapOnlyView}
        />

        {#if showSettings}
          <RadarSettings
            bind:showTerrorists
            bind:showCounterTerrorists
            bind:showFovCones
            bind:showBomb
            bind:playerMarkerSize
            bind:fovConeLength
            bind:highlightColor
          />
        {/if}

        <Scoreboard
          {players}
          {scores}
          {highlightedPlayerId}
          onPlayerSelect={togglePlayerHighlight}
        />
      </PageLayout>
    {/if}
  </div>
</div>

<style>
  @import url("https://fonts.googleapis.com/css2?family=Roboto+Condensed:wght@400;700&display=swap");
  :global(body) {
    font-family: "Roboto Condensed", sans-serif;
  }
</style>
