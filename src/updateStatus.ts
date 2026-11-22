export interface UpdateStatus {
  available: boolean;
  updateAvailable: boolean;
  automaticCheckAfter: number;
  runningVersion: string;
  instanceId?: string;
  savedVersion: string;
  latestVersion: string;
  checksum: string;
  phase: string;
  busy: boolean;
  error: string;
  restartRequired: boolean;
  received: number;
  retryAt: number;
  checkedAt: number;
  checkAfter: number;
  /* Orbit restarts itself through the ELF loader and installs itself in one step. */
  selfRestart?: boolean;
  restartNotice?: string;
  activationState?: "running" | "saved" | "restarting";
  updateAll?: boolean;
}
