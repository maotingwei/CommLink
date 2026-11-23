/* CommLink 前端逻辑: WebSocket 接收 + 历史/搜索/过滤/注入 */
(function () {
  "use strict";

  const $ = (id) => document.getElementById(id);
  let history = [];
  let searchTerm = "";
  let ws = null;

  // ---------- WebSocket ----------
  function connectWS() {
    const proto = location.protocol === "https:" ? "wss:" : "ws:";
    const url = proto + "//" + location.host + "/ws";
    ws = new WebSocket(url);
    ws.onopen = () => {
      $("dot-ws").className = "dot online";
      $("st-ws").textContent = "已连接";
    };
    ws.onclose = () => {
      $("dot-ws").className = "dot offline";
      $("st-ws").textContent = "断开重连中";
      setTimeout(connectWS, 1500);
    };
    ws.onmessage = (e) => {
      const msg = JSON.parse(e.data);
      if (msg.type === "data") {
        history.push(msg.data);
        if (history.length > 1000) history.shift();
        renderIncremental(msg.data);
        updateCounts();
      }
    };
  }

  // ---------- 渲染 ----------
  function esc(s) {
    return String(s).replace(/[&<>]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" }[c]));
  }
  function highlight(text, term) {
    if (!term) return esc(text);
    const lower = text.toLowerCase();
    const tl = term.toLowerCase();
    let out = "", i = 0;
    while (true) {
      const p = lower.indexOf(tl, i);
      if (p < 0) { out += esc(text.slice(i)); break; }
      out += esc(text.slice(i, p)) + "<mark>" + esc(text.slice(p, p + term.length)) + "</mark>";
      i = p + term.length;
    }
    return out;
  }
  function match(item, term) {
    if (!term) return true;
    const t = term.toLowerCase();
    return (item.hex || "").toLowerCase().includes(t) ||
           (item.ascii || "").toLowerCase().includes(t) ||
           (item.dir || "").toLowerCase().includes(t) ||
           (item.time || "").includes(t);
  }
  function dirVisible(d) {
    return (d === "TX" && $("show-tx").checked) || (d === "RX" && $("show-rx").checked);
  }
  function lineHTML(item) {
    return '<div class="line ' + item.dir + '">' +
      '<span class="t">' + esc(item.time) + '</span>' +
      '<span class="dir">' + item.dir + '</span>' +
      '<span class="len">' + item.len + 'B</span>' +
      '<span class="hex">' + highlight(item.hex, searchTerm) + '</span>' +
      '<span class="asc">' + highlight(item.ascii, searchTerm) + '</span>' +
      '</div>';
  }
  function renderIncremental(item) {
    const stream = $("stream");
    if ($("search").value.trim()) return; // 搜索态下不做增量渲染
    if (!dirVisible(item.dir)) return;
    const empty = stream.querySelector(".empty");
    if (empty) empty.remove();
    stream.insertAdjacentHTML("beforeend", lineHTML(item));
    if ($("auto-scroll").checked) stream.scrollTop = stream.scrollHeight;
  }
  function renderAll() {
    const stream = $("stream");
    const shown = history.filter((it) => dirVisible(it.dir) && match(it, searchTerm));
    if (!shown.length) {
      stream.innerHTML = '<div class="empty">暂无数据' + (searchTerm ? " 匹配 \"" + esc(searchTerm) + "\"" : "") + "</div>";
    } else {
      stream.innerHTML = shown.map(lineHTML).join("");
      if ($("auto-scroll").checked) stream.scrollTop = stream.scrollHeight;
    }
    $("search-info").textContent = searchTerm
      ? "搜索 \"" + searchTerm + "\" — 命中 " + shown.length + " 条"
      : "显示全部记录";
    $("total-count").textContent = history.length;
  }
  function updateCounts() {
    $("total-count").textContent = history.length;
  }

  // ---------- API ----------
  async function api(path, options) {
    const res = await fetch(path, options);
    return res.json();
  }
  async function postJSON(path, body) {
    return api(path, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body)
    });
  }

  async function loadConfig() {
    const r = await api("/api/config");
    const c = r.data;
    $("board-enabled").checked = !!c.board.enabled;
    $("board-host").value = c.board.host;
    $("board-port").value = c.board.port;
    $("filter-enabled").checked = !!c.filter.enabled;
    $("filter-mode").value = c.filter.mode;
    $("dir-tx").checked = c.filter.apply_to.includes("TX");
    $("dir-rx").checked = c.filter.apply_to.includes("RX");
    $("filter-patterns").value = (c.filter.patterns || []).join("\n");
  }

  async function loadHistory() {
    const r = await api("/api/history");
    history = r.data || [];
    renderAll();
  }

  async function pollStatus() {
    try {
      const r = await api("/api/status");
      const d = r.data;
      $("dot-driver").className = "dot " + (d.driver_ok ? "online" : "offline");
      $("st-driver").textContent = d.driver_ok ? "已连接" : "未连接";
      $("st-tx").textContent = d.stats.tx_frames;
      $("st-rx").textContent = d.stats.rx_frames;
      $("st-filtered").textContent = d.stats.filtered;
    } catch (e) { /* ignore */ }
  }

  // ---------- 事件绑定 ----------
  $("btn-save-board").onclick = async () => {
    const cfg = (await api("/api/config")).data;
    cfg.board.enabled = $("board-enabled").checked;
    cfg.board.host = $("board-host").value.trim();
    cfg.board.port = parseInt($("board-port").value, 10);
    await postJSON("/api/config", cfg);
    alert("板子配置已保存(重启服务后 TCP 连接生效)");
  };

  $("btn-save-filter").onclick = async () => {
    const apply_to = [];
    if ($("dir-tx").checked) apply_to.push("TX");
    if ($("dir-rx").checked) apply_to.push("RX");
    const payload = {
      filter: {
        enabled: $("filter-enabled").checked,
        mode: $("filter-mode").value,
        patterns: $("filter-patterns").value.split("\n").map(s => s.trim()).filter(Boolean),
        apply_to
      }
    };
    await postJSON("/api/filter", payload);
    alert("过滤规则已保存并生效");
  };

  $("btn-inject").onclick = async () => {
    const r = await postJSON("/api/inject_rx", {
      data: $("inject-text").value,
      hex: $("inject-hex").checked
    });
    if (r.ok) {
      $("inject-text").value = "";
    } else {
      alert("注入失败: " + r.error);
    }
  };

  $("btn-clear-history").onclick = async () => {
    await postJSON("/api/clear", {});
    history = [];
    renderAll();
  };

  $("btn-purge").onclick = async () => {
    const r = await postJSON("/api/purge", {});
    alert(r.ok ? "驱动缓冲已清空" : "清空失败: " + r.error);
  };

  $("btn-search").onclick = () => { searchTerm = $("search").value.trim(); renderAll(); };
  $("search").addEventListener("keydown", (e) => {
    if (e.key === "Enter") { searchTerm = $("search").value.trim(); renderAll(); }
  });
  ["show-tx", "show-rx", "auto-scroll"].forEach((id) => {
    $(id).addEventListener("change", () => renderAll());
  });

  // ---------- 启动 ----------
  connectWS();
  loadConfig();
  loadHistory();
  pollStatus();
  setInterval(pollStatus, 2000);
})();
