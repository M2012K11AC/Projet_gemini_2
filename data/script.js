// 全局变量
let websocket;
let currentLang = localStorage.getItem('preferredLang') || 'zh'; // 默认中文
let translations = {};
let sensorChart;
const MAX_CHART_DATA_POINTS = 90; // 与后端 HISTORICAL_DATA_POINTS 保持一致
let chartLabels = [];
let tempData = [];
let humData = [];
let coData = [];
let no2Data = [];
let c2h5ohData = [];
let vocData = [];

let wsReconnectInterval = 2000; // WebSocket重连间隔 (ms)
let wsMaxReconnectAttempts = 5;
let wsReconnectAttempts = 0;


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

    // 更新图表标签翻译
    if (sensorChart && translations[lang]) {
        sensorChart.data.datasets[0].label = translations[lang].chart_temperature_label || 'Temperature (°C)';
        sensorChart.data.datasets[1].label = translations[lang].chart_humidity_label || 'Humidity (%)';
        sensorChart.data.datasets[2].label = translations[lang].chart_co_label || 'CO (PPM)';
        sensorChart.data.datasets[3].label = translations[lang].chart_no2_label || 'NO2 (PPM)';
        sensorChart.data.datasets[4].label = translations[lang].chart_c2h5oh_label || 'C2H5OH (PPM)';
        sensorChart.data.datasets[5].label = translations[lang].chart_voc_label || 'VOC (PPM)';
        
        if (sensorChart.options.scales.yTemp && sensorChart.options.scales.yTemp.title) {
            sensorChart.options.scales.yTemp.title.text = translations[lang].unit_celsius || '°C';
        }
        if (sensorChart.options.scales.yHum && sensorChart.options.scales.yHum.title) {
            sensorChart.options.scales.yHum.title.text = translations[lang].unit_percent || '%';
        }
        if (sensorChart.options.scales.yGas && sensorChart.options.scales.yGas.title) {
             sensorChart.options.scales.yGas.title.text = translations[lang].unit_ppm || 'PPM';
        }
        sensorChart.update('none'); 
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
    const statusBanner = document.getElementById('connection-status-banner');

    if (statusBanner) {
        statusBanner.textContent = translations[currentLang]?.ws_connecting || '正在连接 WebSocket...';
        statusBanner.className = 'status-banner connecting';
        statusBanner.style.display = 'block';
    }
    console.log(`Attempting to connect to WebSocket at ${gateway}`);
    websocket = new WebSocket(gateway);

    websocket.onopen = (event) => {
        console.log('WebSocket connection opened successfully.');
        wsReconnectAttempts = 0; // 重置重连尝试次数
        if (statusBanner) {
            statusBanner.textContent = translations[currentLang]?.ws_connected || 'WebSocket 已连接';
            statusBanner.className = 'status-banner connected';
            setTimeout(() => { statusBanner.style.display = 'none'; }, 2000);
        }
        // 连接成功后请求初始数据
        if (document.getElementById('wifiConfigForm')) {
            websocket.send(JSON.stringify({ action: "getCurrentSettings" }));
        } else if (document.getElementById('sensorDataChart')) {
            // 延迟请求历史数据，给传感器数据先到达的机会
            setTimeout(() => websocket.send(JSON.stringify({ action: "getHistoricalData" })), 500);
        }
    };

    websocket.onmessage = (event) => {
        let data;
        try {
            data = JSON.parse(event.data);
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
                displayWifiScanResults(data); // 传递整个data对象以处理错误
                break;
            case 'connectWifiStatus':
                updateStatusMessage('connect-wifi-status', data.message, !data.success);
                break;
            case 'saveSettingsStatus':
                updateStatusMessage('save-settings-status', data.message, !data.success);
                break;
            case 'saveBrightnessStatus':
                updateStatusMessage('save-led-status', data.message, !data.success);
                break;
            case 'resetStatus':
                updateStatusMessage('reset-status', data.message, !data.success);
                if(data.success) {
                    alert(translations[currentLang]?.settings_reset_success || "设置已重置。设备将重启。");
                }
                break;
            case 'error':
                console.error('Error message from server:', data.message);
                updateStatusMessage('general-status', data.message, true); // 假设有general-status元素
                break;
            case 'scanStatus': // 扫描开始的状态
                updateStatusMessage('scan-status', data.message, false);
                break;
            default:
                // console.log('Unknown WebSocket message type received:', data.type, data);
                break;
        }
    };

    websocket.onclose = (event) => {
        console.log('WebSocket connection closed. Event:', event);
        if (statusBanner) {
            statusBanner.className = 'status-banner disconnected';
            statusBanner.style.display = 'block';
        }
        updateElementText('wifiStatusText', translations[currentLang]?.ws_disconnected_retry || 'WebSocket 已断开。正在尝试重连...', 'ws_disconnected_retry');
        
        if (wsReconnectAttempts < wsMaxReconnectAttempts) {
            wsReconnectAttempts++;
            if (statusBanner) {
                 statusBanner.textContent = `${translations[currentLang]?.ws_disconnected_retry || 'WebSocket 已断开。正在尝试重连...'} (${wsReconnectAttempts}/${wsMaxReconnectAttempts})`;
            }
            console.log(`WebSocket closed. Reconnecting in ${wsReconnectInterval / 1000}s (attempt ${wsReconnectAttempts}/${wsMaxReconnectAttempts})`);
            setTimeout(connectWebSocket, wsReconnectInterval);
        } else {
            if (statusBanner) {
                statusBanner.textContent = translations[currentLang]?.ws_reconnect_failed || 'WebSocket 重连失败。请检查设备连接或刷新页面。';
            }
            console.error('WebSocket reconnection failed after multiple attempts.');
        }
    };

    websocket.onerror = (error) => {
        console.error('WebSocket error:', error);
        if (statusBanner) {
            statusBanner.textContent = translations[currentLang]?.ws_error || 'WebSocket 连接错误。';
            statusBanner.className = 'status-banner error';
            statusBanner.style.display = 'block';
        }
        // onerror 之后通常会触发 onclose，所以重连逻辑主要在 onclose 中处理
    };
}

