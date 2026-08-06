let liveDataInterval = null;
let chartInstances = {};
let sensorHistory = [];

const chartColor = '#00e5a0';

// ========== Deployment Mode Detection ==========
// If NOT running on an ESP32 local IP, we're on Netlify (or another remote host).
// In Netlify mode: Firebase is the ONLY data source & all controls go through Firebase.
// In Local mode: ESP32 local API is primary, Firebase supplements for history.
const IS_NETLIFY = !(
    window.location.hostname === 'localhost' ||
    window.location.hostname === '127.0.0.1' ||
    /^192\.168\.\d+\.\d+$/.test(window.location.hostname) ||
    /^10\.\d+\.\d+\.\d+$/.test(window.location.hostname) ||
    /^172\.(1[6-9]|2\d|3[01])\.\d+\.\d+$/.test(window.location.hostname)
);

const DATA_SOURCE_LABEL = IS_NETLIFY ? 'Firebase Cloud' : 'Local ESP32 + Firebase';
console.log(`Running in ${IS_NETLIFY ? 'NETLIFY (Firebase-only)' : 'LOCAL (ESP32)'} mode`);

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
const firebaseConfig = {
    databaseURL: "https://hydrophonic-bucket-system-default-rtdb.asia-southeast1.firebasedatabase.app/"
};

let db = null;
let firebaseInitialized = false;

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

// ========== Init ==========
document.addEventListener('DOMContentLoaded', () => {
    initializeCharts();
    setupFirebaseListeners();
    setText('csvStatus', DATA_SOURCE_LABEL);

    if (!IS_NETLIFY) {
        // Local mode: also poll ESP32 directly
        fetchLiveData();
        checkStatus();
        fetchGsmSettings();
        liveDataInterval = setInterval(fetchLiveData, 5000);
        setInterval(checkStatus, 15000);
    } else {
        // Netlify mode: Firebase listener is the sole source — load GSM from Firebase
        fetchGsmSettingsFromFirebase();
    }
});

