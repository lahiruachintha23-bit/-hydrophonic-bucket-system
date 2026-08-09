// Build marker. If the Console doesn't print this line on page load, the
// browser is running a CACHED older script.js and none of the current fixes are
// active — which looks exactly like "the buttons do nothing".
const DASHBOARD_BUILD = 'dosing-pump-v2';
console.log(`[dashboard] script.js loaded — build ${DASHBOARD_BUILD}`);

let liveDataInterval = null;
let chartInstances = {};
let sensorHistory = [];

const chartColor = '#00e5a0';

// ========== Deployment Mode Detection ==========
const IS_NETLIFY = !(
    window.location.hostname === 'localhost' ||
    window.location.hostname === '127.0.0.1' ||
    /^192\.168\.\d+\.\d+$/.test(window.location.hostname) ||
    /^10\.\d+\.\d+\.\d+$/.test(window.location.hostname) ||
    /^172\.(1[6-9]|2\d|3[01])\.\d+\.\d+$/.test(window.location.hostname)
);

const DATA_SOURCE_LABEL = IS_NETLIFY ? 'Firebase Cloud' : 'Local ESP32';
console.log(`Running in ${IS_NETLIFY ? 'NETLIFY (Firebase Cloud)' : 'LOCAL (ESP32 Direct)'} mode`);

// ========== ApexCharts Config ==========
const apexChartOptions = {
    chart: {
        type: 'line',
        toolbar: { show: false },
        zoom: { enabled: false },
        animations: { enabled: true, speed: 400 },
        background: 'transparent',
        sparkline: { enabled: false }
    },
    theme: { mode: 'dark' },
    colors: [chartColor],
    stroke: { curve: 'smooth', width: 2 },
    dataLabels: { enabled: false },
    legend: { show: false },
    grid: {
        borderColor: 'rgba(0, 200, 140, 0.1)',
        strokeDashArray: 3,
        xaxis: { lines: { show: true } },
        yaxis: { lines: { show: true } }
    },
    xaxis: {
        type: 'datetime',
        labels: {
            format: 'HH:mm',
            style: { colors: 'rgba(180, 230, 210, 0.55)', fontSize: '10px', fontFamily: 'Inter' }
        },
        axisBorder: { show: false },
        axisTicks: { show: false }
    },
    yaxis: {
        labels: {
            formatter(value) {
                return Number(value).toFixed(value % 1 === 0 ? 0 : 2);
            },
            style: { colors: 'rgba(180, 230, 210, 0.55)', fontSize: '10px', fontFamily: 'Inter' }
        }
    },
    tooltip: {
        theme: 'dark',
        x: { format: 'dd/MM/yyyy HH:mm:ss' },
        style: { fontSize: '12px', fontFamily: 'Inter' }
    },
    noData: {
        text: 'Waiting for data...',
        style: { color: 'rgba(180, 230, 210, 0.5)', fontSize: '12px', fontFamily: 'Inter' }
    }
};

// ========== Firebase Setup ==========
// Full web config from the Firebase console. With open database rules there is
// no sign-in, and the Realtime Database SDK only actually needs databaseURL —
// the rest is kept as-is so the config matches the console and other Firebase
// features would work if ever added. None of these are secrets; they ship in
// the public bundle by design.
const firebaseConfig = {
    apiKey: "AIzaSyDjj5KLWLrV0kM7gP6eiRLNcxqssXuVThA",
    authDomain: "hydrophonic-bucket-system.firebaseapp.com",
    databaseURL: "https://hydrophonic-bucket-system-default-rtdb.asia-southeast1.firebasedatabase.app",
    projectId: "hydrophonic-bucket-system",
    storageBucket: "hydrophonic-bucket-system.firebasestorage.app",
    messagingSenderId: "1006449836910",
    appId: "1:1006449836910:web:e5c5960a383056b9f3b77f",
    measurementId: "G-2MJMHYG6KB"
};

let db = null;
let firebaseInitialized = false;
let listenersAttached = false;
let freshnessTimer = null;

