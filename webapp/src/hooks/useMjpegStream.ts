import { useEffect, useRef, useState } from 'react'

/**
 * Renders an MJPEG (multipart/x-mixed-replace) stream into a <canvas>.
 *
 * Uses explicit `--frame` boundary resynchronization to prevent false matches
 * when binary JPEG payloads contain `\r\n\r\n` (0x0D 0x0A 0x0D 0x0A).
 * Decouples stream reading from canvas rendering to prevent TCP socket backpressure.
 */

/** No decoded frame for this long ⇒ assume the stream is dead and reconnect. */
const STALL_MS = 6000
const RECONNECT_DELAY_MS = 700

const FRAME_MARKER = new TextEncoder().encode('--frame')
const HEADER_END = new Uint8Array([13, 10, 13, 10]) /* \r\n\r\n */

type Bytes = Uint8Array<ArrayBufferLike>

function indexOfSeq(buf: Bytes, seq: Uint8Array): number {
  outer: for (let i = 0; i <= buf.length - seq.length; i++) {
    for (let j = 0; j < seq.length; j++) {
      if (buf[i + j] !== seq[j]) {
        continue outer
      }
    }
    return i
  }
  return -1
}

function concat(a: Bytes, b: Bytes): Bytes {
  const out = new Uint8Array(a.length + b.length)
  out.set(a, 0)
  out.set(b, a.length)
  return out
}

type TakeResult = { jpeg: Bytes | null; rest: Bytes } | null

/**
 * Pull one complete part off the front of the buffer.
 * Anchors strictly to `--frame` to ensure binary JPEG data is never mistaken for headers.
 */
function takeFrame(buf: Bytes): TakeResult {
  const markerIdx = indexOfSeq(buf, FRAME_MARKER)
  if (markerIdx < 0) {
    // Keep last 16 bytes in case `--frame` was partially received across reads
    const keep = Math.min(buf.length, 16)
    return { jpeg: null, rest: buf.subarray(buf.length - keep) }
  }

  // Trim any stray bytes before `--frame`
  const framedBuf = markerIdx > 0 ? buf.subarray(markerIdx) : buf

  const headerEnd = indexOfSeq(framedBuf, HEADER_END)
  if (headerEnd < 0) {
    return null // Frame header not fully arrived yet
  }

  const headerText = new TextDecoder().decode(framedBuf.subarray(0, headerEnd))
  const match = /content-length:\s*(\d+)/i.exec(headerText)
  if (!match) {
    // Unparseable header — skip past this `--frame` marker to resync on the next boundary
    return { jpeg: null, rest: framedBuf.subarray(FRAME_MARKER.length) }
  }

  const length = Number.parseInt(match[1], 10)
  const start = headerEnd + HEADER_END.length
  if (framedBuf.length < start + length) {
    return null // Full JPEG payload not arrived yet
  }

  return {
    jpeg: framedBuf.subarray(start, start + length),
    rest: framedBuf.subarray(start + length),
  }
}

export function useMjpegStream(url: string | null) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null)
  const [fps, setFps] = useState(0)
  const [hasFrame, setHasFrame] = useState(false)
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    if (!url) {
      setFps(0)
      setHasFrame(false)
      setError(null)
      return
    }

    let cancelled = false
    let controller: AbortController | null = null
    let reconnectTimer: number | null = null
    let lastFrameAt = Date.now()
    let framesInWindow = 0
    let windowStart = Date.now()

    let pendingJpeg: Bytes | null = null
    let isDrawing = false

    const drawFrame = async (jpeg: Bytes) => {
      const canvas = canvasRef.current
      if (!canvas) {
        return
      }
      const blob = new Blob([jpeg], { type: 'image/jpeg' })
      let bitmap: ImageBitmap
      try {
        bitmap = await createImageBitmap(blob)
      } catch {
        return /* skip torn frame */
      }
      if (cancelled) {
        bitmap.close()
        return
      }
      if (canvas.width !== bitmap.width || canvas.height !== bitmap.height) {
        canvas.width = bitmap.width
        canvas.height = bitmap.height
      }
      canvas.getContext('2d')?.drawImage(bitmap, 0, 0)
      bitmap.close()
      setHasFrame(true)

      lastFrameAt = Date.now()
      framesInWindow += 1
      const elapsed = lastFrameAt - windowStart
      if (elapsed >= 1000) {
        setFps(Math.round((framesInWindow * 1000) / elapsed))
        framesInWindow = 0
        windowStart = lastFrameAt
      }
    }

    const processDrawQueue = async () => {
      if (isDrawing) return
      isDrawing = true
      while (pendingJpeg && !cancelled) {
        const current = pendingJpeg
        pendingJpeg = null
        await drawFrame(current)
      }
      isDrawing = false
    }

    const scheduleReconnect = () => {
      if (cancelled || reconnectTimer !== null) {
        return
      }
      reconnectTimer = window.setTimeout(() => {
        reconnectTimer = null
        void run()
      }, RECONNECT_DELAY_MS)
    }

    const run = async () => {
      if (cancelled) {
        return
      }
      lastFrameAt = Date.now()
      controller = new AbortController()
      const signal = controller.signal
      const attemptUrl = `${url}${url.includes('?') ? '&' : '?'}r=${Date.now()}`

      try {
        const response = await fetch(attemptUrl, { signal, cache: 'no-store' })
        if (!response.ok || !response.body) {
          throw new Error(`HTTP ${response.status}`)
        }

        setError(null)
        lastFrameAt = Date.now()
        const reader = response.body.getReader()
        let buf: Bytes = new Uint8Array(0)

        for (;;) {
          const { done, value } = await reader.read()
          if (cancelled) {
            return
          }
          if (done) {
            throw new Error('stream ended')
          }
          buf = concat(buf, value)

          for (;;) {
            const taken = takeFrame(buf)
            if (!taken) {
              break
            }
            buf = taken.rest
            if (taken.jpeg) {
              // Copy bytes to safe array so concat buffer can be re-used
              pendingJpeg = new Uint8Array(taken.jpeg)
              void processDrawQueue()
            }
          }
        }
      } catch (err) {
        if (cancelled || (err instanceof DOMException && err.name === 'AbortError')) {
          return
        }
        setError(err instanceof Error ? err.message : 'stream failed')
        setFps(0)
        scheduleReconnect()
      }
    }

    const watchdog = window.setInterval(() => {
      if (!cancelled && Date.now() - lastFrameAt > STALL_MS) {
        setFps(0)
        setHasFrame(false)
        controller?.abort()
        scheduleReconnect()
      }
    }, 1000)

    void run()

    return () => {
      cancelled = true
      clearInterval(watchdog)
      if (reconnectTimer !== null) {
        clearTimeout(reconnectTimer)
      }
      controller?.abort()
    }
  }, [url])

  return { canvasRef, fps, hasFrame, error }
}

