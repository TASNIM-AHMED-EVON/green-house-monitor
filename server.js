require('dotenv').config();
const express     = require('express');
const http        = require('http');
const { Server }  = require('socket.io');
const TelegramBot = require('node-telegram-bot-api');

const app    = express();
const server = http.createServer(app);
const io     = new Server(server);

app.use(express.json());
app.use(express.static('public'));

// ─── Thresholds (tune after MQ-135 calibration) ──────────────────────────────
const THRESHOLDS = {
  tempHigh:     30,    // °C — auto mode turns fan ON above this
  humidityHigh: 80,    // %
  aqAlert:      600,   // ADC 12-bit value (0–4095)
  co2PumpAlert: 700,   // ADC value — mirrors CO2_PUMP_THRESHOLD in firmware (no O2 sensor: CO2 proxy)
};

const MAX_HISTORY    = 50;
const ALERT_COOLDOWN = 5 * 60 * 1000;  // 5 min between Telegram alerts

// ─── In-memory state ─────────────────────────────────────────────────────────
let history   = [];
let lastAlert = 0;

// This is the single source of truth for what the ESP32 should do.
// ESP32 reads this on every POST response.
// Browser writes this via Socket.io.
let cmdState = {
  mode: 'auto',      // 'auto' | 'manual'
  commands: {
    fan:       false,
    light:     false,
    buzzer:    false,
    pump:      false,
    doorClose: false,  // momentary trigger — auto-resets after being sent once
    doorOpen:  false,  // momentary trigger — auto-resets after being sent once
  },
};

// ─── Telegram (optional) ─────────────────────────────────────────────────────
const bot = process.env.TELEGRAM_TOKEN
  ? new TelegramBot(process.env.TELEGRAM_TOKEN, { polling: false })
  : null;
const CHAT_ID = process.env.TELEGRAM_CHAT_ID || '';

// ─── Routes ──────────────────────────────────────────────────────────────────

// ESP32 POSTs sensor data here every ~5 seconds.
// Response carries the current cmdState so ESP32 knows what to do.
app.post('/api/data', (req, res) => {
  const { temp, humidity, aq, occupied, pump, doorOpen } = req.body;

  if (temp == null || humidity == null || aq == null) {
    return res.status(400).json({ error: 'Missing fields: temp, humidity, aq required' });
  }

  const entry = {
    temp:     parseFloat(temp),
    humidity: parseFloat(humidity),
    aq:       parseInt(aq),
    occupied: !!occupied,
    pump:     !!pump,
    doorOpen: !!doorOpen,
    time:     new Date().toISOString(),
  };

  history.push(entry);
  if (history.length > MAX_HISTORY) history.shift();

  // Push live reading to all browser dashboards
  io.emit('reading', entry);

  // Check thresholds → Telegram alert
  checkAndAlert(entry);

  // Return current command state → ESP32 applies this to hardware
  res.json({
    ok:       true,
    mode:     cmdState.mode,
    commands: cmdState.commands,
  });

  // doorClose / doorOpen are one-shot triggers (servo actions), not
  // persistent toggles like fan/light/pump — reset them right after
  // this response goes out so the ESP32 doesn't re-trigger the servo
  // on every subsequent POST.
  if (cmdState.commands.doorClose || cmdState.commands.doorOpen) {
    cmdState.commands.doorClose = false;
    cmdState.commands.doorOpen  = false;
    io.emit('commandState', cmdState); // keep dashboards in sync (button un-presses)
  }
});

// Dashboard fetches existing readings when page opens
app.get('/api/history',  (_req, res) => res.json(history));

// Dashboard fetches current command state when page opens
app.get('/api/commands', (_req, res) => res.json(cmdState));

// ─── Socket.io — browser ↔ server ────────────────────────────────────────────
io.on('connection', (socket) => {
  console.log(`[dashboard] connected  ${socket.id}`);

  // Send current state immediately to newly connected browser
  socket.emit('commandState', cmdState);

  // Browser sends a command. Two shapes:
  //   { mode: 'manual' }                  — switch mode
  //   { device: 'fan', value: true }      — toggle a device (manual only)
  socket.on('setCommand', (data) => {
    if (data.mode !== undefined) {
      cmdState.mode = data.mode === 'manual' ? 'manual' : 'auto';
      console.log(`[cmd] mode → ${cmdState.mode}`);
    }

    if (
      data.device !== undefined &&
      cmdState.commands[data.device] !== undefined
    ) {
      cmdState.commands[data.device] = !!data.value;
      console.log(`[cmd] ${data.device} → ${data.value}`);
    }

    // Broadcast updated state to ALL connected dashboards (multi-tab safe)
    io.emit('commandState', cmdState);
  });

  socket.on('disconnect', () => {
    console.log(`[dashboard] disconnected ${socket.id}`);
  });
});

// ─── Telegram alerts ─────────────────────────────────────────────────────────
function checkAndAlert(d) {
  if (!bot || !CHAT_ID) return;
  const now = Date.now();
  if (now - lastAlert < ALERT_COOLDOWN) return;

  const msgs = [];
  if (d.temp     > THRESHOLDS.tempHigh)     msgs.push(`🌡 High temp: ${d.temp.toFixed(1)}°C`);
  if (d.humidity > THRESHOLDS.humidityHigh) msgs.push(`💧 High humidity: ${d.humidity.toFixed(1)}%`);
  if (d.aq       > THRESHOLDS.aqAlert)      msgs.push(`😷 Poor air quality (AQ: ${d.aq})`);
  if (d.pump)                               msgs.push(`🫧 O2 pump auto-activated (CO2 proxy AQ: ${d.aq})`);
  if (d.doorOpen)                           msgs.push(`🚪 Door is open`);

  if (!msgs.length) return;

  bot.sendMessage(
    CHAT_ID,
    `⚠️ *Smart Room Alert*\n\n${msgs.join('\n')}\n\n_${new Date().toLocaleString()}_`,
    { parse_mode: 'Markdown' }
  ).catch(err => console.error('[telegram]', err.message));

  lastAlert = now;
}

// ─── Start ───────────────────────────────────────────────────────────────────
const PORT = process.env.PORT || 3000;
server.listen(PORT, () => {
  console.log(`Smart Room Monitor → http://localhost:${PORT}`);
  console.log(`Telegram alerts: ${bot ? 'enabled' : 'disabled (set TELEGRAM_TOKEN to enable)'}`);
});