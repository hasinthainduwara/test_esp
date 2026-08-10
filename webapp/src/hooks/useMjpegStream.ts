import { useEffect, useRef, useState } from 'react'

/**
 * Renders an MJPEG (multipart/x-mixed-replace) stream into a <canvas>.
 *
 * Deliberately not an <img src={streamUrl}>. That is less code, but when the
 * stream aborts mid-part the browser frequently reports neither `load` nor
 * `error` — the picture simply freezes or goes black with no way to notice or
 * recover. Reading the response ourselves makes a stall observable (no frame
 * for STALL_MS) and reconnection something we control.
 */

/** No decoded frame for this long ⇒ assume the stream is dead and reconnect. */
const STALL_MS = 4000
const RECONNECT_DELAY_MS = 700

const HEADER_END = [13, 10, 13, 10] /* \r\n\r\n */

/* ReadableStream yields Uint8Array<ArrayBufferLike>, which is wider than the
 * Uint8Array<ArrayBuffer> a plain `new Uint8Array()` infers. */
type Bytes = Uint8Array<ArrayBufferLike>

function indexOfSeq(buf: Bytes, seq: number[], from = 0): number {
  outer: for (let i = from; i <= buf.length - seq.length; i++) {
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
 * Pull one complete part off the front of the buffer. Relies on the firmware
 * sending Content-Length per part, which makes framing exact rather than
 * requiring a scan for the next boundary.
 */
function takeFrame(buf: Bytes): TakeResult {
  const headerEnd = indexOfSeq(buf, HEADER_END)
  if (headerEnd < 0) {
    return null
  }

  const headerText = new TextDecoder().decode(buf.subarray(0, headerEnd))
  const match = /content-length:\s*(\d+)/i.exec(headerText)
  if (!match) {
    /* Unparseable part header — skip it and resync on the next one. */
    return { jpeg: null, rest: buf.subarray(headerEnd + HEADER_END.length) }
  }

  const length = Number.parseInt(match[1], 10)
  const start = headerEnd + HEADER_END.length
  if (buf.length < start + length) {
    return null /* frame not fully arrived yet */
  }

  return { jpeg: buf.subarray(start, start + length), rest: buf.subarray(start + length) }
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

    const drawFrame = async (jpeg: Bytes) => {
      const canvas = canvasRef.current
      if (!canvas) {
        return
      }
      /* Copy out of the stream buffer: the Blob must own its bytes. */
      const blob = new Blob([new Uint8Array(jpeg)], { type: 'image/jpeg' })
      let bitmap: ImageBitmap
      try {
        bitmap = await createImageBitmap(blob)
      } catch {
        return /* a torn frame — skip it, the next one will be fine */
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
      /* Must be set before the fetch, not after it resolves. The watchdog runs
       * on its own independent interval and only knows an attempt is "new"
       * because of this timestamp — leaving it stale from the previous attempt
       * meant the watchdog could abort a brand-new connection before it even
       * finished its TCP handshake, on every one of its own 1 s ticks. That
       * produced a self-sustaining abort loop (visible in DevTools as a run of
       * ~300 ms "(canceled)" fetches: each attempt survived only until the next
       * watchdog tick, never long enough to receive a frame). */
      lastFrameAt = Date.now()
      controller = new AbortController()
      const signal = controller.signal
      /* Fresh query each attempt so no proxy or cache can replay a dead stream. */
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
              await drawFrame(taken.jpeg)
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

    /* Watchdog: the read above can hang without erroring if the peer vanishes
     * without closing the socket. Aborting forces the catch path to reconnect. */
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
