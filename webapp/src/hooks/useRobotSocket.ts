import { useCallback, useEffect, useRef, useState } from 'react'
import {
  buildMoveCommand,
  buildStreamCommand,
  normalizeHost,
  type ConnectionStatus,
  type MoveDirection,
  wsUrl,
} from '../lib/robotApi'

/* Station mode: the router assigns the robot a DHCP address, so reach it by the
 * mDNS name the firmware advertises. The raw IP still works if typed in. */
const DEFAULT_HOST = 'robot.local'
const HOST_STORAGE_KEY = 'robot.host'
/* Every command gets an ack. Going this long with a command outstanding and no
 * reply of any kind means the socket is open but the firmware is not servicing
 * it — an open WebSocket is not proof that commands are arriving.
 *
 * Deliberately measured against unanswered commands rather than "any traffic":
 * the firmware ships with its telemetry push disabled, so an idle connection is
 * legitimately silent and must not be reported as broken. */
const ACK_TIMEOUT_MS = 4000

function loadHost(): string {
  try {
    return localStorage.getItem(HOST_STORAGE_KEY) || DEFAULT_HOST
  } catch {
    return DEFAULT_HOST
  }
}

function saveHost(host: string) {
  try {
    localStorage.setItem(HOST_STORAGE_KEY, host)
  } catch {
    /* private mode / storage disabled — not worth failing the connect over */
  }
}