/**
 * 更新主页上的传感器读数和状态指示灯
 */
function updateSensorReadings(data) {
    const sensorElements = {
        tempVal: 'temperature', humVal: 'humidity',
        coVal: 'co', no2Val: 'no2', c2h5ohVal: 'c2h5oh', vocVal: 'voc'
    };
    const sensorStatusIndicators = {
        'temp-status-indicator': 'tempStatus', 'hum-status-indicator': 'humStatus',
        'gasCo-status-indicator': 'gasCoStatus', 'gasNo2-status-indicator': 'gasNo2Status',
        'gasC2h5oh-status-indicator': 'gasC2h5ohStatus', 'gasVoc-status-indicator': 'gasVocStatus'
    };

    for (const [elId, dataKey] of Object.entries(sensorElements)) {
        const el = document.getElementById(elId);
        if (el) {
            const value = data[dataKey];
            if (value !== null && value !== undefined && !isNaN(value)) {
                el.textContent = (dataKey === 'no2' || dataKey === 'voc') ? value.toFixed(2) : value.toFixed(1);
            } else {
                el.textContent = '--';
            }
        }
    }

    for (const [elId, statusKey] of Object.entries(sensorStatusIndicators)) {
        updateStatusIndicator(elId, data[statusKey]);
    }
    
    if (sensorChart && data.temperature !== null && data.humidity !== null && data.co !== null /*检查一个气体值*/) {
        const now = new Date(); // 使用客户端时间作为图表标签，如果后端提供了精确时间戳更好
        const timeString = data.timeStr || now.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
        addDataToChart(timeString, data.temperature, data.humidity, data.co, data.no2, data.c2h5oh, data.voc);
    }
}

/**
 * 更新状态指示灯的类
 */
