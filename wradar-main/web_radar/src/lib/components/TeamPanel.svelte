<script lang="ts">
  import type { Player, Score } from "$lib/types";
  import PlayerCard from "./PlayerCard.svelte";

  interface Props {
    players: Player[];
    scores: Score[];
    team: "terrorist" | "ct";
    highlightedPlayerId: number | null;
    onPlayerSelect: (id: number) => void;
  }

  let { players, scores, team, highlightedPlayerId, onPlayerSelect }: Props = $props();

  const teamConfig = {
    terrorist: {
      label: "T",
      name: "Terrorists",
      scoreKey: "TERRORIST",
      colorClass: "text-red-500",
      bgClass: "bg-red-500"
    },
    ct: {
      label: "CT",
      name: "Counter-Terrorists",
      scoreKey: "CT",
      colorClass: "text-sky-400",
      bgClass: "bg-sky-400"
    }
  };

  const config = teamConfig[team];
  const score = $derived(scores.find((s) => s.team === config.scoreKey)?.score ?? 0);
</script>

<div class="flex-1">
  <div class="flex items-center justify-between bg-slate-900/70 p-2 rounded-t-lg">
    {#if team === "terrorist"}
      <div class="flex items-center gap-3">
        <div class="w-8 h-8 {config.bgClass} rounded-sm flex items-center justify-center font-bold">
          {config.label}
        </div>
        <h2 class="text-xl font-bold {config.colorClass} uppercase tracking-wider">
          {config.name}
        </h2>
      </div>
      <div class="text-2xl font-bold">{score}</div>
    {:else}
      <div class="text-2xl font-bold">{score}</div>
      <div class="flex items-center gap-3">
        <h2 class="text-xl font-bold {config.colorClass} uppercase tracking-wider">
          {config.name}
        </h2>
        <div class="w-8 h-8 {config.bgClass} rounded-sm flex items-center justify-center font-bold">
          {config.label}
        </div>
      </div>
    {/if}
  </div>
  <div class="space-y-1.5 bg-slate-800/60 p-2 rounded-b-lg">
    {#each players as player, i (player.id)}
      <PlayerCard
        {player}
        index={i}
        {team}
        isHighlighted={highlightedPlayerId === player.id}
        onSelect={() => onPlayerSelect(player.id)}
      />
    {/each}
  </div>
</div>
