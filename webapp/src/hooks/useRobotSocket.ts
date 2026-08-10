import { useCallback, useEffect, useRef, useState } from 'react'
import {
  buildMoveCommand,
  buildStreamCommand,
  normalizeHost,
  type ConnectionStatus,
  type MoveDirection,
  wsUrl,
} from '../lib/robotApi'

const DEFAULT_HOST = '192.168.4.1'

export function useRobotSocket() {
  const [hostInput, setHostInput] = useState(DEFAULT_HOST)
  const [status, setStatus] = useState<ConnectionStatus>('disconnected')
  const [error, setError] = useState<string | null>(null)
  const [streamSrc, setStreamSrc] = useState<string | null>(null)
  const [streamOn, setStreamOn] = useState(false)
  const [activeDirection, setActiveDirection] = useState<MoveDirection | null>(null)
  const [lastSent, setLastSent] = useState<string | null>(null)
  const [lastReply, setLastReply] = useState<string | null>(null)

  const socketRef = useRef<WebSocket | null>(null)
  const hostRef = useRef(DEFAULT_HOST)
  const pressedRef = useRef<MoveDirection | null>(null)
  const streamOnRef = useRef(false)

  const sendJson = useCallback((payload: object): boolean => {
    const socket = socketRef.current
    if (!socket || socket.readyState !== WebSocket.OPEN) {
      setError('Not connected — press Connect first')
      return false
    }
    const text = JSON.stringify(payload)
    socket.send(text)
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
    pressedRef.current = null
    setActiveDirection(null)
    stopStreamFetch()
    setLastSent(null)
    setLastReply(null)
    setStatus('disconnected')
  }, [stopStreamFetch])

  const connect = useCallback(() => {
    const host = normalizeHost(hostInput)
    if (!host) {
      setError('Enter a robot IP address')
      setStatus('error')
      return
    }

    disconnect()
    hostRef.current = host
    setError(null)
    setStatus('connecting')
    setLastReply(null)

    const socket = new WebSocket(wsUrl(host))
    socketRef.current = socket

    socket.onopen = () => {
      if (socketRef.current !== socket) {
        return
      }
      /* Testing default: WS only — do not open MJPEG (keeps Wi‑Fi free for motors). */
      stopStreamFetch()
      setStatus('connected')
      setError(null)
      document.getElementById('robot-host')?.blur()
    }

    socket.onmessage = (event) => {
      if (socketRef.current !== socket) {
        return
      }
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
      setError('WebSocket failed — check Wi‑Fi and IP')
      setStatus('error')
    }

    socket.onclose = () => {
      if (socketRef.current !== socket) {
        return
      }
      socketRef.current = null
      pressedRef.current = null
      setActiveDirection(null)
      stopStreamFetch()
      setStatus((prev) => (prev === 'error' ? 'error' : 'disconnected'))
    }
  }, [disconnect, hostInput, stopStreamFetch])

  const pressDirection = useCallback(
    (direction: MoveDirection) => {
      if (direction === 'stop') {
        pressedRef.current = null
        setActiveDirection(null)
        sendMove('stop')
        return
      }
      /* If preview is on, pause frames while driving so commands stay responsive. */
      if (streamOnRef.current && !pressedRef.current) {
        sendJson(buildStreamCommand(false))
        setStreamSrc(null)
        setStreamOn(false)
        streamOnRef.current = false
      }
      pressedRef.current = direction
      setActiveDirection(direction)
      sendMove(direction)
    },
    [sendJson, sendMove],
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
