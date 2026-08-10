export type MoveDirection = 'forward' | 'reverse' | 'left' | 'right' | 'stop'

export type ConnectionStatus = 'disconnected' | 'connecting' | 'connected' | 'error'

let commandSeq = 0

export function nextCommandId(): string {
  commandSeq += 1
  return `web-${Date.now()}-${commandSeq}`
}

export function buildMoveCommand(direction: MoveDirection) {
  return {
    type: 'command' as const,
    id: nextCommandId(),
    action: 'move' as const,
    payload: { direction },
  }
}

/** Pause (`enabled: false`) or resume MJPEG output on the robot. */
export function buildStreamCommand(enabled: boolean) {
  return {
    type: 'command' as const,
    id: nextCommandId(),
    action: 'stream' as const,
    payload: { enabled },
  }
}

export function controlBaseUrl(host: string, port = 80): string {
  return `http://${host}:${port}`
}

export function streamUrl(host: string, streamPort = 81): string {
  return `http://${host}:${streamPort}/stream`
}

export function wsUrl(host: string, port = 80): string {
  return `ws://${host}:${port}/ws`
}

export function normalizeHost(input: string): string {
  return input
    .trim()
    .replace(/^https?:\/\//i, '')
    .replace(/^ws:\/\//i, '')
    .replace(/\/.*$/, '')
    .replace(/:\d+$/, '')
}
