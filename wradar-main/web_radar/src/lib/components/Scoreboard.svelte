<script lang="ts">
  import type { Player, Score } from "$lib/types";
  import TeamPanel from "./TeamPanel.svelte";

  interface Props {
    players: Player[];
    scores: Score[];
    highlightedPlayerId: number | null;
    onPlayerSelect: (id: number) => void;
  }

  let { players, scores, highlightedPlayerId, onPlayerSelect }: Props = $props();

  let terrorists = $derived(players.filter((p) => p.team === 2));
  let counterTerrorists = $derived(players.filter((p) => p.team === 3));
</script>

<div class="flex flex-col md:flex-row gap-4 mt-4">
  <TeamPanel
    players={terrorists}
    {scores}
    team="terrorist"
    {highlightedPlayerId}
    {onPlayerSelect}
  />
  <TeamPanel
    players={counterTerrorists}
    {scores}
    team="ct"
    {highlightedPlayerId}
    {onPlayerSelect}
  />
</div>
