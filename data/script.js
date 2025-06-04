// 全局变量
let websocket;
let currentLang = localStorage.getItem('preferredLang') || 'zh'; // 默认中文
let translations = {};
let sensorChart;
const MAX_CHART_DATA_POINTS = 180; // 3分钟数据，每秒一个点 (3 * 60 = 180)
let chartLabels = [];
let tempData = [];
let humData = [];
let gasData = [];

// DOMContentLoaded 事件监听器，确保DOM加载完毕后执行脚本
document.addEventListener('DOMContentLoaded', () => {
    loadTranslations().then(() => {
        applyTranslations(currentLang);
        initPage();
    });

    // 语言切换按钮事件
    document.getElementById('lang-zh')?.addEventListener('click', () => setLanguage('zh'));
    document.getElementById('lang-fr')?.addEventListener('click', () => setLanguage('fr'));
});

/**
 * 加载语言文件 lang.json
 */
async function loadTranslations() {
    try {
        const response = await fetch('lang.json');
        if (!response.ok) {
            console.error('Failed to load lang.json:', response.statusText);
            return;
        }
        translations = await response.json();
    } catch (error) {
        console.error('Error fetching lang.json:', error);
    }
}

/**
 * 应用翻译到页面元素
 * @param {string} lang - 目标语言 ('zh' 或 'fr')
 */
function applyTranslations(lang) {
    document.documentElement.lang = lang;
    document.querySelectorAll('[data-translate]').forEach(el => {
        const key = el.getAttribute('data-translate');
        if (translations[lang] && translations[lang][key]) {
            if (el.tagName === 'INPUT' && (el.type === 'submit' || el.type === 'button')) {
                el.value = translations[lang][key];
            } else if (el.hasAttribute('placeholder') && translations[lang][key + '_placeholder']) {
                el.placeholder = translations[lang][key + '_placeholder'];
            }
            else {
                el.textContent = translations[lang][key];
            }
        }
    });
    const pageTitleKey = document.querySelector('head > title[data-translate]')?.getAttribute('data-translate');
    if (pageTitleKey && translations[lang] && translations[lang][pageTitleKey]) {
        document.title = translations[lang][pageTitleKey];
    }
    document.querySelectorAll('[data-translate-title]').forEach(el => {
        const key = el.getAttribute('data-translate-title');
        if (translations[lang] && translations[lang][key]) {
            el.title = translations[lang][key];
        }
    });
    if (sensorChart && translations[lang]) {
        sensorChart.data.datasets[0].label = translations[lang].chart_temperature_label || 'Temperature (°C)';
        sensorChart.data.datasets[1].label = translations[lang].chart_humidity_label || 'Humidity (%)';
        sensorChart.data.datasets[2].label = translations[lang].chart_gas_label || 'Gas (PPM)';
        // 更新Y轴标题文本
        if (sensorChart.options.scales.yTemp && sensorChart.options.scales.yTemp.title) {
            sensorChart.options.scales.yTemp.title.text = translations[lang].unit_celsius || '°C';
        }
        if (sensorChart.options.scales.yHumGas && sensorChart.options.scales.yHumGas.title) {
            sensorChart.options.scales.yHumGas.title.text = (translations[lang].unit_percent || '%') + ' / ' + (translations[lang].unit_ppm || 'PPM');
        }
        sensorChart.update('none'); // 'none' to prevent animation
    }
}

/**
 * 设置当前语言并保存偏好
 */
function setLanguage(lang) {
    currentLang = lang;
    localStorage.setItem('preferredLang', lang);
    document.querySelectorAll('.lang-button').forEach(btn => btn.classList.remove('active'));
    document.getElementById(`lang-${lang}`)?.classList.add('active');
    applyTranslations(lang);
}

/**
 * 初始化页面特定的功能
 */
function initPage() {
    connectWebSocket();
    if (document.getElementById('sensorDataChart')) {
        initChart();
    } else if (document.getElementById('wifiConfigForm')) {
        setupSettingsPageListeners();
    }
}

/**
 * 连接 WebSocket 服务器
 */
