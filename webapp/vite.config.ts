import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// https://vite.dev/config/
export default defineConfig({
  plugins: [react()],
  // Listen on the LAN so a phone on the same router can open the UI too,
  // not just the laptop running the dev server.
  server: { host: true },
})