if (typeof firebase !== 'undefined') {
    try {
        firebase.initializeApp(firebaseConfig);
        db = firebase.database();
        firebaseInitialized = true;
        console.log("Firebase initialized successfully");
    } catch (e) {
        console.error("Firebase init error:", e);
    }
}

// ========== Firebase Startup ==========
// Guarded by listenersAttached so it can be called more than once without
// stacking duplicate handlers, which would double-count history rows.
function startFirebase() {
    if (!firebaseInitialized || !db) {
        setConnection(false, 'Firebase SDK missing');
        return;
    }
    if (!listenersAttached) {
        setupFirebaseListeners();
        // Re-check staleness on a timer too: if the ESP32 dies, no new snapshot
        // arrives, so nothing else would ever notice it went quiet.
        if (!freshnessTimer) freshnessTimer = setInterval(checkLiveFreshness, 10000);
        listenersAttached = true;
    }
    fetchGsmSettingsFromFirebase();
}

// ========== Init ==========
document.addEventListener('DOMContentLoaded', () => {
    initializeCharts();
    setText('csvStatus', DATA_SOURCE_LABEL);

    if (!IS_NETLIFY) {
        // LAN mode: talk to the ESP32 directly, so the dashboard keeps working
        // if the internet (or Firebase) is unavailable.
        //
        // Deliberately no Firebase listeners here. Local polling appends to
        // sensorHistory every 5s while the Firebase history listener replaces
        // that whole array on every push, so running both makes the charts
        // fight each other. Control presses still write to Firebase below, so
        // the cloud view stays in sync either way.
        fetchLiveData();
        checkStatus();
        fetchGsmSettings();
        liveDataInterval = setInterval(fetchLiveData, 5000);
        setInterval(checkStatus, 15000);
        return;
    }

    // Cloud mode: everything goes through Firebase. No sign-in step — the rules
    // are open, so data flows as soon as the SDK connects.
    setConnection(false, 'Connecting...');
    startFirebase();
});

// ========== Firebase Listeners ==========
function setupFirebaseListeners() {
    if (!firebaseInitialized || !db) {
        if (IS_NETLIFY) setConnection(false, 'Firebase SDK missing');
        return;
    }

    // Live sensor data from Firebase (real-time push from ESP32)
    db.ref('sensors/live').on('value', (snapshot) => {
        const data = snapshot.val();
        if (data) {
            // In LAN mode Firebase is a bonus channel, so mark connected here.
            // In cloud mode updateLiveDisplay -> checkLiveFreshness decides,
            // since a cached snapshot alone doesn't mean the device is alive.
            if (!IS_NETLIFY) setConnection(true, 'Connected');
            updateLiveDisplay(data);
        } else {
            // Firebase is connected, but waiting for ESP32 data push
            if (IS_NETLIFY) {
                setConnection(false, 'Waiting for ESP32 Data...');
            }
        }
    }, (error) => {
        console.error("Firebase permission error:", error);
        if (IS_NETLIFY) setConnection(false, 'Firebase Permission Denied');
    });

    // Historical logs — load latest 100 on startup
    db.ref('sensors/history').limitToLast(100).on('value', (snapshot) => {
        const historyObj = snapshot.val();
        if (historyObj) {
            sensorHistory = Object.values(historyObj)
                .sort((a, b) => (a.timestamp || 0) - (b.timestamp || 0))
                .map(item => ({
                    timestamp: item.timestamp ? new Date(item.timestamp) : new Date(),
                    ...item
                }));
            updateCharts();
            setText('debugCSVCount', String(sensorHistory.length));
        }
    });
}

// ========== Control Helpers ==========
// Raw write — used for plain settings objects like /gsm.
async function firebaseSet(path, value) {
    if (!firebaseInitialized || !db) return;
    return db.ref(path).set(value);
}

