import type { ConnectionStatus } from '../lib/robotApi'

type ConnectBarProps = {
  host: string
  status: ConnectionStatus
  error: string | null
  lastSent: string | null
  lastReply: string | null
  onHostChange: (value: string) => void
  onConnect: () => void
  onDisconnect: () => void
}

const STATUS_LABEL: Record<ConnectionStatus, string> = {
  disconnected: 'Offline',
  connecting: 'Connecting…',
  connected: 'Live',
  error: 'Error',
}

export function ConnectBar({
  host,
  status,
  error,
  lastSent,
  lastReply,
  onHostChange,
  onConnect,
  onDisconnect,
}: ConnectBarProps) {
  const connected = status === 'connected'
  const busy = status === 'connecting'

  return (
    <header className="connect">
      <div className="connect__brand">
        <p className="connect__title">Robot Test</p>
        <p className={`connect__status connect__status--${status}`}>
          <span className="connect__dot" aria-hidden="true" />
          {STATUS_LABEL[status]}
        </p>
      </div>

      <form
        className="connect__form"
        onSubmit={(event) => {
          event.preventDefault()
          if (connected) {
            onDisconnect()
          } else {
            onConnect()
          }
        }}
      >
        <label className="connect__label" htmlFor="robot-host">
          Robot address
        </label>
        <div className="connect__row">
          <input
            id="robot-host"
            className="connect__input"
            value={host}
            onChange={(event) => onHostChange(event.target.value)}
            placeholder="robot.local"
            autoComplete="off"
            spellCheck={false}
            disabled={busy || connected}
            inputMode="url"
          />
          <button
            type="submit"
            className={`connect__action${connected ? ' connect__action--stop' : ''}`}
            disabled={busy}
          >
            {busy ? '…' : connected ? 'Disconnect' : 'Connect'}
          </button>
        </div>
      </form>

      {connected ? (
        <p className="connect__trace" aria-live="polite">
          <span>TX {lastSent ?? '—'}</span>
          <span>RX {lastReply ?? '—'}</span>
        </p>
      ) : null}

      {error ? <p className="connect__error" role="alert">{error}</p> : null}
    </header>
  )
}
