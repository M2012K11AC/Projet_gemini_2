// ==========================================================================
// == ESP32 环境监测器前端脚本 (V4.1 - 细节优化) ==
// ==========================================================================

const App = {
    // 1. 状态和配置变量
    websocket: null,
    currentLang: 'zh',
    translations: {},
    wsReconnectInterval: 3000,
    wsMaxReconnectAttempts: 5,
    wsReconnectAttempts: 0,
    currentWsStatusKey: '',
    statusBannerTimeout: null,

    // 图表相关的状态
    charts: {
        tempHum: {
            instance: null,
            labels: [],
            datasets: { temp: [], hum: [] }
        },
        gas: {
            instance: null,
            labels: [],
            datasets: { co: [], no2: [], c2h5oh: [], voc: [] }
        }
    },
    MAX_CHART_DATA_POINTS: 90, // 与后端 HISTORICAL_DATA_POINTS 保持一致

    // 2. 初始化方法
    init() {
        // 确保DOM加载完毕
        document.addEventListener('DOMContentLoaded', () => {
            this.loadTranslations().then(() => {
                const savedLang = localStorage.getItem('preferredLang') || 'zh';
                this.setLanguage(savedLang); // 应用翻译并初始化页面
            });
        });
    },

    // 页面加载和语言切换
    initPage() {
        this.connectWebSocket();

        // 根据页面ID判断需要初始化哪些内容
        if (document.querySelector('body')) { // 通用
            document.getElementById('lang-zh')?.addEventListener('click', () => this.setLanguage('zh'));
            document.getElementById('lang-fr')?.addEventListener('click', () => this.setLanguage('fr'));
        }
        if (document.getElementById('tempHumChart')) { // 主页
            this.initTempHumChart();
            this.initGasChart();
        }
        if (document.getElementById('wifiConfigForm')) { // 设置页
            this.setupSettingsPageListeners();
        }
    },

    async loadTranslations() {
        try {
            const response = await fetch('lang.json');
            if (!response.ok) throw new Error(`Failed to load lang.json: ${response.statusText}`);
            this.translations = await response.json();
        } catch (error) {
            console.error('Error fetching lang.json:', error);
        }
    },

    setLanguage(lang) {
        this.currentLang = lang;
        localStorage.setItem('preferredLang', lang);

        document.querySelectorAll('.lang-button').forEach(btn => btn.classList.remove('active'));
        document.getElementById(`lang-${lang}`)?.classList.add('active');

        this.applyTranslations();
        // 仅在 setLanguage 第一次被调用时初始化页面
        if (!this.websocket) {
            this.initPage();
        }
    },

    applyTranslations() {
        const lang = this.currentLang;
        document.documentElement.lang = lang;
        document.querySelectorAll('[data-translate]').forEach(el => {
            const key = el.getAttribute('data-translate');
            if (this.translations[lang]?.[key]) {
                const translation = this.translations[lang][key];
                if (el.tagName === 'INPUT' && ['submit', 'button'].includes(el.type)) {
                    el.value = translation;
                } else if (el.hasAttribute('placeholder') && this.translations[lang][key + '_placeholder']) {
                    el.placeholder = this.translations[lang][key + '_placeholder'];
                } else {
                    el.textContent = translation;
                }
            }
        });
        const pageTitleKey = document.querySelector('head > title[data-translate]')?.getAttribute('data-translate');
        if (pageTitleKey && this.translations[lang]?.[pageTitleKey]) {
            document.title = this.translations[lang][pageTitleKey];
        }
        document.querySelectorAll('[data-translate-title]').forEach(el => {
            const key = el.getAttribute('data-translate-title');
            if (this.translations[lang]?.[key]) el.title = this.translations[lang][key];
        });

        // 更新图表翻译
        this.updateChartTranslations();
        // 更新状态横幅的翻译
        this.updateConnectionBannerText();
    },

    // 3. WebSocket 核心逻辑
    connectWebSocket() {
        const gateway = `ws://${window.location.hostname}:81/`;
        this.updateConnectionBanner('ws_connecting', 'connecting');
        console.log(`Attempting to connect to WebSocket at ${gateway}`);
        this.websocket = new WebSocket(gateway);

        this.websocket.onopen = (event) => this.onWsOpen(event);
        this.websocket.onmessage = (event) => this.onWsMessage(event);
        this.websocket.onclose = (event) => this.onWsClose(event);
        this.websocket.onerror = (error) => this.onWsError(error);
    },

    onWsOpen(event) {
        console.log('WebSocket connection opened successfully.');
        this.wsReconnectAttempts = 0;
        this.updateConnectionBanner('ws_connected', 'connected', true);

        // 连接成功后根据页面请求初始数据
        if (document.getElementById('wifiConfigForm')) {
            this.sendMessage({ action: "getCurrentSettings" });
        } else if (document.getElementById('tempHumChart')) {
            setTimeout(() => this.sendMessage({ action: "getHistoricalData" }), 500);
        }
    },

    onWsMessage(event) {
        let data;
        try {
            data = JSON.parse(event.data);
        } catch (e) {
            console.error('Error parsing WebSocket message JSON:', e, "Raw data:", event.data);
            return;
        }

        const handler = {
            'sensorData': (d) => this.handleSensorData(d),
            'wifiStatus': (d) => this.handleWifiStatus(d),
            'historicalData': (d) => this.populateChartsWithHistoricalData(d.history),
            'settingsData': (d) => this.populateSettingsForm(d.settings),
            'wifiScanResults': (d) => this.displayWifiScanResults(d),
            'connectWifiStatus': (d) => this.updateStatusMessage('connect-wifi-status', d.message, d.success ? 'success' : 'failed'),
            'saveSettingsStatus': (d) => this.updateStatusMessage('save-settings-status', d.message, d.success ? 'success' : 'failed'),
            'saveBrightnessStatus': (d) => this.updateStatusMessage('save-led-status', d.message, d.success ? 'success' : 'failed'),
            'resetStatus': (d) => {
                this.updateStatusMessage('reset-status', d.message, d.success ? 'success' : 'failed');
                if(d.success) alert(this.translations[this.currentLang]?.settings_reset_success || "Settings reset. Device will restart.");
            },
            'calibrationStatusUpdate': (d) => this.handleCalibrationStatus(d.calibration),
            'error': (d) => {
                console.error('Error message from server:', d.message);
                this.updateStatusMessage('general-status', d.message, 'failed');
            },
            'scanStatus': (d) => this.updateStatusMessage('scan-status', d.message, 'neutral'),
        }[data.type];

        if (handler) {
            handler(data);
        }
    },

    onWsClose(event) {
        console.log('WebSocket connection closed. Event:', event);

        if (this.wsReconnectAttempts < this.wsMaxReconnectAttempts) {
            this.wsReconnectAttempts++;
            this.updateConnectionBanner('ws_disconnected_retry_attempt', 'disconnected', false);
            console.log(`Reconnecting in ${this.wsReconnectInterval / 1000}s (attempt ${this.wsReconnectAttempts}/${this.wsMaxReconnectAttempts})`);
            setTimeout(() => this.connectWebSocket(), this.wsReconnectInterval);
        } else {
            this.updateConnectionBanner('ws_reconnect_failed', 'error');
            console.error('WebSocket reconnection failed after multiple attempts.');
        }
    },

    onWsError(error) {
        console.error('WebSocket error:', error);
        this.updateConnectionBanner('ws_error', 'error');
    },

    sendMessage(obj) {
        if (this.websocket && this.websocket.readyState === WebSocket.OPEN) {
            this.websocket.send(JSON.stringify(obj));
        } else {
            console.warn("WebSocket is not connected. Message not sent:", obj);
        }
    },

    // 4. 数据处理和UI更新
    handleSensorData(data) {
        this.updateElementText('tempVal', data.temperature !== null ? data.temperature : '--');
        this.updateElementText('humVal', data.humidity !== null ? data.humidity : '--');
        this.updateElementText('coVal', data.gasPpm?.co?.toFixed(2) || '--');
        this.updateElementText('no2Val', data.gasPpm?.no2?.toFixed(2) || '--');
        this.updateElementText('c2h5ohVal', data.gasPpm?.c2h5oh?.toFixed(1) || '--');
        this.updateElementText('vocVal', data.gasPpm?.voc?.toFixed(2) || '--');

        this.updateStatusIndicator('temp-status-indicator', data.tempStatus);
        this.updateStatusIndicator('hum-status-indicator', data.humStatus);
        this.updateStatusIndicator('gasCo-status-indicator', data.gasCoStatus);
        this.updateStatusIndicator('gasNo2-status-indicator', data.gasNo2Status);
        this.updateStatusIndicator('gasC2h5oh-status-indicator', data.gasC2h5ohStatus);
        this.updateStatusIndicator('gasVoc-status-indicator', data.gasVocStatus);

        if (data.timeStr) {
            this.addDataToTempHumChart(data.timeStr, data.temperature, data.humidity);
            this.addDataToGasChart(data.timeStr, data.gasPpm);
        }
    },

    handleWifiStatus(data) {
        const statusTextEl = document.getElementById('wifiStatusText');
        const ntpStatusEl = document.getElementById('ntpStatusText');

        if (statusTextEl) {
            let translationKey = 'wifi_disconnected';
            let statusClass = 'status-neutral'; // Default state: neutral color

            if (data.connected) {
                translationKey = 'wifi_connected_to';
                statusClass = 'status-connected'; // Green
            } else if (data.connecting_attempt_ssid) {
                translationKey = 'wifi_connecting_to';
                statusClass = 'status-connecting'; // Blue
            } else if (data.connection_failed) {
                translationKey = 'wifi_connection_failed';
                statusClass = 'status-failed'; // Red
            }

            let text = this.translations[this.currentLang]?.[translationKey] || translationKey;
            if (data.connected) text = text.replace('{ssid}', data.ssid);
            if (data.connecting_attempt_ssid) text = text.replace('{ssid}', data.connecting_attempt_ssid);

            statusTextEl.textContent = text;
            statusTextEl.className = `status-text-dynamic ${statusClass}`;

            if (!data.connected && data.ap_mode) {
                statusTextEl.textContent += ` (AP: ${data.ap_ssid || 'ESP32_Sensor_Hub_V2'})`;
            }
        }

        if (ntpStatusEl) {
            let key = data.ntp_synced ? 'ntp_status_synced' : 'ntp_status_failed';
            ntpStatusEl.textContent = this.translations[this.currentLang]?.[key] || key;
            ntpStatusEl.style.color = data.ntp_synced ? 'var(--success-color)' : 'var(--warning-color)';
        }

        this.updateElementText('wifiSSIDText', data.ssid || 'N/A');
        this.updateElementText('wifiIPText', data.ip || 'N/A');
    },

    updateStatusIndicator(elementId, status) {
        const indicator = document.getElementById(elementId);
        if (indicator) {
            indicator.className = 'status-indicator ' + (status || '');
        }
    },

    updateElementText(id, text) {
        const element = document.getElementById(id);
        if (element) element.textContent = text;
    },

    updateConnectionBanner(statusKey, bannerClass, autoHide = false) {
        const statusBanner = document.getElementById('connection-status-banner');
        this.currentWsStatusKey = statusKey;

        if (statusBanner) {
            this.updateConnectionBannerText(); // 更新文本
            statusBanner.className = `status-banner ${bannerClass}`;
            statusBanner.style.display = 'block';

            if (this.statusBannerTimeout) clearTimeout(this.statusBannerTimeout);
            if (autoHide) {
                this.statusBannerTimeout = setTimeout(() => {
                    statusBanner.style.display = 'none';
                    this.currentWsStatusKey = '';
                }, 3000);
            }
        }
    },

    updateConnectionBannerText() {
        if (!this.currentWsStatusKey) return;
        let message = this.translations[this.currentLang]?.[this.currentWsStatusKey] || this.currentWsStatusKey.replace(/_/g, ' ');
        if (this.currentWsStatusKey === 'ws_disconnected_retry_attempt') {
            message = message.replace('{attempts}', this.wsReconnectAttempts).replace('{maxAttempts}', this.wsMaxReconnectAttempts);
        }
        this.updateElementText('connection-status-banner', message);
    },

    // 5. Chart.js Logic
    initTempHumChart() {
        const ctx = document.getElementById('tempHumChart')?.getContext('2d');
        if (!ctx) return;

        this.charts.tempHum.instance = new Chart(ctx, {
            type: 'line',
            data: {
                labels: this.charts.tempHum.labels,
                datasets: [
                    {
                        label: this.translations[this.currentLang]?.chart_temperature_label || 'Temperature (°C)',
                        data: this.charts.tempHum.datasets.temp,
                        borderColor: 'rgba(255, 99, 132, 1)',
                        yAxisID: 'yTemp',
                        tension: 0.3,
                        pointRadius: 0
                    },
                    {
                        label: this.translations[this.currentLang]?.chart_humidity_label || 'Humidity (%)',
                        data: this.charts.tempHum.datasets.hum,
                        borderColor: 'rgba(54, 162, 235, 1)',
                        yAxisID: 'yHum',
                        tension: 0.3,
                        pointRadius: 0
                    }
                ]
            },
            options: this.getCommonChartOptions({
                yTemp: { type: 'linear', position: 'left', min: 0, max: 50, title: { display: true, text: '°C' } },
                yHum: { type: 'linear', position: 'right', min: 0, max: 100, title: { display: true, text: '%' }, grid: { drawOnChartArea: false } }
            })
        });
    },

    initGasChart() {
        const ctx = document.getElementById('gasChart')?.getContext('2d');
        if (!ctx) return;
        const lang = this.currentLang;

        this.charts.gas.instance = new Chart(ctx, {
            type: 'line',
            data: {
                labels: this.charts.gas.labels,
                datasets: [
                    { label: this.translations[lang]?.gas_co_label || 'CO', data: this.charts.gas.datasets.co, borderColor: 'rgba(255, 159, 64, 1)', tension: 0.3, pointRadius: 0 },
                    { label: this.translations[lang]?.gas_no2_label || 'NO2', data: this.charts.gas.datasets.no2, borderColor: 'rgba(153, 102, 255, 1)', tension: 0.3, pointRadius: 0 },
                    { label: this.translations[lang]?.gas_c2h5oh_label || 'C2H5OH', data: this.charts.gas.datasets.c2h5oh, borderColor: 'rgba(75, 192, 192, 1)', tension: 0.3, pointRadius: 0 },
                    { label: this.translations[lang]?.gas_voc_label || 'VOC', data: this.charts.gas.datasets.voc, borderColor: 'rgba(255, 205, 86, 1)', tension: 0.3, pointRadius: 0 }
                ]
            },
            options: this.getCommonChartOptions({
                y: { type: 'logarithmic', position: 'left', title: { display: true, text: 'PPM' } }
            })
        });
    },

    getCommonChartOptions(scales) {
        return {
            responsive: true, maintainAspectRatio: false, animation: { duration: 200 },
            scales: {
                x: {
                    ticks: { autoSkip: true, maxTicksLimit: 10, font: { size: 10 } },
                    grid: { display: true, color: 'rgba(200, 200, 200, 0.1)' }
                },
                ...scales
            },
            plugins: {
                tooltip: { mode: 'index', intersect: false },
                legend: { position: 'top', labels: { font: { size: 11 }, boxWidth: 15, padding: 10 } },
            },
            interaction: { mode: 'index', intersect: false },
        }
    },

    addDataToChart(chartObj, label, data) {
        if (!chartObj.instance) return;
        chartObj.labels.push(label);
        Object.keys(data).forEach(key => {
            chartObj.datasets[key]?.push(data[key]);
        });

        if (chartObj.labels.length > this.MAX_CHART_DATA_POINTS) {
            chartObj.labels.shift();
            Object.values(chartObj.datasets).forEach(d => d.shift());
        }
        chartObj.instance.update('none');
    },

    addDataToTempHumChart(label, temp, hum) {
        this.addDataToChart(this.charts.tempHum, label, { temp, hum });
    },

    addDataToGasChart(label, gasData) {
        if (gasData) {
            this.addDataToChart(this.charts.gas, label, gasData);
        }
    },

    populateChartsWithHistoricalData(history) {
        if (!history || !Array.isArray(history)) return;

        Object.values(this.charts).forEach(chart => {
            chart.labels.length = 0;
            Object.values(chart.datasets).forEach(d => d.length = 0);
        });

        history.forEach(record => {
            this.charts.tempHum.labels.push(record.time);
            this.charts.tempHum.datasets.temp.push(record.temp);
            this.charts.tempHum.datasets.hum.push(record.hum);

            this.charts.gas.labels.push(record.time);
            this.charts.gas.datasets.co.push(record.co);
            this.charts.gas.datasets.no2.push(record.no2);
            this.charts.gas.datasets.c2h5oh.push(record.c2h5oh);
            this.charts.gas.datasets.voc.push(record.voc);
        });

        this.charts.tempHum.instance?.update('none');
        this.charts.gas.instance?.update('none');
        console.log("Charts populated with historical data. Points:", history.length);
    },

    updateChartTranslations() {
        const lang = this.currentLang;
        if (!this.translations[lang]) return;

        const tempHumChart = this.charts.tempHum.instance;
        if (tempHumChart) {
            tempHumChart.data.datasets[0].label = this.translations[lang].chart_temperature_label || 'Temperature (°C)';
            tempHumChart.data.datasets[1].label = this.translations[lang].chart_humidity_label || 'Humidity (%)';
            tempHumChart.options.scales.yTemp.title.text = this.translations[lang].unit_celsius || '°C';
            tempHumChart.options.scales.yHum.title.text = this.translations[lang].unit_percent || '%';
            tempHumChart.update('none');
        }

        const gasChart = this.charts.gas.instance;
        if (gasChart) {
            gasChart.data.datasets[0].label = this.translations[lang].gas_co_label || 'CO';
            gasChart.data.datasets[1].label = this.translations[lang].gas_no2_label || 'NO2';
            gasChart.data.datasets[2].label = this.translations[lang].gas_c2h5oh_label || 'C2H5OH';
            gasChart.data.datasets[3].label = this.translations[lang].gas_voc_label || 'VOC';
            gasChart.options.scales.y.title.text = this.translations[lang].unit_ppm || 'PPM';
            gasChart.update('none');
        }
    },


    // 6. Settings Page Logic
    setupSettingsPageListeners() {
        document.getElementById('scanWifiButton')?.addEventListener('click', () => this.handleScanWifi());
        document.getElementById('connectWifiButton')?.addEventListener('click', () => this.handleConnectWifi());
        document.getElementById('saveThresholdsButton')?.addEventListener('click', () => this.handleSaveThresholds());
        document.getElementById('resetSettingsButton')?.addEventListener('click', () => this.handleResetSettings());

        const brightnessSlider = document.getElementById('ledBrightness');
        const brightnessValueDisplay = document.getElementById('ledBrightnessValue');
        if (brightnessSlider && brightnessValueDisplay) {
            brightnessSlider.addEventListener('input', () => {
                brightnessValueDisplay.textContent = `${brightnessSlider.value}%`;
            });
        }
        document.getElementById('saveLedBrightnessButton')?.addEventListener('click', () => this.handleSaveLedBrightness());
        document.getElementById('startCalibrationButton')?.addEventListener('click', () => this.handleStartCalibration());
    },

    handleStartCalibration() {
        const confirmationText = this.translations[this.currentLang]?.calibration_confirm || "请确认设备已在洁净空气中且稳定。校准将开始，耗时约1分钟。";
        if (confirm(confirmationText)) {
            this.sendMessage({ action: 'startCalibration' });
            this.updateStatusMessage('calibration-status', this.translations[this.currentLang]?.calibration_starting || '正在启动校准...', 'connecting');
        }
    },

    handleCalibrationStatus(data) {
        const caliForm = document.getElementById('calibrationSettingsForm');
        const caliButton = document.getElementById('startCalibrationButton');
        const progressContainer = document.getElementById('calibration-progress-container');
        const progressBar = document.getElementById('calibration-progress-bar');
        const statusEl = document.getElementById('calibration-status');

        if (!caliForm || !caliButton || !progressContainer || !progressBar || !statusEl) return;
        
        // 0: IDLE, 1: IN_PROGRESS, 2: COMPLETED, 3: FAILED
        switch (data.state) {
            case 1: // In Progress
                progressContainer.style.display = 'block';
                caliForm.classList.add('calibrating');
                caliButton.disabled = true;
                
                progressBar.style.width = `${data.progress}%`;
                progressBar.textContent = `${data.progress}%`;
                this.updateStatusMessage('calibration-status', this.translations[this.currentLang]?.calibration_inprogress || '校准中，请勿移动设备...', 'connecting');
                
                this.updateElementText('measured_r0_co', data.measuredR0.co?.toFixed(2) || '--');
                this.updateElementText('measured_r0_no2', data.measuredR0.no2?.toFixed(2) || '--');
                this.updateElementText('measured_r0_c2h5oh', data.measuredR0.c2h5oh?.toFixed(2) || '--');
                this.updateElementText('measured_r0_voc', data.measuredR0.voc?.toFixed(2) || '--');
                break;
            case 2: // Completed
                progressBar.style.width = '100%';
                progressBar.textContent = '100%';
                caliButton.disabled = true;
                this.updateStatusMessage('calibration-status', this.translations[this.currentLang]?.calibration_success_reboot || '校准成功！设备将在3秒后重启。', 'success');
                break;
            case 3: // Failed
                progressContainer.style.display = 'none';
                caliForm.classList.remove('calibrating');
                caliButton.disabled = false;
                this.updateStatusMessage('calibration-status', this.translations[this.currentLang]?.calibration_failed || '校准失败，请重试。', 'failed');
                break;
            case 0: // Idle
            default:
                progressContainer.style.display = 'none';
                caliForm.classList.remove('calibrating');
                caliButton.disabled = false;
                statusEl.textContent = '';
                break;
        }
    },

    handleScanWifi() {
        this.sendMessage({ action: 'scanWifi' });
        this.updateStatusMessage('scan-status', this.translations[this.currentLang]?.wifi_scanning || 'Scanning WiFi...', 'connecting');
    },

    displayWifiScanResults(data) {
        const container = document.getElementById('ssid-list-container');
        if (!container) return;

        container.innerHTML = '';
        if (data.error) {
            this.updateStatusMessage('scan-status', data.error, 'failed');
            container.style.display = 'none';
            return;
        }

        const networks = data.networks;
        if (networks?.length > 0) {
            networks.forEach(net => {
                const button = document.createElement('button');
                button.type = 'button';
                button.textContent = `${net.ssid} (${net.rssi} dBm)`;
                button.onclick = () => {
                    document.getElementById('wifiSSID').value = net.ssid;
                    container.style.display = 'none';
                };
                container.appendChild(button);
            });
            const foundMsg = `${networks.length} ${this.translations[this.currentLang]?.networks_found_status || 'networks found.'}`;
            this.updateStatusMessage('scan-status', foundMsg, 'success');
            container.style.display = 'block';
        } else {
            this.updateStatusMessage('scan-status', this.translations[this.currentLang]?.no_networks_found_status || 'No networks found.', 'neutral');
            container.style.display = 'none';
        }
    },

    handleConnectWifi() {
        const ssid = document.getElementById('wifiSSID')?.value.trim();
        const password = document.getElementById('wifiPassword')?.value;
        if (!ssid) {
            this.updateStatusMessage('connect-wifi-status', this.translations[this.currentLang]?.wifi_ssid_empty || 'SSID cannot be empty.', 'failed');
            return;
        }
        this.sendMessage({ action: 'connectWifi', ssid, password });
        this.updateStatusMessage('connect-wifi-status', this.translations[this.currentLang]?.connecting_wifi || 'Connecting...', 'connecting');
    },

    handleSaveThresholds() {
        const thresholds = {
            action: 'saveThresholds',
            tempMin: parseInt(document.getElementById('tempMin')?.value, 10),
            tempMax: parseInt(document.getElementById('tempMax')?.value, 10),
            humMin: parseInt(document.getElementById('humMin')?.value, 10),
            humMax: parseInt(document.getElementById('humMax')?.value, 10),
            coPpmMax: parseFloat(document.getElementById('coPpmMax')?.value),
            no2PpmMax: parseFloat(document.getElementById('no2PpmMax')?.value),
            c2h5ohPpmMax: parseFloat(document.getElementById('c2h5ohPpmMax')?.value),
            vocPpmMax: parseFloat(document.getElementById('vocPpmMax')?.value),
        };

        for (const key in thresholds) {
            if (key !== 'action' && isNaN(thresholds[key])) {
                this.updateStatusMessage('save-settings-status', this.translations[this.currentLang]?.settings_invalid_threshold || 'All thresholds must be numbers.', 'failed'); return;
            }
        }
        this.sendMessage(thresholds);
        this.updateStatusMessage('save-settings-status', this.translations[this.currentLang]?.settings_saving || 'Saving settings...', 'neutral');
    },

    handleSaveLedBrightness() {
        const brightness = parseInt(document.getElementById('ledBrightness')?.value, 10);
        if (isNaN(brightness) || brightness < 0 || brightness > 100) {
            this.updateStatusMessage('save-led-status', this.translations[this.currentLang]?.led_brightness_invalid || 'Brightness must be between 0 and 100.', 'failed');
            return;
        }
        this.sendMessage({ action: 'saveLedBrightness', brightness });
        this.updateStatusMessage('save-led-status', this.translations[this.currentLang]?.led_brightness_saving || 'Saving brightness...', 'neutral');
    },

    handleResetSettings() {
        const confirmationText = this.translations[this.currentLang]?.reset_settings_prompt || "Are you sure? This cannot be undone.";
        if (confirm(confirmationText)) {
            this.sendMessage({ action: 'resetSettings' });
            this.updateStatusMessage('reset-status', this.translations[this.currentLang]?.settings_resetting || 'Resetting...', 'neutral');
        }
    },

    populateSettingsForm(settings) {
        if (!settings) return;
        if (settings.thresholds) {
            const t = settings.thresholds;
            const fields = ['tempMin', 'tempMax', 'humMin', 'humMax', 'coPpmMax', 'no2PpmMax', 'c2h5ohPpmMax', 'vocPpmMax'];
            fields.forEach(field => this.updateElementValue(field, t[field]));
        }
        if (settings.r0Values) {
            const r0 = settings.r0Values;
            this.updateElementText('current_r0_co', r0.co?.toFixed(2) || '--');
            this.updateElementText('current_r0_no2', r0.no2?.toFixed(2) || '--');
            this.updateElementText('current_r0_c2h5oh', r0.c2h5oh?.toFixed(2) || '--');
            this.updateElementText('current_r0_voc', r0.voc?.toFixed(2) || '--');
        }
        this.updateElementValue('wifiSSID', settings.currentSSID);
        this.updateElementValue('ledBrightness', settings.ledBrightness);
        const ledValue = document.getElementById('ledBrightnessValue');
        if (ledValue) ledValue.textContent = `${settings.ledBrightness}%`;
    },

    updateElementValue(id, value) {
        const el = document.getElementById(id);
        if (el && value !== undefined) el.value = value;
    },

    updateStatusMessage(elementId, message, status = 'neutral') {
        const el = document.getElementById(elementId);
        if (el) {
            el.textContent = message;

            el.classList.remove('status-connecting', 'status-success', 'status-failed');

            if (status !== 'neutral') {
                el.classList.add(`status-${status}`);
            }

            if (status !== 'connecting') {
                 setTimeout(() => {
                    if (el.textContent === message) {
                       el.textContent = '';
                       el.classList.remove('status-connecting', 'status-success', 'status-failed');
                    }
                }, 5000);
            }
        }
    }
};

// 启动应用
App.init();
