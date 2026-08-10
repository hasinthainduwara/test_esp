import { CameraPreview } from './components/CameraPreview'
import { ConnectBar } from './components/ConnectBar'
import { DrivePad } from './components/DrivePad'
import { useRobotSocket } from './hooks/useRobotSocket'

export default function App() {
  const robot = useRobotSocket()

  return (
    <div className="shell">
      <div className="shell__glow" aria-hidden="true" />
      <div className="app">
        <ConnectBar
          host={robot.hostInput}
          status={robot.status}
          error={robot.error}
          lastSent={robot.lastSent}
          lastReply={robot.lastReply}
          onHostChange={robot.setHostInput}
          onConnect={robot.connect}
          onDisconnect={robot.disconnect}
        />
        <CameraPreview
          src={robot.streamSrc}
          connected={robot.isConnected}
          streamOn={robot.streamOn}
          onToggleStream={robot.setStreamEnabled}
        />
        <DrivePad
          enabled={robot.isConnected}
          activeDirection={robot.activeDirection}
          onPress={robot.pressDirection}
          onRelease={robot.releaseDirection}
        />
      </div>
    </div>
  )
}
