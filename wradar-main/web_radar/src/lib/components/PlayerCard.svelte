<script lang="ts">
  import { fly } from "svelte/transition";
  import type { Player } from "$lib/types";
  import { getWeaponName } from "$lib/weapons";
  import { getWeaponIconPath } from "$lib/radarUtils";

  interface Props {
    player: Player;
    index: number;
    isHighlighted: boolean;
    team: "terrorist" | "ct";
    onSelect: () => void;
  }

  let { player, index, isHighlighted, team, onSelect }: Props = $props();

  const teamColor = team === "terrorist" ? "red" : "sky";
  const borderActiveClass = team === "terrorist" ? "border-red-500" : "border-sky-400";
</script>

<button
  onclick={onSelect}
  in:fly={{ y: 20, duration: 300, delay: index * 70 }}
  class="flex items-center w-full bg-slate-700/50 p-2 rounded-lg transition-all duration-300 hover:bg-slate-700 hover:scale-[1.02] cursor-pointer border-2 text-left"
  class:opacity-40={!player.is_alive}
  class:saturate-0={!player.is_alive}
  class:border-yellow-400={isHighlighted}
  class:border-transparent={!isHighlighted}
>
  {#if team === "terrorist"}
    <img
      src={player.avatar}
      alt={player.name}
      class="w-11 h-11 rounded-full mr-3 border-2"
      class:border-red-500={player.is_alive}
      class:border-slate-600={!player.is_alive}
    />
    <div class="flex-1">
      <p class="font-bold text-lg">{player.name}</p>
      <div class="h-1.5 w-full bg-slate-600 rounded-full mt-1 overflow-hidden">
        <div
          class="h-full rounded-full transition-all duration-300 ease-out health-bar"
          style="width: {player.health}%; --health: {player.health}"
        ></div>
      </div>
    </div>
    <div class="flex items-center justify-end text-center w-24 mx-3 font-mono">
      <span class="font-bold w-1/3 text-slate-200">{player.stats.kills}</span>
      <span class="text-slate-400 w-1/3">{player.stats.assists}</span>
      <span class="w-1/3 text-slate-200">{player.stats.deaths}</span>
    </div>
    <div class="flex items-center justify-end space-x-2 w-40">
      {#if player.is_alive}
        {#each player.weapons as weapon}
          <div class="text-slate-200">
            <img
              src={getWeaponIconPath(getWeaponName(weapon.id))}
              alt={getWeaponName(weapon.id)}
              class="w-16 h-16 rounded-md"
            />
          </div>
        {/each}
      {/if}
    </div>
  {:else}
    <div class="flex items-center justify-start space-x-2 w-40">
      {#if player.is_alive}
        {#each player.weapons as weapon}
          <div class="text-slate-200">
            <img
              src={getWeaponIconPath(getWeaponName(weapon.id))}
              alt={getWeaponName(weapon.id)}
              class="w-16 h-16 rounded-md"
            />
          </div>
        {/each}
      {/if}
    </div>
    <div class="flex items-center text-center w-24 mx-3 font-mono">
      <span class="font-bold w-1/3 text-slate-200">{player.stats.kills}</span>
      <span class="text-slate-400 w-1/3">{player.stats.assists}</span>
      <span class="w-1/3 text-slate-200">{player.stats.deaths}</span>
    </div>
    <div class="flex-1 text-right">
      <p class="font-bold text-lg">{player.name}</p>
      <div class="h-1.5 w-full bg-slate-600 rounded-full mt-1 overflow-hidden">
        <div
          class="h-full rounded-full ml-auto transition-all duration-300 ease-out health-bar"
          style="width: {player.health}%; --health: {player.health}"
        ></div>
      </div>
    </div>
    <img
      src={player.avatar}
      alt={player.name}
      class="w-11 h-11 rounded-full ml-3 border-2"
      class:border-sky-400={player.is_alive}
      class:border-slate-600={!player.is_alive}
    />
  {/if}
</button>

<style>
  .health-bar {
    --color-high: #4ade80;
    --color-mid: #facc15;
    --color-low: #ef4444;
    background-color: var(--color-low);
  }
  .health-bar[style*="--health"]:is(
      [style*="100"],
      [style*="9"],
      [style*="8"],
      [style*="7"],
      [style*="6"],
      [style*="5"]
    ) {
    background-color: var(--color-high);
  }
  .health-bar[style*="--health"]:is([style*="4"], [style*="3"], [style*="2"]) {
    background-color: var(--color-mid);
  }
</style>
