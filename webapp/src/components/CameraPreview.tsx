type CameraPreviewProps = {
  src: string | null
  connected: boolean
  streamOn: boolean
  onToggleStream: (enabled: boolean) => void
}

export function CameraPreview({
  src,
  connected,
  streamOn,
  onToggleStream,
}: CameraPreviewProps) {
  return (
    <section className="camera" aria-label="Camera preview">
      {src ? (
        <img
          className="camera__feed"
          src={src}
          alt="Robot camera stream"
          draggable={false}
        />
      ) : (
        <div className="camera__placeholder">
          <span className="camera__mark">ROBOT</span>
          <p>
            {!connected
              ? 'Connect for motor control'
              : 'Camera off (saves Wi‑Fi for motors)'}
          </p>
        </div>
      )}
      <div className="camera__vignette" aria-hidden="true" />
      {connected ? (
        <button
          type="button"
          className="camera__toggle"
          onClick={() => onToggleStream(!streamOn)}
        >
          {streamOn ? 'Stop camera' : 'Start camera'}
        </button>
      ) : null}
    </section>
  )
}