function connectWebSocket() {
    const gateway = `ws://${window.location.hostname}:81/`;
    console.log(`Attempting to connect to WebSocket at ${gateway}`);
    websocket = new WebSocket(gateway);

    websocket.onopen = (event) => {
        console.log('WebSocket connection opened successfully.');
        if (document.getElementById('wifiConfigForm')) {
            websocket.send(JSON.stringify({ action: "getCurrentSettings" }));
        } else if (document.getElementById('sensorDataChart')) {
            websocket.send(JSON.stringify({ action: "getHistoricalData" }));
        }
    };

    websocket.onmessage = (event) => {
        // console.log('WebSocket message received raw:', event.data);
        let data;
        try {
            data = JSON.parse(event.data);
            // console.log('Parsed WebSocket data:', data);
        } catch (e) {
            console.error('Error parsing WebSocket message JSON:', e, "Raw data:", event.data);
            return;
        }

        switch (data.type) {
            case 'sensorData':
                updateSensorReadings(data);
                break;
            case 'wifiStatus':
                updateWifiStatus(data);
                break;
            case 'historicalData':
                populateChartWithHistoricalData(data.history);
                break;
            case 'settingsData':
                populateSettingsForm(data.settings);
                break;
            case 'wifiScanResults':
                displayWifiScanResults(data.networks);
                break;
            case 'connectWifiStatus':
                updateStatusMessage('connect-wifi-status', data.message, !data.success);
                break;
            case 'saveSettingsStatus':
                updateStatusMessage('save-settings-status', data.message, !data.success);
                break;
            case 'resetStatus':
                updateStatusMessage('reset-status', data.message, !data.success);
                if(data.success) {
                    alert(translations[currentLang]?.settings_saved || "Settings reset successfully. Device will restart.");
                }
                break;
            case 'error':
                console.error('Error message from server:', data.message);
                updateStatusMessage('general-status', data.message, true);
                break;
            default:
                // console.log('Unknown WebSocket message type received:', data.type, data);
                break;
        }
    };

    websocket.onclose = (event) => {
        console.log('WebSocket connection closed. Event:', event);
        updateElementText('wifiStatusText', translations[currentLang]?.wifi_disconnected || 'Disconnected', 'wifi_disconnected');
        setTimeout(connectWebSocket, 2000);
    };

    websocket.onerror = (error) => {
        console.error('WebSocket error:', error);
    };
}

/**
 * 更新主页上的传感器读数和状态指示灯
 */
function updateSensorReadings(data) {
    const tempValEl = document.getElementById('tempVal');
    const humValEl = document.getElementById('humVal');
    const gasValEl = document.getElementById('gasVal');

    if (tempValEl) tempValEl.textContent = data.temperature !== null ? data.temperature.toFixed(1) : '--';
    if (humValEl) humValEl.textContent = data.humidity !== null ? data.humidity.toFixed(1) : '--';
    if (gasValEl) gasValEl.textContent = data.gas !== null ? data.gas.toFixed(0) : '--';

    updateStatusIndicator('temp-status-indicator', data.tempStatus);
    updateStatusIndicator('hum-status-indicator', data.humStatus);
    updateStatusIndicator('gas-status-indicator', data.gasStatus);

    if (sensorChart && data.temperature !== null && data.humidity !== null && data.gas !== null) {
        const now = new Date();
        const timeString = now.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
        addDataToChart(timeString, data.temperature, data.humidity, data.gas);
    }
}

/**
 * 更新状态指示灯的类
 */
function updateStatusIndicator(elementId, status) {
    const indicator = document.getElementById(elementId);
    if (indicator) {
        indicator.className = 'status-indicator';
        if (status === 'normal') indicator.classList.add('normal');
        else if (status === 'warning') indicator.classList.add('warning');
        else if (status === 'disconnected') indicator.classList.add('disconnected');
    }
}

/**
 * 更新主页上的WiFi状态信息
 */
function updateWifiStatus(data) {
    const statusTextEl = document.getElementById('wifiStatusText');
    if (statusTextEl) {
        if (data.connected) {
            statusTextEl.innerHTML = `${translations[currentLang]?.wifi_connected_to || 'Connected to'} <span style="font-weight:bold;">${data.ssid}</span>`;
        } else {
            statusTextEl.textContent = translations[currentLang]?.wifi_disconnected || 'Disconnected';
            if (data.ap_mode) {
                statusTextEl.textContent += ` (AP: ${data.ap_ssid || 'ESP32_Sensor_Hub'})`;
            }
        }
    }
    updateElementText('wifiSSIDText', data.ssid || 'N/A');
    updateElementText('wifiIPText', data.ip || 'N/A');
}


