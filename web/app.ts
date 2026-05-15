// Описание одной строки таблицы процессов, которую backend присылает в JSON
interface ProcessInfo {
    pid: number;
    user: string;
    command: string;
    state: string;
    cpu: number;
    memoryKb: number;
    threads: number;
}

// Полный снимок системы. Структура совпадает с JSON из /api/stats
interface Snapshot {
    timestamp: string;
    platform: string;
    uptimeSeconds: number;
    loadAverage: number[];
    runningProcesses: number;
    totalProcesses: number;
    cpu: {
        total: number;
        cores: number[];
    };
    memory: {
        totalKb: number;
        usedKb: number;
        availableKb: number;
        percent: number;
    };
    swap: {
        totalKb: number;
        usedKb: number;
        percent: number;
    };
    network: {
        rxBytes: number;
        txBytes: number;
        rxRate: number;
        txRate: number;
    };
    disk: {
        readBytes: number;
        writeBytes: number;
        readRate: number;
        writeRate: number;
    };
    processes: ProcessInfo[];
    error: string | null;
}

// Частота обновления UI. Backend хранит прошлые снимки, поэтому 1 секунда удобна для расчета CPU% и скоростей сети/диска
const refreshMs = 1000;

// Безопасный поиск DOM-элемента: если id изменили в HTML, ошибка будет понятной
function element<T extends HTMLElement>(selector: string): T {
    const node = document.querySelector<T>(selector);
    if (!node) {
        throw new Error(`Не найден элемент ${selector}`);
    }
    return node;
}

// Все ссылки на элементы собираем в одном месте, чтобы render() был коротким

const ui = {
    connectionDot: element<HTMLSpanElement>("#connection-dot"),
    connectionText: element<HTMLSpanElement>("#connection-text"),
    cpuTotal: element<HTMLElement>("#cpu-total"),
    cpuTotalBar: element<HTMLSpanElement>("#cpu-total-bar"),
    memoryPercent: element<HTMLElement>("#memory-percent"),
    memoryText: element<HTMLElement>("#memory-text"),
    memoryBar: element<HTMLSpanElement>("#memory-bar"),
    swapPercent: element<HTMLElement>("#swap-percent"),
    swapText: element<HTMLElement>("#swap-text"),
    swapBar: element<HTMLSpanElement>("#swap-bar"),
    loadAverage: element<HTMLElement>("#load-average"),
    processCount: element<HTMLElement>("#process-count"),
    cpuCoreCount: element<HTMLElement>("#cpu-core-count"),
    cpuCores: element<HTMLElement>("#cpu-cores"),
    timestamp: element<HTMLElement>("#timestamp"),
    networkRx: element<HTMLElement>("#network-rx"),
    networkTx: element<HTMLElement>("#network-tx"),
    diskRead: element<HTMLElement>("#disk-read"),
    diskWrite: element<HTMLElement>("#disk-write"),
    uptime: element<HTMLElement>("#uptime"),
    platform: element<HTMLElement>("#platform"),
    processes: element<HTMLTableSectionElement>("#processes"),
};

function clampPercent(value: number): number {
    return Math.max(0, Math.min(100, Number.isFinite(value) ? value : 0));
}

// Читаемый процент с одним знаком после запятой
function percentText(value: number): string {
    return `${clampPercent(value).toFixed(1)}%`;
}

// Цвет шкалы зависит от нагрузки: зеленый, желтый, красный
function colorForLoad(value: number): string {
    const percent = clampPercent(value);
    if (percent >= 85) return "var(--red)";
    if (percent >= 65) return "var(--yellow)";
    return "var(--green)";
}

// Обновляет ширину и цвет progress bar
function setProgress(bar: HTMLElement, value: number): void {
    const percent = clampPercent(value);
    bar.style.width = `${percent}%`;
    bar.style.background = colorForLoad(percent);
}

// Переводит байты в B/KB/MB/GB/TB для компактного отображения
function formatBytes(bytes: number): string {
    const units = ["B", "KB", "MB", "GB", "TB"];
    let value = Math.max(0, bytes);
    let unitIndex = 0;

    while (value >= 1024 && unitIndex < units.length - 1) {
        value /= 1024;
        unitIndex += 1;
    }

    return `${value.toFixed(unitIndex === 0 ? 0 : 1)} ${units[unitIndex]}`;
}

// Backend присылает память в KiB, а UI показывает ее в человекочитаемом виде
function formatKb(kb: number): string {
    return formatBytes(kb * 1024);
}

// Скорость сети/диска отображаем как "MB/s", "KB/s" и т.д.
function formatRate(bytesPerSecond: number): string {
    return `${formatBytes(bytesPerSecond)}/s`;
}

