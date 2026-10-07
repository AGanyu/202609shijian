const moduleGrid = document.querySelector("#moduleGrid");
const logList = document.querySelector("#logList");
const connectionStatus = document.querySelector("#connectionStatus");
const commandResult = document.querySelector("#commandResult");

function escapeHtml(value) {
  return String(value ?? "").replace(/[&<>\"]/g, (char) => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;"}[char]));
}

function valueText(value, unit) {
  return value === null || value === undefined ? "--" : `${Number(value).toFixed(1)}<span class="measure-unit">${unit}</span>`;
}

function renderModules(modules) {
  if (!modules.length) {
    moduleGrid.innerHTML = '<div class="empty-state">等待传感器数据…</div>';
    return;
  }
  moduleGrid.innerHTML = modules.map((item) => `
    <article class="module-card">
      <div class="module-title"><span class="module-name">${escapeHtml(item.module)}</span><span class="module-time">${escapeHtml(item.updatedAt || "--")}</span></div>
      <div class="measurements">
        <div class="measure"><span class="measure-label">温度</span><span class="measure-value">${valueText(item.temperature, "℃")}</span></div>
        <div class="measure humidity"><span class="measure-label">湿度</span><span class="measure-value">${valueText(item.humidity, "%")}</span></div>
      </div>
    </article>`).join("");
}

function renderLogs(messages) {
  if (!messages.length) { logList.innerHTML = '<div class="empty-state">暂无消息</div>'; return; }
  logList.innerHTML = messages.slice(0, 20).map((item) => `<div class="log-item"><span class="log-time">${escapeHtml(item.time)}</span><span class="log-data">${escapeHtml(item.data)}</span></div>`).join("");
}

async function refresh() {
  try {
    const response = await fetch("/api/state", { cache: "no-store" });
    if (!response.ok) throw new Error("state request failed");
    const state = await response.json();
    connectionStatus.classList.add("online");
    connectionStatus.innerHTML = '<span class="dot"></span>后端在线';
    document.querySelector("#deviceCount").textContent = (state.devices || state.clients || []).length;
    document.querySelector("#moduleCount").textContent = state.modules.length;
    document.querySelector("#lastUpdate").textContent = state.serverTime.slice(11, 19);
    renderModules(state.modules);
    renderLogs(state.messages);
  } catch (error) {
    connectionStatus.classList.remove("online");
    connectionStatus.innerHTML = '<span class="dot"></span>后端离线';
  }
}

document.querySelector("#refreshButton").addEventListener("click", refresh);
document.querySelector("#commandForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  const input = document.querySelector("#commandInput");
  const data = input.value.trim();
  if (!data) return;
  commandResult.textContent = "发送中…";
  try {
    const response = await fetch("/api/command", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ data }) });
    const result = await response.json();
    commandResult.textContent = result.sent ? `已发送到 ${result.sent} 个 ESP32` : "当前没有在线 ESP32";
    if (result.sent) input.value = "";
  } catch (error) { commandResult.textContent = "发送失败，请检查后端。"; }
});

refresh();
setInterval(refresh, 2000);