// Command write — used for /controls/*. The firmware matches on `ts`, not on the
// string, so pressing the same button twice still registers, and a command left
// in the database can't silently re-fire a pump after the ESP32 reboots (on boot
// the device stamps its own "off" and ignores anything older).
//
// ts comes from Firebase's SERVER clock, never the browser's. The firmware only
// applies a command when ts > the last one it applied, so a phone with a slow
// clock would have every press silently ignored, and a phone running fast would
// push that watermark into the future and deaden the controls until real time
// caught up. Server time keeps the device and every client on one timeline.
// Rules still see a number: the placeholder resolves before .validate runs.
// Shape is enforced by database.rules.json: {state: string<=16, ts: number}.
async function firebaseCommand(path, state) {
    return firebaseSet(path, {
        state,
        ts: firebase.database.ServerValue.TIMESTAMP
    });
}

// ========== Control Buttons ==========
// Controls the peristaltic dosing pump (relay on PUMP_PIN). The Firebase node
// stays "controls/pump" — renaming it would break compatibility with firmware
// that's already deployed and with any stored command; only the UI wording
// changed to "Dosing Pump".
async function controlPump(action) {
    console.log(`[dashboard] controlPump("${action}") clicked — netlify mode: ${IS_NETLIFY}`);
    // Send to Firebase
    try {
        await firebaseCommand('controls/pump', action);
        if (IS_NETLIFY) showToast(`Dosing Pump: ${action.toUpperCase()} sent to Firebase`);
    } catch (e) {
        console.warn("Firebase control set failed:", e);
        // In cloud mode this write IS the only way the command reaches the
        // device — there's no local fallback below. Silently swallowing the
        // error here (as before) made a permission-denied or offline Firebase
        // write look identical to "nothing happened", with no clue why.
        if (IS_NETLIFY) showToast(`Failed to send command: ${e.message || e}`);
    }

    // Send directly to local ESP32 if not on Netlify
    if (!IS_NETLIFY) {
        try {
            const data = await apiPost('/api/pump', action);
            updateControlStates(
                data.pump === 'on',
                data.autoPump === 'enabled',
                data.pumpManualOverride === 'active'
            );
            // Local mode previously gave no visible feedback at all on success —
            // only the small status text changed — which made a working button
            // feel identical to a dead one.
            showToast(`Dosing Pump: ${String(data.pump).toUpperCase()}`);
            await fetchLiveData();
        } catch (error) {
            console.error('Local dosing pump control error:', error);
            alert(`Failed to control dosing pump locally: ${error.message}`);
        }
    }
}

async function controlPumpAuto(action) {
    console.log(`[dashboard] controlPumpAuto("${action}") clicked — netlify mode: ${IS_NETLIFY}`);
    try {
        await firebaseCommand('controls/autoPump', action);
        if (IS_NETLIFY) showToast(`Auto Pump: ${action.toUpperCase()} sent to Firebase`);
    } catch (e) {
        console.warn("Firebase auto pump set failed:", e);
        if (IS_NETLIFY) showToast(`Failed to send command: ${e.message || e}`);
    }

    if (!IS_NETLIFY) {
        try {
            const data = await apiPost('/api/pump/auto', action);
            updateControlStates(
                data.pump === 'on',
                data.autoPump === 'enabled',
                data.pumpManualOverride === 'active'
            );
            showToast(`Auto Dosing: ${String(data.autoPump).toUpperCase()}`);
            await fetchLiveData();
        } catch (error) {
            console.error('Local auto dosing control error:', error);
            alert(`Failed to control auto dosing locally: ${error.message}`);
        }
    }
}

// ========== Toast Notification ==========
function showToast(message) {
    let toast = document.getElementById('toast-notification');
    if (!toast) {
        toast = document.createElement('div');
        toast.id = 'toast-notification';
        toast.style.cssText = `
            position: fixed; bottom: 32px; left: 50%; transform: translateX(-50%);
            background: rgba(0, 229, 160, 0.18); color: #00e5a0;
            border: 1px solid rgba(0, 229, 160, 0.4); border-radius: 12px;
            padding: 12px 24px; font-family: Inter, sans-serif; font-size: 0.85rem;
            font-weight: 600; backdrop-filter: blur(8px); z-index: 9999;
            transition: opacity 0.4s ease;
        `;
        document.body.appendChild(toast);
    }
    toast.textContent = message;
    toast.style.opacity = '1';
    clearTimeout(toast._hideTimer);
    toast._hideTimer = setTimeout(() => { toast.style.opacity = '0'; }, 3000);
}

