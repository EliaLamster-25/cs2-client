<script lang="ts">
  import { fly, fade } from "svelte/transition";

  interface Props {
    showTerrorists: boolean;
    showCounterTerrorists: boolean;
    showFovCones: boolean;
    showBomb: boolean;
    playerMarkerSize: number;
    fovConeLength: number;
    highlightColor: string;
  }

  let {
    showTerrorists = $bindable(),
    showCounterTerrorists = $bindable(),
    showFovCones = $bindable(),
    showBomb = $bindable(),
    playerMarkerSize = $bindable(),
    fovConeLength = $bindable(),
    highlightColor = $bindable()
  }: Props = $props();
</script>

<div
  in:fly={{ y: -20, duration: 300 }}
  out:fade={{ duration: 200 }}
  class="my-4 bg-slate-800/60 border border-slate-700 rounded-lg p-4 shadow-lg"
>
  <h3 class="text-lg font-bold text-slate-400 uppercase tracking-wider mb-4 text-center">
    Radar Options
  </h3>
  <div class="grid grid-cols-1 md:grid-cols-3 gap-x-8 gap-y-6">
    <div class="space-y-4">
      <h4 class="font-bold border-b border-slate-600 pb-1">Visibility</h4>
      <label class="flex items-center justify-between cursor-pointer">
        <span class="font-semibold">Show Terrorists</span>
        <input type="checkbox" bind:checked={showTerrorists} class="toggle-checkbox" />
      </label>
      <label class="flex items-center justify-between cursor-pointer">
        <span class="font-semibold">Show CTs</span>
        <input type="checkbox" bind:checked={showCounterTerrorists} class="toggle-checkbox" />
      </label>
      <label class="flex items-center justify-between cursor-pointer">
        <span class="font-semibold">Show Vision Cones</span>
        <input type="checkbox" bind:checked={showFovCones} class="toggle-checkbox" />
      </label>
      <label class="flex items-center justify-between cursor-pointer">
        <span class="font-semibold">Show Bomb Icon</span>
        <input type="checkbox" bind:checked={showBomb} class="toggle-checkbox" />
      </label>
    </div>
    <div class="space-y-4">
      <h4 class="font-bold border-b border-slate-600 pb-1">Sizing</h4>
      <label class="block">
        <div class="flex justify-between items-center">
          <span class="font-semibold">Player Size</span>
          <span class="text-sm text-slate-400">{playerMarkerSize}</span>
        </div>
        <input
          type="range"
          bind:value={playerMarkerSize}
          min="2"
          max="10"
          step="0.5"
          class="w-full h-2 bg-slate-700 rounded-lg appearance-none cursor-pointer range-slider"
        />
      </label>
      <label class="block">
        <div class="flex justify-between items-center">
          <span class="font-semibold">Cone Size</span>
          <span class="text-sm text-slate-400">{fovConeLength}</span>
        </div>
        <input
          type="range"
          bind:value={fovConeLength}
          min="10"
          max="150"
          step="5"
          class="w-full h-2 bg-slate-700 rounded-lg appearance-none cursor-pointer range-slider"
        />
      </label>
    </div>
    <div class="space-y-4">
      <h4 class="font-bold border-b border-slate-600 pb-1">Highlighting</h4>
      <label class="flex items-center justify-between">
        <span class="font-semibold">Highlight Color</span>
        <input
          type="color"
          bind:value={highlightColor}
          class="w-10 h-8 p-1 bg-slate-700 border border-slate-600 rounded cursor-pointer"
        />
      </label>
      <p class="text-sm text-slate-400 italic">
        Click on a player in the lists below to highlight them on the map.
      </p>
    </div>
  </div>
</div>

<style>
  .toggle-checkbox {
    appearance: none;
    width: 40px;
    height: 22px;
    background-color: #334155;
    border-radius: 9999px;
    position: relative;
    cursor: pointer;
    transition: background-color 0.2s ease-in-out;
  }
  .toggle-checkbox::before {
    content: "";
    position: absolute;
    width: 16px;
    height: 16px;
    border-radius: 50%;
    background-color: white;
    top: 3px;
    left: 3px;
    transition: transform 0.2s ease-in-out;
  }
  .toggle-checkbox:checked {
    background-color: #38bdf8;
  }
  .toggle-checkbox:checked::before {
    transform: translateX(18px);
  }

  .range-slider::-webkit-slider-thumb {
    -webkit-appearance: none;
    appearance: none;
    width: 18px;
    height: 18px;
    background: #f1f5f9;
    cursor: pointer;
    border-radius: 50%;
    border: 2px solid #38bdf8;
  }
  .range-slider::-moz-range-thumb {
    width: 18px;
    height: 18px;
    background: #f1f5f9;
    cursor: pointer;
    border-radius: 50%;
    border: 2px solid #38bdf8;
  }
</style>