// Uptime превращаем из секунд в короткую строку: дни/часы/минуты
function formatDuration(totalSeconds: number): string {
    const seconds = Math.max(0, Math.floor(totalSeconds));
    const days = Math.floor(seconds / 86400);
    const hours = Math.floor((seconds % 86400) / 3600);
    const minutes = Math.floor((seconds % 3600) / 60);

    if (days > 0) {
        return `${days}д ${hours}ч ${minutes}м`;
    }
    if (hours > 0) {
        return `${hours}ч ${minutes}м`;
    }
    return `${minutes}м`;
}

// Команды процессов вставляются через innerHTML, поэтому их обязательно экранируем
function escapeHtml(value: string): string {
    return value
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/"/g, "&quot;")
        .replace(/'/g, "&#039;");
}

// Показывает состояние связи с backend
function setConnection(online: boolean, text: string): void {
    ui.connectionDot.classList.toggle("online", online);
    ui.connectionDot.classList.toggle("offline", !online);
    ui.connectionText.textContent = text;
}

// Перерисовывает список ядер CPU. Количество ядер берется из ответа backend
function renderCores(cores: number[]): void {
    ui.cpuCoreCount.textContent = `${cores.length} ядер`;
    ui.cpuCores.innerHTML = cores
        .map((value, index) => {
            const percent = clampPercent(value);
            return `
                <div class="core-row">
                    <span>CPU ${index}</span>
                    <div class="progress">
                        <span style="width: ${percent}%; background: ${colorForLoad(percent)}"></span>
                    </div>
                    <span class="core-value">${percent.toFixed(1)}%</span>
                </div>
            `;
        })
        .join("");
}

// Перерисовывает таблицу процессов
function renderProcesses(processes: ProcessInfo[]): void {
    ui.processes.innerHTML = processes
        .map((process) => {
            const cpuClass = process.cpu >= 85 ? "danger" : process.cpu >= 65 ? "warning" : "";
            return `
                <tr>
                    <td>${process.pid}</td>
                    <td>${escapeHtml(process.user)}</td>
                    <td class="state">${escapeHtml(process.state)}</td>
                    <td class="${cpuClass}">${process.cpu.toFixed(1)}%</td>
                    <td>${formatKb(process.memoryKb)}</td>
                    <td>${process.threads}</td>
                    <td><div class="command" title="${escapeHtml(process.command)}">${escapeHtml(process.command)}</div></td>
                </tr>
            `;
        })
        .join("");
}

// Главная функция отрисовки: раскладывает JSON-снимок по карточкам и таблице
function render(snapshot: Snapshot): void {
    ui.cpuTotal.textContent = percentText(snapshot.cpu.total);
    setProgress(ui.cpuTotalBar, snapshot.cpu.total);

    ui.memoryPercent.textContent = percentText(snapshot.memory.percent);
    ui.memoryText.textContent = `${formatKb(snapshot.memory.usedKb)} / ${formatKb(snapshot.memory.totalKb)}`;
    setProgress(ui.memoryBar, snapshot.memory.percent);

    ui.swapPercent.textContent = percentText(snapshot.swap.percent);
    ui.swapText.textContent = `${formatKb(snapshot.swap.usedKb)} / ${formatKb(snapshot.swap.totalKb)}`;
    setProgress(ui.swapBar, snapshot.swap.percent);

    ui.loadAverage.textContent = snapshot.loadAverage.map((value) => value.toFixed(2)).join(" ");
    ui.processCount.textContent = `${snapshot.runningProcesses}/${snapshot.totalProcesses} процессов`;

    renderCores(snapshot.cpu.cores);

    ui.timestamp.textContent = snapshot.timestamp;
    ui.networkRx.textContent = formatRate(snapshot.network.rxRate);
    ui.networkTx.textContent = formatRate(snapshot.network.txRate);
    ui.diskRead.textContent = formatRate(snapshot.disk.readRate);
    ui.diskWrite.textContent = formatRate(snapshot.disk.writeRate);
    ui.uptime.textContent = formatDuration(snapshot.uptimeSeconds);
    ui.platform.textContent = snapshot.platform;
    renderProcesses(snapshot.processes);

    // В demo-режиме backend честно сообщает, что настоящие метрики недоступны
    setConnection(true, snapshot.error ?? "онлайн");
}

// Один цикл обновления: запросить /api/stats, проверить статус и перерисовать UI
async function refresh(): Promise<void> {
    try {
        const response = await fetch("/api/stats", { cache: "no-store" });
        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }

        const snapshot = (await response.json()) as Snapshot;
        render(snapshot);
    } catch (error) {
        setConnection(false, "нет связи с backend");
        console.error(error);
    }
}

void refresh();
window.setInterval(refresh, refreshMs);