// ========== GSM Settings ==========
async function fetchGsmSettingsFromFirebase() {
    if (!firebaseInitialized || !db) return;
    db.ref('gsm').once('value', (snapshot) => {
        const data = snapshot.val();
        if (!data) return;
        const phoneInput = document.getElementById('gsmPhoneNumber');
        const msgInput   = document.getElementById('gsmMessage');
        const enabledChk = document.getElementById('gsmEnabled');
        if (phoneInput) phoneInput.value = data.phoneNumber || '';
        if (msgInput)   msgInput.value   = data.message    || '';
        if (enabledChk) enabledChk.checked = !!data.enabled;
    });
}

async function saveGsmSettings() {
    const phoneNumber = document.getElementById('gsmPhoneNumber').value.trim();
    const message     = document.getElementById('gsmMessage').value.trim();
    const enabled     = document.getElementById('gsmEnabled').checked;

    try {
        await firebaseSet('gsm', { phoneNumber, message, enabled });
    } catch (e) {}

    if (IS_NETLIFY) {
        showToast('GSM settings saved to Firebase ✓');
    } else {
        try {
            const data = await apiPostForm('/api/gsm', {
                enabled: enabled ? '1' : '0',
                phoneNumber,
                message
            });
            alert('GSM settings saved successfully');
            if (data.enabled !== undefined) {
                document.getElementById('gsmEnabled').checked = data.enabled;
            }
        } catch (error) {
            console.error('Failed to save GSM settings:', error);
            alert(`Failed to save GSM settings: ${error.message}`);
        }
    }
}

// ========== Refresh & Status ==========
function refreshData() {
    if (!IS_NETLIFY) {
        fetchLiveData();
        checkStatus();
    } else {
        showToast('Dashboard is live — updates automatically via Firebase');
    }
}

async function checkStatus() {
    try {
        const data = await apiGet('/api/status');
        setConnection(true, 'ESP32 Connected');
        setText('csvStatus', 'Local ESP32');
        updateControlStates(
            data.pump === 'on',
            data.autoPump === 'enabled',
            data.pumpManualOverride === 'active'
        );
    } catch (error) {
        console.error('Status check error:', error);
        if (!IS_NETLIFY) setConnection(false, 'Local API Unreachable');
        setText('csvStatus', IS_NETLIFY ? 'Firebase Cloud' : 'Unavailable');
    }
}

// ========== ESP32 Local API Helpers ==========
async function apiGet(url) {
    const response = await fetch(url, { cache: 'no-store' });
    if (!response.ok) throw new Error(`GET ${url} failed: ${response.status}`);
    return response.json();
}

async function apiPost(url, action) {
    // Include action in BOTH query string and body to guarantee ESPAsyncWebServer parsing
    const fullUrl = `${url}?action=${encodeURIComponent(action)}`;
    const response = await fetch(fullUrl, {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
        body: new URLSearchParams({ action }).toString()
    });
    if (!response.ok) {
        const text = await response.text();
        throw new Error(text || `POST ${url} failed: ${response.status}`);
    }
    return response.json();
}

async function apiPostForm(url, params) {
    const fullUrl = `${url}?${new URLSearchParams(params).toString()}`;
    const response = await fetch(fullUrl, {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
        body: new URLSearchParams(params).toString()
    });
    if (!response.ok) {
        const text = await response.text();
        throw new Error(text || `POST ${url} failed: ${response.status}`);
    }
    return response.json();
}