function updateStatusIndicator(elementId, status) {
    const indicator = document.getElementById(elementId);
    if (indicator) {
        indicator.className = 'status-indicator'; // Reset classes
        if (status === 'normal') indicator.classList.add('normal');
        else if (status === 'warning') indicator.classList.add('warning');
        else if (status === 'disconnected') indicator.classList.add('disconnected');
        else if (status === 'initializing') indicator.classList.add('initializing'); // 新增预热状态
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
        } else if (data.connecting_attempt_ssid) {
            statusTextEl.textContent = `${translations[currentLang]?.wifi_connecting_to || 'Connecting to'} ${data.connecting_attempt_ssid}...`;
        } else if (data.connection_failed) {
            statusTextEl.textContent = translations[currentLang]?.wifi_connection_failed || 'WiFi connection failed.';
        }
         else {
            statusTextEl.textContent = translations[currentLang]?.wifi_disconnected || 'Disconnected';
            if (data.ap_mode) {
                statusTextEl.textContent += ` (AP: ${data.ap_ssid || 'ESP32_Sensor_Hub_V2'})`;
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
                    data: tempData, borderColor: 'rgba(255, 99, 132, 1)', backgroundColor: 'rgba(255, 99, 132, 0.2)',
                    borderWidth: 1.5, tension: 0.3, yAxisID: 'yTemp', fill: false, pointRadius: 0, pointHitRadius: 10
                },
                {
                    label: translations[currentLang]?.chart_humidity_label || 'Humidity (%)',
                    data: humData, borderColor: 'rgba(54, 162, 235, 1)', backgroundColor: 'rgba(54, 162, 235, 0.2)',
                    borderWidth: 1.5, tension: 0.3, yAxisID: 'yHum', fill: false, pointRadius: 0, pointHitRadius: 10
                },
                {
                    label: translations[currentLang]?.chart_co_label || 'CO (PPM)',
                    data: coData, borderColor: 'rgba(255, 159, 64, 1)', backgroundColor: 'rgba(255, 159, 64, 0.2)',
                    borderWidth: 1.5, tension: 0.3, yAxisID: 'yGas', fill: false, pointRadius: 0, pointHitRadius: 10
                },
                {
                    label: translations[currentLang]?.chart_no2_label || 'NO2 (PPM)',
                    data: no2Data, borderColor: 'rgba(153, 102, 255, 1)', backgroundColor: 'rgba(153, 102, 255, 0.2)',
                    borderWidth: 1.5, tension: 0.3, yAxisID: 'yGas', fill: false, pointRadius: 0, pointHitRadius: 10
                },
                {
                    label: translations[currentLang]?.chart_c2h5oh_label || 'C2H5OH (PPM)',
                    data: c2h5ohData, borderColor: 'rgba(75, 192, 192, 1)', backgroundColor: 'rgba(75, 192, 192, 0.2)',
                    borderWidth: 1.5, tension: 0.3, yAxisID: 'yGas', fill: false, pointRadius: 0, pointHitRadius: 10
                },
                {
                    label: translations[currentLang]?.chart_voc_label || 'VOC (PPM)',
                    data: vocData, borderColor: 'rgba(255, 205, 86, 1)', backgroundColor: 'rgba(255, 205, 86, 0.2)',
                    borderWidth: 1.5, tension: 0.3, yAxisID: 'yGas', fill: false, pointRadius: 0, pointHitRadius: 10
                }
            ]
        },
        options: {
            responsive: true, maintainAspectRatio: false, animation: { duration: 200 },
            scales: {
                x: {
                    type: 'category',
                    ticks: { autoSkip: true, maxTicksLimit: 10, maxRotation: 45, minRotation: 20, font: { size: 10 }},
                    grid: { display: true, color: 'rgba(200, 200, 200, 0.1)', drawBorder: false }
                },
                yTemp: {
                    type: 'linear', position: 'left', min: 0, max: 50, // 动态调整或根据实际情况设定
                    title: { display: true, text: translations[currentLang]?.unit_celsius || '°C', font: { size: 11, weight: 'bold' }, padding: { top: 0, bottom: 5 } },
                    grid: { drawBorder: false, color: 'rgba(255, 99, 132, 0.15)'}, ticks: { font: { size: 10 }, color: 'rgba(255, 99, 132, 0.8)' }
                },
                yHum: {
                    type: 'linear', position: 'right', min: 0, max: 100,
                    title: { display: true, text: translations[currentLang]?.unit_percent || '%', font: { size: 11, weight: 'bold' }, padding: { top: 0, bottom: 5 } },
                    grid: { drawOnChartArea: false }, ticks: { font: { size: 10 }, color: 'rgba(54, 162, 235, 0.8)' }
                },
                yGas: {
                    type: 'linear', position: 'right', min: 0, // max 可以根据数据动态调整或预设一个较大值
                    // suggestedMax: 100, // 初始建议最大值
                    title: { display: true, text: translations[currentLang]?.unit_ppm || 'PPM', font: { size: 11, weight: 'bold' }, padding: { top: 0, bottom: 5 } },
                    grid: { drawOnChartArea: false }, ticks: { font: { size: 10 }, color: 'rgba(75, 192, 192, 0.8)' }
                }
            },
            plugins: {
                tooltip: { mode: 'index', intersect: false, callbacks: { title: (tooltipItems) => tooltipItems[0].label }},
                legend: { position: 'top', labels: { font: { size: 11 }, boxWidth: 15, padding: 10 }},
            },
            interaction: { mode: 'index', intersect: false, },
        }
    });
}

/**
 * 向图表添加新数据
 */
