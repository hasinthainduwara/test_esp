import { CameraPreview } from './components/CameraPreview'
import { ConnectBar } from './components/ConnectBar'
import { DrivePad } from './components/DrivePad'
import { useMjpegStream } from './hooks/useMjpegStream'
import { useRobotSocket } from './hooks/useRobotSocket'

export default function App() {
  const robot = useRobotSocket()
  const stream = useMjpegStream(robot.streamSrc)

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
          canvasRef={stream.canvasRef}
          active={robot.streamOn && stream.hasFrame}
          connected={robot.isConnected}
          streamOn={robot.streamOn}
          fps={stream.fps}
          streamError={stream.error}
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