async function fetchLiveData() {
    try {
        const data = await apiGet('/api/sensors');
        setConnection(true, 'ESP32 Connected');
        updateLiveDisplay(data);
        // Spread FIRST, then override timestamp. The device sends its own
        // `timestamp` field as millis() uptime, so spreading last would clobber
        // the real clock and plot every local-mode point in Jan 1970.
        sensorHistory.push({ ...data, timestamp: new Date() });
        if (sensorHistory.length > 100) sensorHistory.shift();
        updateCharts();
    } catch (error) {
        console.error('Fetch live data error:', error);
        setConnection(false, 'Disconnected');
    }
}

async function fetchGsmSettings() {
    try {
        const data = await apiGet('/api/gsm');
        const phoneInput = document.getElementById('gsmPhoneNumber');
        const msgInput   = document.getElementById('gsmMessage');
        const enabledChk = document.getElementById('gsmEnabled');
        if (phoneInput) phoneInput.value = data.phoneNumber || '';
        if (msgInput)   msgInput.value   = data.message    || '';
        if (enabledChk) enabledChk.checked = data.enabled === true;
    } catch (error) {
        console.error('Failed to fetch GSM settings:', error);
    }
}

async function sendManualGsmMessage() {
    const button = document.querySelector('button[onclick="sendManualGsmMessage()"]');
    if (button) {
        button.disabled = true;
        button.textContent = '⏳ Sending...';
    }

    try {
        if (IS_NETLIFY) {
            // The cloud dashboard has no server of its own to POST /api/gsm/send
            // to — that endpoint only exists on the ESP32 itself, which this
            // page isn't talking to directly. Route through Firebase instead,
            // the same way the pump buttons do, and wait briefly for the device
            // to write back a real result.
            await sendGsmCommandViaFirebase();
        } else {
            const data = await apiPostForm('/api/gsm/send', {});
            // The device now queues the send and returns immediately (status
            // "queued") so its web server task can't block and reboot the board.
            if (data.status === 'ok' || data.status === 'queued') {
                showToast(data.message || 'SMS queued');
            } else {
                showToast(data.message || 'SMS send failed');
            }
        }
    } catch (error) {
        console.error('Manual SMS send failed:', error);
        showToast(`Send failed: ${error.message}`);
    } finally {
        if (button) {
            button.disabled = false;
            button.textContent = '📤 Send Now';
        }
    }
}

// Writes the send command to Firebase, then listens on /device/gsmLastSendResult
// for the ESP32 to report what actually happened (it writes {status, message, ts}
// after attempting the send). Falls back to a generic "command sent" toast if the
// device doesn't answer within the timeout — it may be offline, or on an older
// firmware build that doesn't handle /controls/gsmSend yet.
function sendGsmCommandViaFirebase() {
    return new Promise((resolve, reject) => {
        if (!firebaseInitialized || !db) {
            reject(new Error('Firebase not available'));
            return;
        }

        const requestedAt = Date.now();
        const ref = db.ref('device/gsmLastSendResult');
        let settled = false;
        const finish = (message) => {
            if (settled) return;
            settled = true;
            ref.off('value', handler);
            clearTimeout(timer);
            showToast(message);
            resolve();
        };

        firebaseCommand('controls/gsmSend', 'send')
            .then(() => showToast('Send command sent to device…'))
            .catch((e) => {
                console.warn('Firebase gsmSend write failed:', e);
                // Report this immediately rather than letting it fall through to
                // the 15s "no response from device" timeout, which would blame the
                // device for what is actually a rejected write. PERMISSION_DENIED
                // here means the database rules haven't been published with the
                // gsmSend control whitelisted.
                const denied = String(e && (e.code || e.message)).toUpperCase().includes('PERMISSION');
                finish(denied
                    ? 'Firebase rejected the command — publish the updated database rules (gsmSend not allowed yet)'
                    : `Could not send command: ${e.message || e}`);
            });

        const handler = ref.on('value', (snapshot) => {
            const val = snapshot.val();
            // Only trust a result timestamped after we sent the command — an
            // older cached result would otherwise show a stale outcome.
            if (val && val.ts && val.ts >= requestedAt - 5000) {
                finish(val.status === 'ok' ? 'SMS sent successfully' : (val.message || 'SMS send failed'));
            }
        });

        const timer = setTimeout(() => {
            finish('Command sent — no response from device yet, check its status later');
        }, 15000);
    });
}

