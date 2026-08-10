import type { MoveDirection } from '../lib/robotApi'

type DrivePadProps = {
  enabled: boolean
  activeDirection: MoveDirection | null
  onPress: (direction: MoveDirection) => void
  onRelease: (direction?: MoveDirection) => void
}

const BUTTONS: { direction: MoveDirection; label: string; grid: string }[] = [
  { direction: 'forward', label: 'Forward', grid: 'fwd' },
  { direction: 'left', label: 'Left', grid: 'left' },
  { direction: 'stop', label: 'Stop', grid: 'stop' },
  { direction: 'right', label: 'Right', grid: 'right' },
  { direction: 'reverse', label: 'Reverse', grid: 'rev' },
]

export function DrivePad({
  enabled,
  activeDirection,
  onPress,
  onRelease,
}: DrivePadProps) {
  return (
    <section className="drive" aria-label="Motor controls">
      <p className="drive__hint">Hold to drive · release to stop · WASD / arrows</p>
      <div className="drive__pad">
        {BUTTONS.map(({ direction, label, grid }) => {
          const pressed = activeDirection === direction
          return (
            <button
              key={direction}
              type="button"
              className={`drive__btn drive__btn--${grid}${pressed ? ' is-active' : ''}`}
              aria-label={label}
              disabled={!enabled}
              onPointerDown={(event) => {
                event.preventDefault()
                event.currentTarget.setPointerCapture(event.pointerId)
                onPress(direction)
              }}
              onPointerUp={() => {
                if (direction === 'stop') {
                  return
                }
                onRelease(direction)
              }}
              onPointerCancel={() => onRelease(direction)}
              onLostPointerCapture={() => {
                if (direction !== 'stop') {
                  onRelease(direction)
                }
              }}
            >
              <PadIcon direction={direction} />
              <span>{label}</span>
            </button>
          )
        })}
      </div>
    </section>
  )
}

function PadIcon({ direction }: { direction: MoveDirection }) {
  const paths: Record<MoveDirection, string> = {
    forward: 'M12 5 L19 14 H5 Z',
    reverse: 'M12 19 L5 10 H19 Z',
    left: 'M5 12 L14 5 V19 Z',
    right: 'M19 12 L10 5 V19 Z',
    stop: 'M8 8 H16 V16 H8 Z',
  }

  return (
    <svg viewBox="0 0 24 24" aria-hidden="true" className="drive__icon">
      <path d={paths[direction]} />
    </svg>
  )
}
