<script lang="ts">
  import type { Snippet } from "svelte";

  interface Props {
    map: string;
    shareButtonText: string;
    onShare: () => void;
    children: Snippet;
  }

  let { map, shareButtonText, onShare, children }: Props = $props();
</script>

<header class="text-center mb-4">
  <h1 class="text-2xl font-bold uppercase tracking-widest text-slate-400">
    Current Map: <span class="text-white">{map}</span>
  </h1>
</header>

{@render children()}

<footer class="text-center mt-6">
  <button
    onclick={onShare}
    class="bg-slate-700/80 text-slate-300 px-6 py-2 rounded-lg hover:bg-slate-700 transition-colors duration-200 shadow-md"
  >
    {shareButtonText}
  </button>
</footer>