// ========== Connection State UI ==========
function setConnection(isConnected, customLabel = '') {
    const liveDot = document.getElementById('liveDot');
    const badge   = document.getElementById('esp32Status');

    if (liveDot) {
        if (isConnected) {
            liveDot.classList.add('connected');
            liveDot.textContent = customLabel || (IS_NETLIFY ? 'Firebase Live' : 'Connected');
        } else {
            liveDot.classList.remove('connected');
            liveDot.textContent = customLabel || 'Disconnected';
        }
    }
    if (badge) {
        badge.textContent  = isConnected ? 'Connected' : (customLabel || 'Disconnected');
        badge.style.color  = isConnected ? 'var(--accent)' : '#ff5050';
    }
}

// ========== Display Update ==========
// The ESP32 pushes /sensors/live every 5s. If we haven't heard from it in this
// long, the device is offline even though Firebase itself is connected fine —
// an important distinction when the whole point is remote monitoring.
const LIVE_STALE_MS = 45000;
let lastLiveTimestamp = 0;   // device's own NTP clock, for display
let lastLiveArrivalMs = 0;   // when we received it, for staleness
let liveUpdates = 0;         // how many snapshots we've seen this page load

function checkLiveFreshness() {
    if (!IS_NETLIFY || !lastLiveArrivalMs) return;

    // Two signals, each covering the other's blind spot:
    //  - arrival age is immune to a skewed phone clock, but the first snapshot
    //    is a cached replay that "arrives" now even if the device died hours ago;
    //  - device-timestamp age catches that stale replay, but trusts the browser
    //    clock.
    // So the device timestamp is only consulted until a genuinely fresh push
    // lands; after that, arrival age alone is both sufficient and skew-proof.
    const arrivalAge = Date.now() - lastLiveArrivalMs;
    const deviceAge  = lastLiveTimestamp ? Date.now() - lastLiveTimestamp : 0;
    const age = liveUpdates > 1 ? arrivalAge : Math.max(arrivalAge, deviceAge);

    if (age > LIVE_STALE_MS) {
        const mins = Math.floor(age / 60000);
        const ago = mins >= 60 ? `${Math.floor(mins / 60)}h ago`
                  : mins >= 1  ? `${mins}m ago`
                               : `${Math.floor(age / 1000)}s ago`;
        setConnection(false, `ESP32 offline — last seen ${ago}`);
    } else {
        setConnection(true, 'Firebase Live');
    }
}

function updateLiveDisplay(data) {
    if (data.dhtTemperature !== undefined && data.dhtTemperature !== -1) {
        setHTML('liveTemp', `${Number(data.dhtTemperature).toFixed(2)}<span class="unit">°C</span>`);
    } else {
        setHTML('liveTemp', `--<span class="unit">°C</span>`);
    }

    if (data.dhtHumidity !== undefined && data.dhtHumidity !== -1) {
        setHTML('liveHumidity', `${Number(data.dhtHumidity).toFixed(2)}<span class="unit">%</span>`);
    } else {
        setHTML('liveHumidity', `--<span class="unit">%</span>`);
    }

    setHTML('liveFlow', `${Number(data.flowRate || 0).toFixed(2)}<span class="unit">L/min</span>`);
    setHTML('liveTDS',  `${Number(data.tdsMScm  || 0).toFixed(2)}<span class="unit">mS/cm</span>`);
    setText('liveWaterLevel', data.waterLevel || 'LOW');

    // Prefer the device's own timestamp over the browser clock. Firebase replays
    // the last cached value on connect, so without this a dead ESP32 would keep
    // reporting "Live" with hours-old readings every time the page was opened.
    const deviceTs = Number(data.timestamp) || 0;
    lastLiveTimestamp = deviceTs;
    lastLiveArrivalMs = Date.now();
    liveUpdates++;
    const stamp = deviceTs > 0 ? new Date(deviceTs) : new Date();
    const formattedDate = `${String(stamp.getDate()).padStart(2,'0')}/${String(stamp.getMonth()+1).padStart(2,'0')}/${stamp.getFullYear()}, ${stamp.toLocaleTimeString()}`;
    setText('lastUpdate', formattedDate);
    setText('debugCSVTime', stamp.toLocaleTimeString());
    checkLiveFreshness();

    const isDosingPumpOn = !!data.pumpActive;
    setText('dosingPumpStatus', isDosingPumpOn ? 'Active' : 'Idle');
    updateBadge('dosingPumpBadge', isDosingPumpOn);

    updateControlStates(
        data.pumpActive,
        data.autoPumpEnabled,
        data.pumpManualOverride,
        data.ecLower,
        data.ecUpper
    );
}

