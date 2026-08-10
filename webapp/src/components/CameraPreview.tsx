import type { RefObject } from 'react'

type CameraPreviewProps = {
  canvasRef: RefObject<HTMLCanvasElement | null>
  active: boolean
  connected: boolean
  streamOn: boolean
  fps: number
  streamError: string | null
  onToggleStream: (enabled: boolean) => void
}

export function CameraPreview({
  canvasRef,
  active,
  connected,
  streamOn,
  fps,
  streamError,
  onToggleStream,
}: CameraPreviewProps) {
  return (
    <section className="camera" aria-label="Camera preview">
      {/* The canvas stays mounted while streaming so its ref is available
          before the first frame arrives; the placeholder sits on top until
          there is something to show. */}
      <canvas
        className="camera__feed"
        ref={canvasRef}
        hidden={!active}
        aria-label="Robot camera stream"
      />
      {!active ? (
        <div className="camera__placeholder">
          <span className="camera__mark">ROBOT</span>
          <p>
            {!connected
              ? 'Connect for live video and motor control'
              : streamOn
                ? streamError
                  ? `Reconnecting to camera — ${streamError}`
                  : 'Waiting for first frame…'
                : 'Camera paused — press Start camera'}
          </p>
        </div>
      ) : null}
      <div className="camera__vignette" aria-hidden="true" />
      {connected ? (
        <>
          {active ? <span className="camera__fps">{fps} fps</span> : null}
          <button
            type="button"
            className="camera__toggle"
            onClick={() => onToggleStream(!streamOn)}
          >
            {streamOn ? 'Stop camera' : 'Start camera'}
          </button>
        </>
      ) : null}
    </section>
  )
}