/**
 * 初始化 Chart.js 图表
 */
function initChart() {
    const ctx = document.getElementById('sensorDataChart')?.getContext('2d');
    if (!ctx) return;

    sensorChart = new Chart(ctx, {
        type: 'line',
        data: {
            labels: chartLabels,
            datasets: [
                {
                    label: translations[currentLang]?.chart_temperature_label || 'Temperature (°C)',
                    data: tempData,
                    borderColor: 'rgba(255, 99, 132, 1)',
                    backgroundColor: 'rgba(255, 99, 132, 0.2)',
                    borderWidth: 2, tension: 0.3, yAxisID: 'yTemp', fill: false,
                    pointRadius: 0, // 去掉数据点
                    pointHitRadius: 10 // 悬停时点的响应范围
                },
                {
                    label: translations[currentLang]?.chart_humidity_label || 'Humidity (%)',
                    data: humData,
                    borderColor: 'rgba(54, 162, 235, 1)',
                    backgroundColor: 'rgba(54, 162, 235, 0.2)',
                    borderWidth: 2, tension: 0.3, yAxisID: 'yHumGas', fill: false,
                    pointRadius: 0, // 去掉数据点
                    pointHitRadius: 10
                },
                {
                    label: translations[currentLang]?.chart_gas_label || 'Gas (PPM)',
                    data: gasData,
                    borderColor: 'rgba(75, 192, 192, 1)',
                    backgroundColor: 'rgba(75, 192, 192, 0.2)',
                    borderWidth: 2, tension: 0.3, yAxisID: 'yHumGas', fill: false,
                    pointRadius: 0, // 去掉数据点
                    pointHitRadius: 10
                }
            ]
        },
        options: {
            responsive: true, maintainAspectRatio: false, animation: { duration: 200 },
            scales: {
                x: {
                    type: 'category',
                    ticks: { autoSkip: true, maxTicksLimit: 10, maxRotation: 60, minRotation: 30, font: { size: 10 }},
                    grid: {
                        display: true, // 显示X轴的竖线 (背景竖线)
                        color: 'rgba(200, 200, 200, 0.1)', // 设置竖线颜色
                        drawBorder: false
                    }
                },
                yTemp: {
                    type: 'linear', position: 'left', min: -10, max: 50, ticks: { stepSize: 10, font: { size: 10 }},
                    title: {
                        display: true,
                        text: translations[currentLang]?.unit_celsius || '°C',
                        font: { size: 12, weight: 'bold' }, // 调整字体大小和粗细
                        align: 'end', // 'start', 'center', 'end' - 尝试 'end' 使其靠近轴的顶端
                        padding: { top: 0, bottom: 5 } // 调整内边距
                    },
                    grid: { drawBorder: false, color: 'rgba(200, 200, 200, 0.2)'}
                },
                yHumGas: {
                    type: 'linear', position: 'right', min: 0, max: 100, // PPM可能需要不同轴或动态调整
                    ticks: { stepSize: 10, font: { size: 10 }},
                    title: {
                        display: true,
                        text: (translations[currentLang]?.unit_percent || '%') + ' / ' + (translations[currentLang]?.unit_ppm || 'PPM'),
                        font: { size: 12, weight: 'bold' },
                        align: 'end',
                        padding: { top: 0, bottom: 5 }
                    },
                    grid: { drawBorder: false, color: 'rgba(200, 200, 200, 0.1)'}
                }
            },
            plugins: {
                tooltip: { mode: 'index', intersect: false, callbacks: { title: (tooltipItems) => tooltipItems[0].label }},
                legend: { position: 'top', labels: { font: { size: 11 }, boxWidth: 20, padding: 15 }},
            },
            interaction: { mode: 'index', intersect: false, },
        },
        plugins: [{
            id: 'verticalLinePlugin',
            afterDraw: (chart) => {
                if (chart.tooltip && chart.tooltip.getActiveElements && chart.tooltip.getActiveElements().length) {
                    const activeElement = chart.tooltip.getActiveElements()[0];
                    if (activeElement && activeElement.element) {
                        const x = activeElement.element.x;
                        const yAxis = chart.scales.yTemp;
                        const ctx = chart.ctx;
                        ctx.save(); ctx.beginPath(); ctx.moveTo(x, yAxis.top); ctx.lineTo(x, yAxis.bottom);
                        ctx.lineWidth = 1; ctx.strokeStyle = 'rgba(100, 100, 100, 0.3)'; ctx.stroke(); ctx.restore();
                    }
                }
            }
        }]
    });
}