function updateBadge(badgeId, isOn) {
    const el = document.getElementById(badgeId);
    if (!el) return;
    el.textContent = isOn ? 'ON' : 'OFF';
    if (isOn) el.classList.add('on');
    else el.classList.remove('on');
}

function updateControlStates(pumpActive, autoPumpEnabled, pumpManualOverride, ecLower, ecUpper) {
    setText('pumpState',          pumpActive      ? 'ON ✓' : 'OFF ✗');
    setText('autoPumpState',      autoPumpEnabled  ? 'ENABLED'   : 'DISABLED');
    setText('autoPumpStateBadge', autoPumpEnabled  ? 'ENABLED ✓' : 'DISABLED ✗');
    setText('manualOverrideStatus', pumpManualOverride ? 'ACTIVE ⚠️' : 'Inactive');

    const ecThresholds = document.getElementById('ecThresholds');
    if (ecThresholds) {
        ecThresholds.textContent = (ecLower !== undefined && ecUpper !== undefined && ecLower > 0)
            ? `${ecLower.toFixed(1)} - ${ecUpper.toFixed(1)} mS/cm`
            : '2.2 - 2.8 mS/cm';
    }
}

function setText(id, value) {
    const el = document.getElementById(id);
    if (el) el.textContent = value;
}

function setHTML(id, html) {
    const el = document.getElementById(id);
    if (el) el.innerHTML = html;
}

// ========== ApexCharts ==========
function initializeCharts() {
    createChart('chartTemperature', 'temperature', 'Air Temperature (°C)');
    createChart('chartHumidity',    'humidity',    'Humidity (%)');
    createChart('chartWaterTemp',   'waterTemp',   'Water Temperature (°C)');
    createChart('chartWaterLevel',  'waterLevel',  'Water Level');
    createChart('chartTDS',         'tds',         'EC (mS/cm)');
    createChart('chartFlow',        'flow',        'Flow Rate (L/min)');
}

function createChart(elementId, key, seriesName) {
    const el = document.getElementById(elementId);
    if (!el) return;

    const customOptions = JSON.parse(JSON.stringify(apexChartOptions));
    customOptions.series = [{ name: seriesName, data: [] }];
    customOptions.chart.height = 200;

    if (key === 'waterLevel') {
        customOptions.yaxis = {
            min: 0, max: 1, tickAmount: 1,
            labels: {
                formatter(value) { return Number(value) >= 0.5 ? 'HIGH' : 'LOW'; },
                style: { colors: 'rgba(180, 230, 210, 0.55)', fontSize: '10px', fontFamily: 'Inter' }
            }
        };
        customOptions.tooltip.y = {
            formatter(value) { return Number(value) >= 0.5 ? 'HIGH' : 'LOW'; }
        };
    }

    chartInstances[key] = new ApexCharts(el, customOptions);
    chartInstances[key].render();
}