function addDataToChart(label, temp, hum, co, no2, c2h5oh, voc) {
    if (!sensorChart) return;
    sensorChart.data.labels.push(label);
    sensorChart.data.datasets[0].data.push(temp);
    sensorChart.data.datasets[1].data.push(hum);
    sensorChart.data.datasets[2].data.push(co);
    sensorChart.data.datasets[3].data.push(no2);
    sensorChart.data.datasets[4].data.push(c2h5oh);
    sensorChart.data.datasets[5].data.push(voc);

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
    // 清空现有数据
    chartLabels.length = 0; tempData.length = 0; humData.length = 0;
    coData.length = 0; no2Data.length = 0; c2h5ohData.length = 0; vocData.length = 0;

    history.forEach(record => {
        chartLabels.push(record.time || new Date(record.timestamp * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' }));
        tempData.push(record.temp); 
        humData.push(record.hum);
        coData.push(record.co);
        no2Data.push(record.no2);
        c2h5ohData.push(record.c2h5oh);
        vocData.push(record.voc);
    });

    sensorChart.data.labels = chartLabels;
    sensorChart.data.datasets[0].data = tempData;
    sensorChart.data.datasets[1].data = humData;
    sensorChart.data.datasets[2].data = coData;
    sensorChart.data.datasets[3].data = no2Data;
    sensorChart.data.datasets[4].data = c2h5ohData;
    sensorChart.data.datasets[5].data = vocData;
    
    // 确保数据点数量不超过最大值
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
    
    const brightnessSlider = document.getElementById('ledBrightness');
    const brightnessValueDisplay = document.getElementById('ledBrightnessValue');
    if (brightnessSlider && brightnessValueDisplay) {
        brightnessSlider.addEventListener('input', () => {
            brightnessValueDisplay.textContent = `${brightnessSlider.value}%`;
        });
    }
    document.getElementById('saveLedBrightnessButton')?.addEventListener('click', handleSaveLedBrightness);
}

/**
 * 处理扫描WiFi网络的请求
 */
function handleScanWifi() {
    if (websocket && websocket.readyState === WebSocket.OPEN) {
        websocket.send(JSON.stringify({ action: 'scanWifi' }));
        updateStatusMessage('scan-status', translations[currentLang]?.wifi_scanning || '正在扫描WiFi...', false);
    } else {
        updateStatusMessage('scan-status', translations[currentLang]?.ws_not_connected || 'WebSocket 未连接.', true);
    }
}

/**
 * 显示WiFi扫描结果
 */
function displayWifiScanResults(data) {
    const container = document.getElementById('ssid-list-container');
    const scanStatusEl = document.getElementById('scan-status');
    if (!container || !scanStatusEl) return;
    
    container.innerHTML = ''; // 清空旧结果
    if (data.error) {
        scanStatusEl.textContent = data.error;
        scanStatusEl.style.color = 'var(--danger-color)';
        container.style.display = 'none';
        return;
    }

    const networks = data.networks;
    if (networks && networks.length > 0) {
        networks.forEach(net => {
            const button = document.createElement('button');
            button.type = 'button';
            button.textContent = `${net.ssid} (${net.rssi} dBm, ${net.encryption})`;
            button.onclick = () => {
                document.getElementById('wifiSSID').value = net.ssid;
                container.style.display = 'none'; // 选择后隐藏列表
            };
            container.appendChild(button);
        });
        scanStatusEl.textContent = `${networks.length} ${translations[currentLang]?.networks_found_status || '个网络被发现.'}`;
        scanStatusEl.style.color = 'var(--text-light-color)';
        container.style.display = 'block';
    } else {
        scanStatusEl.textContent = translations[currentLang]?.no_networks_found_status || '未找到网络或扫描失败。';
        scanStatusEl.style.color = 'var(--text-light-color)';
        container.style.display = 'none';
    }
}

/**
 * 处理连接WiFi的请求
 */
function handleConnectWifi() {
    const ssid = document.getElementById('wifiSSID')?.value.trim();
    const password = document.getElementById('wifiPassword')?.value; // 注意：密码字段类型为text
    if (!ssid) {
        updateStatusMessage('connect-wifi-status', translations[currentLang]?.wifi_ssid_empty || 'SSID 不能为空.', true);
        return;
    }
    if (websocket && websocket.readyState === WebSocket.OPEN) {
        websocket.send(JSON.stringify({ action: 'connectWifi', ssid: ssid, password: password }));
        updateStatusMessage('connect-wifi-status', translations[currentLang]?.connecting_wifi || '正在连接...', false);
    } else {
        updateStatusMessage('connect-wifi-status', translations[currentLang]?.ws_not_connected || 'WebSocket 未连接.', true);
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
        coMin: parseFloat(document.getElementById('coMin')?.value),
        coMax: parseFloat(document.getElementById('coMax')?.value),
        no2Min: parseFloat(document.getElementById('no2Min')?.value),
        no2Max: parseFloat(document.getElementById('no2Max')?.value),
        c2h5ohMin: parseFloat(document.getElementById('c2h5ohMin')?.value),
        c2h5ohMax: parseFloat(document.getElementById('c2h5ohMax')?.value),
        vocMin: parseFloat(document.getElementById('vocMin')?.value),
        vocMax: parseFloat(document.getElementById('vocMax')?.value),
    };

    // 简单校验
    for (const key in thresholds) {
        if (key !== 'action' && isNaN(thresholds[key])) {
            updateStatusMessage('save-settings-status', translations[currentLang]?.settings_invalid_threshold || '所有阈值都必须是数字。', true); return;
        }
    }
    if (thresholds.tempMin >= thresholds.tempMax || thresholds.humMin >= thresholds.humMax || 
        thresholds.coMin >= thresholds.coMax || thresholds.no2Min >= thresholds.no2Max ||
        thresholds.c2h5ohMin >= thresholds.c2h5ohMax || thresholds.vocMin >= thresholds.vocMax) {
         updateStatusMessage('save-settings-status', translations[currentLang]?.settings_min_max_error || '每个阈值的最小值必须小于最大值。', true); return;
    }

    if (websocket && websocket.readyState === WebSocket.OPEN) {
        websocket.send(JSON.stringify(thresholds));
        updateStatusMessage('save-settings-status', translations[currentLang]?.settings_saving || '正在保存设置...', false);
    } else {
        updateStatusMessage('save-settings-status', translations[currentLang]?.ws_not_connected || 'WebSocket 未连接.', true);
    }
}

/**
 * 处理保存LED亮度的请求
 */
function handleSaveLedBrightness() {
    const brightness = parseInt(document.getElementById('ledBrightness')?.value, 10);
    if (isNaN(brightness) || brightness < 0 || brightness > 100) {
        updateStatusMessage('save-led-status', translations[currentLang]?.led_brightness_invalid || '亮度值必须在 0 和 100 之间。', true);
        return;
    }
    if (websocket && websocket.readyState === WebSocket.OPEN) {
        websocket.send(JSON.stringify({ action: 'saveLedBrightness', brightness: brightness }));
        updateStatusMessage('save-led-status', translations[currentLang]?.led_brightness_saving || '正在保存亮度...', false);
    } else {
        updateStatusMessage('save-led-status', translations[currentLang]?.ws_not_connected || 'WebSocket 未连接.', true);
    }
}


/**
 * 处理恢复出厂设置的请求
 */
function handleResetSettings() {
    const confirmationText = translations[currentLang]?.reset_settings_prompt || "确定要重置所有设置吗？此操作无法撤销。";
    if (confirm(confirmationText)) {
        if (websocket && websocket.readyState === WebSocket.OPEN) {
            websocket.send(JSON.stringify({ action: 'resetSettings' }));
            updateStatusMessage('reset-status', translations[currentLang]?.settings_resetting || '正在重置设置...', false);
        } else {
            updateStatusMessage('reset-status', translations[currentLang]?.ws_not_connected || 'WebSocket 未连接.', true);
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
        const fields = ['tempMin', 'tempMax', 'humMin', 'humMax', 'coMin', 'coMax', 'no2Min', 'no2Max', 'c2h5ohMin', 'c2h5ohMax', 'vocMin', 'vocMax'];
        fields.forEach(field => {
            const el = document.getElementById(field);
            if (el && t[field] !== undefined) el.value = t[field];
        });
    }
    if (settings.currentSSID !== undefined) {
        const ssidEl = document.getElementById('wifiSSID');
        if (ssidEl) ssidEl.value = settings.currentSSID;
    }
    if (settings.ledBrightness !== undefined) {
        const brightnessSlider = document.getElementById('ledBrightness');
        const brightnessValueDisplay = document.getElementById('ledBrightnessValue');
        if (brightnessSlider) brightnessSlider.value = settings.ledBrightness;
        if (brightnessValueDisplay) brightnessValueDisplay.textContent = `${settings.ledBrightness}%`;
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
        el.style.color = isError ? 'var(--danger-color)' : 'var(--success-color)'; // 假设有这些CSS变量
        setTimeout(() => { if (el.textContent === message) el.textContent = ''; }, 5000); // 5秒后清除
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
