<script lang="ts">
  import { fly, fade } from "svelte/transition";

  interface Props {
    status: "connecting" | "connected" | "disconnected";
    onRetry: () => void;
  }

  let { status, onRetry }: Props = $props();
</script>

{#if status !== 'connected'}
  <div
    class="fixed inset-0 z-50 flex items-center justify-center bg-slate-950/80 backdrop-blur-sm"
    transition:fade={{ duration: 200 }}
  >
    <div
      class="bg-slate-800 border border-slate-700 rounded-2xl p-8 shadow-2xl text-center max-w-sm w-full mx-4"
      in:fly={{ y: 20, duration: 300, delay: 150 }}
      out:fade={{ duration: 150 }}
    >
      <svg
        class="w-16 h-16 mx-auto mb-4"
        class:text-red-500={status === 'disconnected'}
        class:text-sky-500={status === 'connecting'}
        class:animate-spin={status === 'connecting'}
        xmlns="http://www.w3.org/2000/svg"
        fill="none"
        viewBox="0 0 24 24"
        stroke-width="1.5"
        stroke="currentColor"
      >
        {#if status === 'connecting'}
          <path d="M256 48a208 208 0 0 1 187 116" />
          <polyline points="472 48 472 184 336 184" fill="none" stroke="black" stroke-width="40" stroke-linecap="round" stroke-linejoin="round"/>
          <path d="M256 464a208 208 0 0 1 -187 -116" />
          <polyline points="40 464 40 328 176 328" fill="none" stroke="black" stroke-width="40" stroke-linecap="round" stroke-linejoin="round"/>
        {:else}
          <path
            stroke-linecap="round"
            stroke-linejoin="round"
            d="M12 9v3.75m-9.303 3.376c-.866 1.5.217 3.374 1.948 3.374h14.71c1.73 0 2.813-1.874 1.948-3.374L13.949 3.378c-.866-1.5-3.032-1.5-3.898 0L2.697 16.126zM12 15.75h.007v.008H12v-.008z"
          />
        {/if}
      </svg>

      <h2 class="text-2xl font-bold text-slate-200 mb-2">
        {#if status === 'connecting'}
          Connecting
        {:else}
          Connection Lost
        {/if}
      </h2>
      <p class="text-slate-400 mb-6">
        {#if status === 'connecting'}
          Attempting to connect to the live match server...
        {:else}
          Could not establish a connection. Please check your network and try again.
        {/if}
      </p>
      {#if status === 'disconnected'}
        <button
          onclick={onRetry}
          class="w-full bg-sky-500 text-white font-bold px-8 py-3 rounded-lg hover:bg-sky-600 transition-colors duration-200 shadow-lg focus:outline-none focus:ring-2 focus:ring-sky-400 focus:ring-opacity-75"
        >
          Retry
        </button>
      {/if}
    </div>
  </div>
{/if}