function updateCharts() {
    if (!sensorHistory.length) return;
    const timestamps = sensorHistory.map(row => new Date(row.timestamp).getTime());

    updateSeries('temperature', 'Air Temperature (°C)', timestamps,
        sensorHistory.map(d => (d.dhtTemperature !== -1 && d.dhtTemperature !== undefined) ? d.dhtTemperature : null));

    updateSeries('humidity', 'Humidity (%)', timestamps,
        sensorHistory.map(d => (d.dhtHumidity !== -1 && d.dhtHumidity !== undefined) ? d.dhtHumidity : null));

    updateSeries('waterTemp', 'Water Temp (°C)', timestamps,
        sensorHistory.map(d => (d.waterTemperature !== -1 && d.waterTemperature !== undefined) ? d.waterTemperature : null));

    updateSeries('waterLevel', 'Water Level', timestamps,
        sensorHistory.map(d => Number(d.waterLevelValue ?? (d.waterLevel === 'HIGH' ? 1 : 0))));

    updateSeries('tds', 'EC (mS/cm)', timestamps,
        sensorHistory.map(d => Number(d.tdsMScm || 0)));

    updateSeries('flow', 'Flow Rate (L/min)', timestamps,
        sensorHistory.map(d => Number(d.flowRate || 0)));
}

function updateSeries(chartKey, name, timestamps, values) {
    const chart = chartInstances[chartKey];
    if (!chart) return;
    chart.updateSeries([{ name, data: values.map((y, i) => ({ x: timestamps[i], y })) }]);
}

// ========== CSV Download Export ==========
// Hard cap on an export pull. The history node grows ~1440 rows/day forever, so
// an unbounded once('value') would eventually download tens of MB — painful on
// mobile data, which is the main way this dashboard gets used.
const CSV_MAX_ROWS = 5000;

function downloadCSV() {
    if (!sensorHistory.length) {
        if (IS_NETLIFY && firebaseInitialized && db) {
            showToast('Fetching history from Firebase...');
            db.ref('sensors/history')
              .orderByChild('timestamp')
              .limitToLast(CSV_MAX_ROWS)
              .once('value')
              .then((snapshot) => {
                  const historyObj = snapshot.val();
                  if (!historyObj) { alert('No history data in Firebase yet.'); return; }
                  const allHistory = Object.values(historyObj)
                      .sort((a, b) => (a.timestamp || 0) - (b.timestamp || 0));
                  exportCSV(allHistory);
                  showToast(`Exported ${allHistory.length} rows`);
              })
              .catch((e) => {
                  console.error('History export failed:', e);
                  alert(`Could not fetch history: ${e.message}`);
              });
        } else {
            alert('No historical sensor data available to export.');
        }
        return;
    }
    exportCSV(sensorHistory);
}

// Quote anything containing a comma, quote or newline, and neutralise values a
// spreadsheet would treat as a formula.
function csvCell(value) {
    let s = (value === undefined || value === null) ? '' : String(value);
    if (/^[=+\-@\t\r]/.test(s)) s = `'${s}`;
    if (/[",\n\r]/.test(s)) s = `"${s.replace(/"/g, '""')}"`;
    return s;
}

function exportCSV(rows) {
    const headers = ['Timestamp', 'Air Temp (C)', 'Humidity (%)', 'Water Temp (C)', 'Water Level', 'EC (mS/cm)', 'Flow Rate (L/min)'];
    const csvRows = rows.map(row => [
        row.timestamp ? new Date(row.timestamp).toISOString() : '',
        row.dhtTemperature   ?? '',
        row.dhtHumidity      ?? '',
        row.waterTemperature ?? '',
        row.waterLevel       ?? '',
        row.tdsMScm          ?? '',
        row.flowRate         ?? ''
    ].map(csvCell));

    // Leading BOM so Excel opens it as UTF-8 rather than mangling characters.
    const csvContent = '﻿' + [headers.map(csvCell).join(','), ...csvRows.map(e => e.join(','))].join('\r\n');
    const blob = new Blob([csvContent], { type: 'text/csv;charset=utf-8;' });
    const url  = URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.setAttribute('href', url);
    link.setAttribute('download', `greenhouse_report_${new Date().toISOString().slice(0,10)}.csv`);
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
    URL.revokeObjectURL(url);   // otherwise the blob is held until page unload
}

window.addEventListener('beforeunload', () => {
    clearInterval(liveDataInterval);
});