export function useRobotSocket() {
  const [hostInput, setHostInput] = useState(loadHost)
  const [status, setStatus] = useState<ConnectionStatus>('disconnected')
  const [error, setError] = useState<string | null>(null)
  const [streamSrc, setStreamSrc] = useState<string | null>(null)
  const [streamOn, setStreamOn] = useState(false)
  const [activeDirection, setActiveDirection] = useState<MoveDirection | null>(null)
  const [lastSent, setLastSent] = useState<string | null>(null)
  const [lastReply, setLastReply] = useState<string | null>(null)

  const socketRef = useRef<WebSocket | null>(null)
  const hostRef = useRef(loadHost())
  const pressedRef = useRef<MoveDirection | null>(null)
  const streamOnRef = useRef(false)
  /* Timestamp of the oldest command still awaiting any reply, or null when
   * everything sent has been answered. */
  const pendingSinceRef = useRef<number | null>(null)
  const watchdogRef = useRef<number | null>(null)

  const clearWatchdog = useCallback(() => {
    if (watchdogRef.current !== null) {
      clearInterval(watchdogRef.current)
      watchdogRef.current = null
    }
  }, [])

  const sendJson = useCallback((payload: object): boolean => {
    const socket = socketRef.current
    if (!socket || socket.readyState !== WebSocket.OPEN) {
      setError('Not connected — press Connect first')
      return false
    }
    const text = JSON.stringify(payload)
    socket.send(text)
    if (pendingSinceRef.current === null) {
      pendingSinceRef.current = Date.now()
    }
    const action = 'action' in payload ? String((payload as { action?: string }).action) : '?'
    const direction =
      'payload' in payload &&
      payload.payload &&
      typeof payload.payload === 'object' &&
      'direction' in (payload.payload as object)
        ? String((payload.payload as { direction?: string }).direction)
        : null
    setLastSent(direction ? `${action}:${direction}` : action)
    return true
  }, [])

  const sendMove = useCallback(
    (direction: MoveDirection) => {
      sendJson(buildMoveCommand(direction))
    },
    [sendJson],
  )

  const stopStreamFetch = useCallback(() => {
    setStreamSrc(null)
    setStreamOn(false)
    streamOnRef.current = false
  }, [])

  const setStreamEnabled = useCallback(
    (enabled: boolean) => {
      if (!sendJson(buildStreamCommand(enabled))) {
        return
      }
      streamOnRef.current = enabled
      setStreamOn(enabled)
      if (enabled) {
        setStreamSrc(streamCacheBust(hostRef.current))
      } else {
        setStreamSrc(null)
      }
    },
    [sendJson],
  )

  const disconnect = useCallback(() => {
    const socket = socketRef.current
    socketRef.current = null
    clearWatchdog()
    if (socket) {
      socket.onopen = null
      socket.onclose = null
      socket.onerror = null
      socket.onmessage = null
      if (
        socket.readyState === WebSocket.OPEN ||
        socket.readyState === WebSocket.CONNECTING
      ) {
        socket.close()
      }
    }
    pendingSinceRef.current = null
    pressedRef.current = null
    setActiveDirection(null)
    stopStreamFetch()
    setLastSent(null)
    setLastReply(null)
    setStatus('disconnected')
  }, [clearWatchdog, stopStreamFetch])

  const connect = useCallback(() => {
    const host = normalizeHost(hostInput)
    if (!host) {
      setError('Enter the robot address (robot.local or its IP)')
      setStatus('error')
      return
    }

    disconnect()
    hostRef.current = host
    saveHost(host)
    setError(null)
    setStatus('connecting')
    setLastReply(null)

    const socket = new WebSocket(wsUrl(host))
    socketRef.current = socket

    socket.onopen = () => {
      if (socketRef.current !== socket) {
        return
      }
      setStatus('connected')
      setError(null)
      document.getElementById('robot-host')?.blur()

      /* Camera comes up with the connection: you drive by looking at it. The
       * firmware may still be paused from a previous session, so ask for it
       * explicitly rather than assuming the default. */
      sendJson(buildStreamCommand(true))
      streamOnRef.current = true
      setStreamOn(true)
      setStreamSrc(streamCacheBust(hostRef.current))

      pendingSinceRef.current = null
      clearWatchdog()
      watchdogRef.current = window.setInterval(() => {
        if (socketRef.current !== socket) {
          return
        }
        const pendingSince = pendingSinceRef.current
        if (pendingSince !== null && Date.now() - pendingSince > ACK_TIMEOUT_MS) {
          setError('Robot is not acknowledging commands. Press Connect to retry.')
          setStatus('error')
          clearWatchdog()
        }
      }, 1000)
    }

    socket.onmessage = (event) => {
      if (socketRef.current !== socket) {
        return
      }
      /* Any reply proves the firmware is servicing the socket. */
      pendingSinceRef.current = null
      try {
        const msg = JSON.parse(String(event.data)) as {
          type?: string
          success?: boolean
          code?: string
          message?: string
          commandId?: string
        }
        if (msg.type === 'ack') {
          setLastReply(`ack ${msg.commandId ?? ''}`.trim())
        } else if (msg.type === 'error') {
          setLastReply(`error ${msg.code ?? ''}: ${msg.message ?? ''}`)
          setError(msg.message ?? msg.code ?? 'Command error')
        }
      } catch {
        /* ignore non-JSON (e.g. telemetry noise if any) */
      }
    }

    socket.onerror = () => {
      if (socketRef.current !== socket) {
        return
      }
      setError(
        `Could not reach ${hostRef.current} — same Wi‑Fi? If robot.local fails, type the IP from the serial log`,
      )
      setStatus('error')
    }

    socket.onclose = () => {
      if (socketRef.current !== socket) {
        return
      }
      socketRef.current = null
      clearWatchdog()
      pressedRef.current = null
      setActiveDirection(null)
      stopStreamFetch()
      setStatus((prev) => (prev === 'error' ? 'error' : 'disconnected'))
    }
  }, [clearWatchdog, disconnect, hostInput, sendJson, stopStreamFetch])

  const pressDirection = useCallback(
    (direction: MoveDirection) => {
      if (direction === 'stop') {
        pressedRef.current = null
        setActiveDirection(null)
        sendMove('stop')
        return
      }
      /* The camera deliberately keeps running while driving — seeing where you
       * are going is the whole point. The firmware keeps commands ahead of
       * video (higher-priority task, separate core, TCP_NODELAY) rather than
       * the app having to choose between them. */
      pressedRef.current = direction
      setActiveDirection(direction)
      sendMove(direction)
    },
    [sendMove],
  )

  const releaseDirection = useCallback(
    (direction?: MoveDirection) => {
      if (direction && pressedRef.current && pressedRef.current !== direction) {
        return
      }
      if (pressedRef.current === null) {
        return
      }
      pressedRef.current = null
      setActiveDirection(null)
      sendMove('stop')
    },
    [sendMove],
  )

  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.repeat) {
        return
      }
      const target = event.target as HTMLElement | null
      if (
        target &&
        (target.tagName === 'INPUT' ||
          target.tagName === 'TEXTAREA' ||
          target.isContentEditable)
      ) {
        return
      }
      const direction = keyToDirection(event.key)
      if (!direction) {
        return
      }
      event.preventDefault()
      pressDirection(direction)
    }

    const onKeyUp = (event: KeyboardEvent) => {
      const target = event.target as HTMLElement | null
      if (
        target &&
        (target.tagName === 'INPUT' ||
          target.tagName === 'TEXTAREA' ||
          target.isContentEditable)
      ) {
        return
      }
      const direction = keyToDirection(event.key)
      if (!direction) {
        return
      }
      event.preventDefault()
      releaseDirection(direction)
    }

    const onBlur = () => releaseDirection()

    window.addEventListener('keydown', onKeyDown)
    window.addEventListener('keyup', onKeyUp)
    window.addEventListener('blur', onBlur)
    return () => {
      window.removeEventListener('keydown', onKeyDown)
      window.removeEventListener('keyup', onKeyUp)
      window.removeEventListener('blur', onBlur)
    }
  }, [pressDirection, releaseDirection])

  useEffect(() => () => disconnect(), [disconnect])

  return {
    hostInput,
    setHostInput,
    status,
    error,
    streamSrc,
    streamOn,
    streamPaused: !streamOn,
    activeDirection,
    lastSent,
    lastReply,
    connect,
    disconnect,
    pressDirection,
    releaseDirection,
    setStreamEnabled,
    isConnected: status === 'connected',
  }
}

function streamCacheBust(host: string): string {
  return `http://${host}:81/stream?t=${Date.now()}`
}

function keyToDirection(key: string): MoveDirection | null {
  switch (key) {
    case 'ArrowUp':
    case 'w':
    case 'W':
      return 'forward'
    case 'ArrowDown':
    case 's':
    case 'S':
      return 'reverse'
    case 'ArrowLeft':
    case 'a':
    case 'A':
      return 'left'
    case 'ArrowRight':
    case 'd':
    case 'D':
      return 'right'
    case ' ':
    case 'Escape':
      return 'stop'
    default:
      return null
  }
}