/**
 * 向图表添加新数据
 */
function addDataToChart(label, temp, hum, gas) {
    if (!sensorChart) return;
    sensorChart.data.labels.push(label);
    sensorChart.data.datasets[0].data.push(temp);
    sensorChart.data.datasets[1].data.push(hum);
    sensorChart.data.datasets[2].data.push(gas);
    if (sensorChart.data.labels.length > MAX_CHART_DATA_POINTS) {
        sensorChart.data.labels.shift();
        sensorChart.data.datasets.forEach(dataset => dataset.data.shift());
    }
    sensorChart.update('none');
}

/**
 * 使用历史数据填充图表
 */
function populateChartWithHistoricalData(history) {
    if (!sensorChart || !history || !Array.isArray(history)) {
        console.log("populateChartWithHistoricalData: Chart or history not ready.");
        return;
    }
    chartLabels.length = 0; tempData.length = 0; humData.length = 0; gasData.length = 0;
    history.forEach(record => {
        chartLabels.push(record.time || new Date(record.timestamp * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' }));
        tempData.push(record.temp); humData.push(record.hum); gasData.push(record.gas);
    });
    sensorChart.data.labels = chartLabels;
    sensorChart.data.datasets[0].data = tempData;
    sensorChart.data.datasets[1].data = humData;
    sensorChart.data.datasets[2].data = gasData;
    while (sensorChart.data.labels.length > MAX_CHART_DATA_POINTS) {
        sensorChart.data.labels.shift();
        sensorChart.data.datasets.forEach(dataset => dataset.data.shift());
    }
    sensorChart.update('none');
    console.log("Chart populated with historical data. Points:", chartLabels.length);
}

/**
 * 为设置页面的表单和按钮添加事件监听器
 */
function setupSettingsPageListeners() {
    document.getElementById('scanWifiButton')?.addEventListener('click', handleScanWifi);
    document.getElementById('connectWifiButton')?.addEventListener('click', handleConnectWifi);
    document.getElementById('saveThresholdsButton')?.addEventListener('click', handleSaveThresholds);
    document.getElementById('resetSettingsButton')?.addEventListener('click', handleResetSettings);
}

/**
 * 处理扫描WiFi网络的请求
 */
function handleScanWifi() {
    if (websocket && websocket.readyState === WebSocket.OPEN) {
        websocket.send(JSON.stringify({ action: 'scanWifi' }));
        updateStatusMessage('scan-status', translations[currentLang]?.connecting_wifi || 'Scanning...', false);
    } else {
        updateStatusMessage('scan-status', 'WebSocket not connected.', true);
    }
}

/**
 * 显示WiFi扫描结果
 */
function displayWifiScanResults(networks) {
    const container = document.getElementById('ssid-list-container');
    const scanStatusEl = document.getElementById('scan-status');
    if (!container || !scanStatusEl) {
        console.error("SSID list container or scan status element not found.");
        return;
    }
    container.innerHTML = '';
    if (networks && networks.length > 0) {
        networks.forEach(net => {
            const button = document.createElement('button');
            button.type = 'button';
            button.textContent = `${net.ssid} (${net.rssi} dBm, ${net.encryption})`;
            button.onclick = () => {
                document.getElementById('wifiSSID').value = net.ssid;
                container.style.display = 'none';
            };
            container.appendChild(button);
        });
        scanStatusEl.textContent = `${networks.length} ${translations[currentLang]?.networks_found_status || 'network(s) found.'}`;
        scanStatusEl.style.color = 'var(--text-light-color)';
        container.style.display = 'block';
    } else {
        scanStatusEl.textContent = translations[currentLang]?.no_networks_found_status || 'No networks found or scan failed.';
        scanStatusEl.style.color = 'var(--text-light-color)';
        container.style.display = 'none';
    }
}

/**
 * 处理连接WiFi的请求
 */
function handleConnectWifi() {
    const ssid = document.getElementById('wifiSSID')?.value.trim();
    const password = document.getElementById('wifiPassword')?.value;
    if (!ssid) {
        updateStatusMessage('connect-wifi-status', 'SSID cannot be empty.', true);
        return;
    }
    if (websocket && websocket.readyState === WebSocket.OPEN) {
        websocket.send(JSON.stringify({ action: 'connectWifi', ssid: ssid, password: password }));
        updateStatusMessage('connect-wifi-status', translations[currentLang]?.connecting_wifi || 'Connecting...', false);
    } else {
        updateStatusMessage('connect-wifi-status', 'WebSocket not connected.', true);
    }
}

/**
 * 处理保存报警阈值的请求
 */
function handleSaveThresholds() {
    const thresholds = {
        action: 'saveThresholds',
        tempMin: parseFloat(document.getElementById('tempMin')?.value),
        tempMax: parseFloat(document.getElementById('tempMax')?.value),
        humMin: parseFloat(document.getElementById('humMin')?.value),
        humMax: parseFloat(document.getElementById('humMax')?.value),
        gasMin: parseFloat(document.getElementById('gasMin')?.value),
        gasMax: parseFloat(document.getElementById('gasMax')?.value),
    };
    if (isNaN(thresholds.tempMin) || isNaN(thresholds.tempMax) || isNaN(thresholds.humMin) || isNaN(thresholds.humMax) || isNaN(thresholds.gasMin) || isNaN(thresholds.gasMax)) {
        updateStatusMessage('save-settings-status', 'All threshold values must be numbers.', true); return;
    }
    if (thresholds.tempMin >= thresholds.tempMax || thresholds.humMin >= thresholds.humMax || thresholds.gasMin >= thresholds.gasMax) {
         updateStatusMessage('save-settings-status', 'Min value must be less than Max value for each threshold.', true); return;
    }
    if (websocket && websocket.readyState === WebSocket.OPEN) {
        websocket.send(JSON.stringify(thresholds));
        updateStatusMessage('save-settings-status', 'Saving settings...', false);
    } else {
        updateStatusMessage('save-settings-status', 'WebSocket not connected.', true);
    }
}

/**
 * 处理恢复出厂设置的请求
 */
function handleResetSettings() {
    const confirmationText = translations[currentLang]?.reset_settings_prompt || "Are you sure you want to reset all settings? This cannot be undone.";
    if (confirm(confirmationText)) {
        if (websocket && websocket.readyState === WebSocket.OPEN) {
            websocket.send(JSON.stringify({ action: 'resetSettings' }));
            updateStatusMessage('reset-status', 'Resetting settings...', false);
        } else {
            updateStatusMessage('reset-status', 'WebSocket not connected.', true);
        }
    }
}

/**
 * 填充设置页面的表单字段
 */
function populateSettingsForm(settings) {
    if (!settings) { console.log("populateSettingsForm: No settings data received."); return; }
    if (settings.thresholds) {
        const t = settings.thresholds;
        document.getElementById('tempMin').value = t.tempMin !== undefined ? t.tempMin : '';
        document.getElementById('tempMax').value = t.tempMax !== undefined ? t.tempMax : '';
        document.getElementById('humMin').value = t.humMin !== undefined ? t.humMin : '';
        document.getElementById('humMax').value = t.humMax !== undefined ? t.humMax : '';
        document.getElementById('gasMin').value = t.gasMin !== undefined ? t.gasMin : '';
        document.getElementById('gasMax').value = t.gasMax !== undefined ? t.gasMax : '';
    }
    console.log("Settings form populated.");
}

/**
 * 更新页面上的状态消息
 */
function updateStatusMessage(elementId, message, isError) {
    const el = document.getElementById(elementId);
    if (el) {
        el.textContent = message;
        el.style.color = isError ? 'var(--danger-color)' : 'var(--success-color)';
        setTimeout(() => { if (el.textContent === message) el.textContent = ''; }, 3000);
    }
}

/**
 * 辅助函数：更新元素的文本内容
 */
function updateElementText(id, text, translateKey = null) {
    const element = document.getElementById(id);
    if (element) {
        if (translateKey && translations[currentLang] && translations[currentLang][translateKey]) {
            element.textContent = translations[currentLang][translateKey];
        } else {
            element.textContent = text;
        }
    }
}