// ========== Firebase Listeners ==========
function setupFirebaseListeners() {
    if (!firebaseInitialized || !db) return;

    // Live sensor data from Firebase (real-time push from ESP32)
    db.ref('sensors/live').on('value', (snapshot) => {
        const data = snapshot.val();
        if (data) {
            setConnection(true);
            updateLiveDisplay(data);
        }
    });

    // Historical logs — load latest 100 on startup, then keep appending live
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
// Write a pump command to Firebase — ESP32 reads and executes it
async function firebaseControl(path, value) {
    if (!firebaseInitialized || !db) {
        throw new Error('Firebase not available');
    }
    return db.ref(path).set(value);
}

// ========== Control Buttons ==========
async function controlPump(action) {
    if (IS_NETLIFY) {
        try {
            await firebaseControl('controls/pump', action);
            showToast(`Air Pump command sent: ${action.toUpperCase()}`);
        } catch (error) {
            alert(`Firebase control error: ${error.message}`);
        }
    } else {
        try {
            const data = await apiPost('/api/pump', action);
            updateControlStates(
                data.pump === 'on',
                data.autoPump === 'enabled',
                data.pumpManualOverride === 'active'
            );
            await fetchLiveData();
        } catch (error) {
            console.error('Pump control error:', error);
            alert(`Failed to control pump: ${error.message}`);
        }
    }
}

async function controlPumpAuto(action) {
    if (IS_NETLIFY) {
        try {
            await firebaseControl('controls/autoPump', action);
            showToast(`Auto Pump ${action === 'enable' ? 'ENABLED' : 'DISABLED'}`);
        } catch (error) {
            alert(`Firebase control error: ${error.message}`);
        }
    } else {
        try {
            const data = await apiPost('/api/pump/auto', action);
            updateControlStates(
                data.pump === 'on',
                data.autoPump === 'enabled',
                data.pumpManualOverride === 'active',
                0, 0,
                data.waterPumpActive === 'on'
            );
            await fetchLiveData();
        } catch (error) {
            console.error('Auto pump control error:', error);
            alert(`Failed to control auto pump: ${error.message}`);
        }
    }
}

async function controlWaterPump(action) {
    if (IS_NETLIFY) {
        try {
            await firebaseControl('controls/waterPump', action);
            showToast(`Water Pump command sent: ${action.toUpperCase()}`);
        } catch (error) {
            alert(`Firebase control error: ${error.message}`);
        }
    } else {
        try {
            const data = await apiPost('/api/waterpump', action);
            const waterPumpOn = data.waterPump === 'on';
            updateControlStates(false, false, false, 0, 0, waterPumpOn);
            await fetchLiveData();
        } catch (error) {
            console.error('Water pump control error:', error);
            alert(`Failed to control water pump: ${error.message}`);
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
            background: rgba(0, 229, 160, 0.15); color: #00e5a0;
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

// ========== GSM Settings via Firebase (Netlify mode) ==========
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

    if (IS_NETLIFY) {
        try {
            await firebaseControl('gsm', { phoneNumber, message, enabled });
            showToast('GSM settings saved to Firebase ✓');
        } catch (error) {
            alert(`Failed to save GSM settings: ${error.message}`);
        }
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
        showToast('Dashboard is live — Firebase updates automatically');
    }
}

async function checkStatus() {
    try {
        const data = await apiGet('/api/status');
        setConnection(true);
        setText('csvStatus', 'Local ESP32');
        updateControlStates(
            data.pump === 'on',
            data.autoPump === 'enabled',
            data.pumpManualOverride === 'active',
            0, 0,
            data.waterPumpActive === 'on'
        );
    } catch (error) {
        console.error('Status check error:', error);
        if (!IS_NETLIFY) setConnection(false);
        setText('csvStatus', IS_NETLIFY ? 'Firebase Cloud' : 'Unavailable');
    }
}

// ========== ESP32 Local API ==========
async function apiGet(url) {
    const response = await fetch(url, { cache: 'no-store' });
    if (!response.ok) throw new Error(`GET ${url} failed: ${response.status}`);
    return response.json();
}

async function apiPost(url, action) {
    const response = await fetch(url, {
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
    const response = await fetch(url, {
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
        setConnection(true);
        updateLiveDisplay(data);
        sensorHistory.push({ timestamp: new Date(), ...data });
        if (sensorHistory.length > 100) sensorHistory.shift();
        updateCharts();
    } catch (error) {
        console.error('Fetch live data error:', error);
        setConnection(false);
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

// ========== Connection State ==========
function setConnection(isConnected) {
    const liveDot = document.getElementById('liveDot');
    const badge   = document.getElementById('esp32Status');

    if (liveDot) {
        if (isConnected) {
            liveDot.classList.add('connected');
            liveDot.textContent = IS_NETLIFY ? 'Firebase Live' : 'ESP32 Connected';
        } else {
            liveDot.classList.remove('connected');
            liveDot.textContent = 'Disconnected';
        }
    }
    if (badge) {
        badge.textContent  = isConnected ? 'Connected' : 'Disconnected';
        badge.style.color  = isConnected ? 'var(--accent)' : '#ff5050';
    }
}

// ========== Display Update ==========
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

    const now = new Date();
    const formattedDate = `${String(now.getDate()).padStart(2,'0')}/${String(now.getMonth()+1).padStart(2,'0')}/${now.getFullYear()}, ${now.toLocaleTimeString()}`;
    setText('lastUpdate', formattedDate);
    setText('debugCSVTime', now.toLocaleTimeString());

    const isAirPumpOn   = !!data.pumpActive;
    const isWaterPumpOn = !!data.waterPumpActive;
    setText('airPumpStatus',   isAirPumpOn   ? 'Active' : 'Idle');
    setText('waterPumpStatus', isWaterPumpOn ? 'Active' : 'Idle');
    updateBadge('airPumpBadge',   isAirPumpOn);
    updateBadge('waterPumpBadge', isWaterPumpOn);

    updateControlStates(
        data.pumpActive,
        data.autoPumpEnabled,
        data.pumpManualOverride,
        data.ecLower,
        data.ecUpper,
        data.waterPumpActive
    );
}

function updateBadge(badgeId, isOn) {
    const el = document.getElementById(badgeId);
    if (!el) return;
    el.textContent = isOn ? 'ON' : 'OFF';
    if (isOn) el.classList.add('on');
    else el.classList.remove('on');
}

function updateControlStates(pumpActive, autoPumpEnabled, pumpManualOverride, ecLower, ecUpper, waterPumpActive = false) {
    setText('pumpState',          pumpActive      ? 'ON ✓' : 'OFF ✗');
    setText('autoPumpState',      autoPumpEnabled  ? 'ENABLED'   : 'DISABLED');
    setText('autoPumpStateBadge', autoPumpEnabled  ? 'ENABLED ✓' : 'DISABLED ✗');
    setText('manualOverrideStatus', pumpManualOverride ? 'ACTIVE ⚠️' : 'Inactive');
    setText('waterPumpState',     waterPumpActive  ? 'ON ✓' : 'OFF ✗');

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
function downloadCSV() {
    if (!sensorHistory.length) {
        if (IS_NETLIFY) {
            // Try to fetch full history directly from Firebase for CSV export
            if (firebaseInitialized && db) {
                showToast('Fetching full history from Firebase...');
                db.ref('sensors/history').once('value', (snapshot) => {
                    const historyObj = snapshot.val();
                    if (!historyObj) { alert('No history data in Firebase yet.'); return; }
                    const allHistory = Object.values(historyObj).sort((a, b) => (a.timestamp || 0) - (b.timestamp || 0));
                    exportCSV(allHistory);
                });
            }
        } else {
            alert('No historical sensor data available to export.');
        }
        return;
    }
    exportCSV(sensorHistory);
}

function exportCSV(rows) {
    const headers = ['Timestamp', 'Air Temp (C)', 'Humidity (%)', 'Water Temp (C)', 'Water Level', 'EC (mS/cm)', 'Flow Rate (L/min)'];
    const csvRows = rows.map(row => [
        row.timestamp ? new Date(row.timestamp).toISOString() : '',
        row.dhtTemperature  ?? '',
        row.dhtHumidity     ?? '',
        row.waterTemperature ?? '',
        row.waterLevel       ?? '',
        row.tdsMScm          ?? '',
        row.flowRate         ?? ''
    ]);

    const csvContent = [headers.join(','), ...csvRows.map(e => e.join(','))].join('\n');
    const blob = new Blob([csvContent], { type: 'text/csv;charset=utf-8;' });
    const url  = URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.setAttribute('href', url);
    link.setAttribute('download', `greenhouse_report_${new Date().toISOString().slice(0,10)}.csv`);
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
}

window.addEventListener('beforeunload', () => {
    clearInterval(liveDataInterval);
});