export const Team = {
  TERRORIST: 2,
  COUNTER_TERRORIST: 3,
} as const;

export const TeamName = {
  [Team.TERRORIST]: "Terrorists",
  [Team.COUNTER_TERRORIST]: "Counter-Terrorists",
}