# KIK Simulator

A Node.js server that mimics the ESP32 Central Receiver so you can preview
and develop the full-colour dashboard in any browser **without any hardware**.

## Run

```bash
cd simulator
npm install
npm start
```

Open: <http://localhost:8080>

- `/` — main tablet dashboard
- `/tv.html` — fullscreen leaderboard TV mode
- `/instructor.html` — instructor view

Auto-hits fire every second or so. Pause with:

```bash
curl -X POST http://localhost:8080/api/sim/toggle
```

Fire a manual hit on a specific target:

```bash
curl "http://localhost:8080/api/sim/hit?target=3"
```
