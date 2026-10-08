/* 家庭室内多节点烟雾报警系统 —— 网页大屏逻辑（性能优化版）
 * 数据来源: 后端 REST (/api/status, /api/series, /api/events, /api/stats) + SSE (/api/stream)
 * 控制下发: POST /api/cmd -> 后端 MQTT -> ESP32 网关 -> BLE -> 节点
 *
 * ── 这一版针对"点击切换曲线明显卡顿"的关键改动 ────────────────────────────
 *  1) SSE 的 tele 消息现在带数值：卡片和曲线直接用推送更新，不再"每来一帧就 GET 一次"。
 *     走 Cloudflare 隧道时一次请求要 1.5 秒以上，之前 3 个节点每 2 秒一帧，请求会堆成卡顿。
 *  2) 卡片 DOM 只建一次，之后按字段就地打补丁，不再 innerHTML 整块重建。
 *  3) 选择器按钮只在节点集合变化时重建；切换只改 active 类，按钮不会被点着点着换掉。
 *  4) 曲线按 (节点, 时间范围) 缓存：切回去立刻出图，再后台增量校准；空闲时预热其它节点。
 *  5) 画图用 lttb 采样 + 不做 notMerge 整图重建，切换不再"整块重画 + 重新动画"。
 *  6) 首屏 4 个接口并发拉取。
 */

const $ = (id) => document.getElementById(id);
const TOKEN_KEY = 'smoke_token';

let token = localStorage.getItem(TOKEN_KEY) || '';
let devices = [];
let activeMac = '';
let chartMac = '';
let presets = [];
let chartDpct = null;
let chartAlarm = null;
let gatewayInfo = {};
let topicBase = '';
let es = null;

/* 刷新节奏：卡片由 SSE 推送驱动，HTTP 只做"兜底校准" */
const STATUS_CALIB_MS = 15000;    // /api/status 兜底轮询
const SERIES_CALIB_MS = 30000;    // 曲线后台增量校准
const SERIES_KEEP_PTS = 2000;     // 前端最多保留的点数

/* 曲线缓存：key = "MAC|hours" -> { mac, hours, rows:[[ts,dpct,alarm]], lastTs, loaded } */
const seriesStore = new Map();
let seriesBusy = false;
let statusBusy = false;
let liveTimers = [];
let cardEls = new Map();          // mac -> 卡片里各个字段的元素引用
let pickEls = { dev: new Map(), chart: new Map() };
let pickerSig = '';

/* ---------------- 工具 ---------------- */
function hhmmss(ts) {
  const d = new Date(ts * 1000);
  const p = (n) => String(n).padStart(2, '0');
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}
function mmddhhmm(ts) {
  const d = new Date(ts * 1000);
  const p = (n) => String(n).padStart(2, '0');
  return `${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`;
}
function macShort(mac) {
  return mac ? mac.slice(-4) : '----';
}
function fmtDpct(v) {
  if (v === null || v === undefined) return '—';
  return (v > 0 ? '+' : '') + Number(v).toFixed(1) + '%';
}
function devName(d) {
  if (!d) return '?';
  const parts = [`节点 ${d.label || '?'}`];
  if (d.tag) parts.push(d.tag);
  if (d.model) parts.push(d.model);
  return parts.join(' · ');
}
function fmtRs(v) {
  if (v === null || v === undefined) return '—';
  return v >= 10000 ? (v / 1000).toFixed(1) + ' kΩ' : v + ' Ω';
}
function ago(ts) {
  if (!ts) return '';
  const s = Math.max(0, Math.floor(Date.now() / 1000) - ts);
  if (s < 5) return '刚刚';
  if (s < 60) return s + ' 秒前';
  if (s < 3600) return Math.floor(s / 60) + ' 分钟前';
  if (s < 86400) return Math.floor(s / 3600) + ' 小时前';
  return Math.floor(s / 86400) + ' 天前';
}
function hoursSel() {
  return $('chart-hours').value || '0.5';
}
function api(path, opts = {}) {
  opts.headers = Object.assign({ 'X-Auth': token }, opts.headers || {});
  return fetch(path, opts).then(async (r) => {
    const body = await r.json().catch(() => ({}));
    if (!r.ok) throw new Error(body.detail || `请求失败 (${r.status})`);
    return body;
  });
}
function flash(el) {
  if (!el) return;
  el.classList.add('flash');
  setTimeout(() => el.classList.remove('flash'), 700);
}

/* ---------------- 登录 ---------------- */
async function tryLogin(pass, silent = false) {
  token = pass;
  try {
    await api('/api/status');
    localStorage.setItem(TOKEN_KEY, token);
    $('login').classList.add('hidden');
    $('app').classList.remove('hidden');
    start();
    return true;
  } catch (e) {
    if (!silent) $('login-msg').textContent = e.message === '访问口令错误' ? '口令不正确' : e.message;
    $('login').classList.remove('hidden');
    $('app').classList.add('hidden');
    return false;
  }
}
$('login-btn').onclick = () => tryLogin($('pass').value);
$('pass').addEventListener('keydown', (e) => { if (e.key === 'Enter') tryLogin($('pass').value); });
$('logout').onclick = () => { localStorage.removeItem(TOKEN_KEY); location.reload(); };

/* ---------------- 渲染：节点卡片（建一次，之后就地打补丁） ---------------- */
const CARD_HTML = `
  <div class="card-hd">
    <span class="dot js-dot"></span>
    <div class="card-title">
      <b class="js-title"></b>
      <span class="meta js-meta"></span>
    </div>
    <span class="pill js-pill"></span>
  </div>
  <div class="mac js-mac"></div>

  <div class="delta">
    <span class="delta-v js-dpct"></span>
    <span class="delta-k">Δ% 相对基线</span>
  </div>
  <div class="bar"><span class="mid"></span><span class="fill js-fill"></span></div>

  <div class="metrics">
    <div><span>ADC</span><b class="js-adc"></b></div>
    <div><span>AO 电压</span><b class="js-ao"></b></div>
    <div><span>Rs</span><b class="js-rs"></b></div>
    <div><span>信号</span><b class="js-rssi"></b></div>
  </div>

  <div class="card-ft">
    <span class="js-seen"></span>
    <span class="js-alarminfo"></span>
  </div>`;

function ensureCard(d) {
  let c = cardEls.get(d.mac);
  if (c) return c;
  const el = document.createElement('div');
  el.id = 'card-' + d.mac;
  el.innerHTML = CARD_HTML;
  const q = (k) => el.querySelector('.js-' + k);
  c = {
    root: el, dot: q('dot'), title: q('title'), meta: q('meta'), pill: q('pill'),
    mac: q('mac'), dpct: q('dpct'), fill: q('fill'), adc: q('adc'), ao: q('ao'),
    rs: q('rs'), rssi: q('rssi'), seen: q('seen'), alarminfo: q('alarminfo'),
  };
  el.onclick = () => { activeMac = d.mac; renderPicker(); };
  cardEls.set(d.mac, c);
  $('cards').appendChild(el);
  return c;
}

function patchCard(c, d) {
  c.root.className = 'card' + (d.alarm ? ' alarm' : '') + (d.online ? '' : ' offline');

  c.dot.className = 'dot' + (d.online ? (d.alarm ? ' bad' : ' on') : '');
  c.title.textContent = `节点 ${d.label || '?'}`;
  c.meta.textContent = [d.tag, d.model].filter(Boolean).join(' · ') || '—';
  c.pill.textContent = !d.online ? '离线' : d.alarm ? '报警' : d.warmup ? '预热中' : '正常';
  c.pill.className = 'pill' + (!d.online ? ' off' : d.alarm ? ' alarm' : d.warmup ? ' warm' : '');
  c.mac.textContent = `MAC …${macShort(d.mac)}${d.fw ? ' · 固件 ' + d.fw : ''}`;

  c.dpct.textContent = fmtDpct(d.dpct);
  c.dpct.className = 'delta-v' + (d.alarm ? ' bad' : Math.abs(d.dpct || 0) >= 12 ? ' warn' : '');

  /* 进度条：以 0 为中心，±60% 打满（只改样式，不重建节点） */
  const v = Math.max(-60, Math.min(60, Number(d.dpct) || 0));
  const w = (Math.abs(v) / 60 * 50).toFixed(1);
  c.fill.style.cssText = v >= 0 ? `left:50%;width:${w}%` : `right:50%;width:${w}%`;
  c.fill.className = 'fill js-fill' + (d.alarm ? ' bad' : Math.abs(v) >= 12 ? ' warn' : '');

  c.adc.textContent = d.adc != null ? d.adc : '—';
  c.ao.textContent = d.ao_mv != null ? d.ao_mv + ' mV' : '—';
  c.rs.textContent = fmtRs(d.rs_ohm);
  c.rssi.textContent = d.rssi != null ? d.rssi + ' dBm' : '—';

  c.seen.textContent = `上报 ${d.tele_count || 0} 帧 · ${d.last_seen ? ago(d.last_seen) : '—'}`;
  c.alarminfo.textContent = d.last_alarm_ts
    ? `最近报警 ${mmddhhmm(d.last_alarm_ts)} · 累计 ${d.alarm_count || 0} 次`
    : '暂无报警记录';
  c.alarminfo.className = 'js-alarminfo' + (d.alarm ? ' bad' : '');
}

function renderDevices() {
  const keep = new Set();
  devices.forEach((d) => {
    keep.add(d.mac);
    patchCard(ensureCard(d), d);
  });
  for (const [mac, c] of cardEls) {
    if (!keep.has(mac)) { c.root.remove(); cardEls.delete(mac); }
  }
  /* 顺带把顺序对齐到 devices（后端按 label 排序） */
  const box = $('cards');
  devices.forEach((d) => {
    const c = cardEls.get(d.mac);
    if (c && c.root.parentNode === box) box.appendChild(c.root);
  });

  const anyAlarm = devices.some((d) => d.online && d.alarm);
  $('alarm-banner').classList.toggle('hidden', !anyAlarm);
  if (anyAlarm) {
    const names = devices.filter((d) => d.online && d.alarm).map(devName).join('、');
    $('alarm-text').textContent = `${names} 检测到异常，请立即查看`;
  }
}

/* 顶部指标条（只用已有数据，不额外发请求） */
function renderSummary() {
  const gw = gatewayInfo || {};
  const online = devices.filter((d) => d.online).length;
  const alarming = devices.filter((d) => d.online && d.alarm);
  const totalAlarms = devices.reduce((s, d) => s + (d.alarm_count || 0), 0);

  $('stat-online').textContent = `${online} / ${gw.nodes_max || devices.length || 0}`;
  $('stat-online').className = 'v ' + (online === 0 ? 'bad' : 'ok');

  $('stat-state').textContent = alarming.length ? `报警 ${alarming.length} 个` : (online ? '全部正常' : '无节点在线');
  $('stat-state').className = 'v ' + (alarming.length ? 'bad' : (online ? 'ok' : 'warn'));

  $('stat-ip').textContent = gw.ip || '—';
  $('stat-wifi').textContent = gw.wifi_rssi != null ? gw.wifi_rssi + ' dBm' : '—';
  $('stat-wifi').className = 'v ' + (gw.wifi_rssi != null && gw.wifi_rssi > -70 ? 'ok' : 'warn');

  $('stat-alarm').textContent = totalAlarms + ' 次';
  $('stat-alarm').className = 'v ' + (totalAlarms ? 'warn' : '');
}

/* 选择器：只在"节点集合/在线状态"变化时重建按钮，平时只切 active 类 */
function pickerSignature() {
  return devices.map((d) => d.mac + (d.online ? '1' : '0') + (d.label || '')).join(',');
}

function renderPicker(force) {
  const sig = pickerSignature();
  if (force || sig !== pickerSig) {
    pickerSig = sig;
    const build = (boxId, els, onPick) => {
      els.clear();
      const box = $(boxId);
      box.innerHTML = '';
      devices.forEach((d) => {
        const b = document.createElement('button');
        b.textContent = d.label || '?';
        b.title = devName(d);
        b.disabled = !d.online;
        b.onclick = () => onPick(d.mac);
        els.set(d.mac, b);
        box.appendChild(b);
      });
    };
    build('dev-pick', pickEls.dev, (mac) => { activeMac = mac; renderPicker(); });
    build('chart-pick', pickEls.chart, (mac) => switchChart(mac));
  }
  pickEls.dev.forEach((b, mac) => { b.className = mac === activeMac ? 'active' : ''; });
  pickEls.chart.forEach((b, mac) => { b.className = mac === chartMac ? 'active' : ''; });
}

function renderPresets() {
  const box = $('cmd-presets');
  box.innerHTML = '';
  presets.forEach((p) => {
    const b = document.createElement('button');
    b.textContent = p.label;
    b.onclick = () => sendCmd(p.text);
    box.appendChild(b);
  });
}

function renderGateway() {
  const gw = gatewayInfo || {};
  $('gw-dot').className = 'dot' + (gw.online ? ' on' : ' bad');
  $('gw-info').textContent = gw.online
    ? `网关在线 · IP ${gw.ip || '—'} · WiFi ${gw.wifi_rssi != null ? gw.wifi_rssi + ' dBm' : '—'} · 节点 ${gw.nodes || 0}/${gw.nodes_max || '?'}`
    : '网关离线（ESP32 未连接或无心跳）';
  $('footer-topic').textContent = 'MQTT ' + (topicBase || '') + '/#';
}

/* ---------------- 渲染：事件流 ---------------- */
const KIND_TEXT = {
  alarm: '报警', clear: '解除', state: '状态', cmd: '下发', ack: '回执',
};

function feedItem(ev) {
  const div = document.createElement('div');
  div.className = 'item';
  const label = ev.label || '?';
  let txt = '';
  if (ev.kind === 'alarm') txt = `节点 ${label} <b>检测到异常</b> Δ%=${fmtDpct(ev.dpct)}`;
  else if (ev.kind === 'clear') txt = `节点 ${label} 报警解除 Δ%=${fmtDpct(ev.dpct)}`;
  else if (ev.kind === 'cmd') txt = `向节点 ${label} 下发 “${ev.text}”`;
  else if (ev.kind === 'ack') txt = `节点 ${label} 回执: ${ev.text}`;
  else txt = `节点 ${label} ${ev.text === 'online' ? '上线' : '离线'}`;
  div.innerHTML = `<span class="t">${hhmmss(ev.ts)}</span>
                   <span class="tag ${ev.kind}">${KIND_TEXT[ev.kind] || ev.kind}</span>
                   <span class="txt">${txt}</span>`;
  return div;
}

function renderFeed(items) {
  const feed = $('feed');
  feed.innerHTML = '';
  items.forEach((ev) => feed.appendChild(feedItem(ev)));
}

/* ---------------- 渲染：Δ% 曲线 ---------------- */
function seriesKey(mac, hours) {
  return mac + '|' + hours;
}

function getCache(mac, hours) {
  const key = seriesKey(mac, hours);
  let c = seriesStore.get(key);
  if (!c) {
    c = { key, mac, hours, rows: [], lastTs: 0, loaded: false };
    seriesStore.set(key, c);
  }
  return c;
}

/* 基础样式只设一次：之后切换节点只更新 series 数据，不做 notMerge 整图重建 */
function chartBaseOption() {
  return {
    backgroundColor: 'transparent',
    animation: true,
    animationDuration: 250,
    animationDurationUpdate: 180,
    useDirtyRect: true,
    grid: { left: 46, right: 14, top: 28, bottom: 30 },
    tooltip: {
      trigger: 'axis',
      valueFormatter: (v) => (v === null || v === undefined ? '—' : Number(v).toFixed(1) + '%'),
    },
    legend: { top: 0, textStyle: { color: '#5f7796', fontSize: 11 } },
    xAxis: { type: 'time', axisLine: { lineStyle: { color: '#cfe0f0' } },
             axisLabel: { color: '#5f7796' },
             splitLine: { show: false } },
    yAxis: { type: 'value', name: 'Δ%', nameTextStyle: { color: '#5f7796' },
             axisLabel: { color: '#5f7796' },
             splitLine: { lineStyle: { color: '#eef3f9' } } },
    series: [
      {
        id: 'dpct',
        name: 'Δ%',
        type: 'line',
        showSymbol: false,
        smooth: true,
        sampling: 'lttb',          /* 关键：按像素抽稀，几百上千点也只在屏幕上画几百个 */
        lineStyle: { width: 2, color: '#0ea5e9' },
        areaStyle: { color: 'rgba(14,165,233,0.14)' },
        data: [],
        markLine: {
          silent: true,
          symbol: 'none',
          label: { formatter: '报警阈值 ±25%', color: '#b21232', fontSize: 10 },
          lineStyle: { color: '#e11d48', type: 'dashed' },
          data: [{ yAxis: 25 }, { yAxis: -25 }],
        },
      },
      {
        id: 'alarmpts',
        name: '报警点',
        type: 'scatter',
        symbolSize: 9,
        itemStyle: { color: '#e02442' },
        data: [],
      },
    ],
  };
}

function renderSeries() {
  if (!chartDpct) {
    chartDpct = echarts.init($('chart-dpct'));
    chartDpct.setOption(chartBaseOption(), true);
  }
  const c = getCache(chartMac, hoursSel());
  const points = [];
  const alarms = [];
  for (let i = 0; i < c.rows.length; i++) {
    const r = c.rows[i];
    const p = [r[0] * 1000, r[1]];
    points.push(p);
    if (r[2]) alarms.push(p);
  }
  const dev = devices.find((d) => d.mac === chartMac);
  /* 不用 notMerge：只换数据，坐标轴自动重算，动画也只在数据层做 */
  chartDpct.setOption({
    series: [
      { id: 'dpct', name: dev ? devName(dev) : 'Δ%', data: points },
      { id: 'alarmpts', data: alarms },
    ],
  });
  updateSeriesNote();
}

function updateSeriesNote() {
  const note = $('series-note');
  if (!note) return;
  if (!chartMac) { note.textContent = ''; return; }
  const c = getCache(chartMac, hoursSel());
  const live = c.loaded ? '' : ' · 加载中…';
  note.textContent = `● 实时推送 · ${c.rows.length} 点 · 最后 ${c.lastTs ? ago(c.lastTs) : '—'}${live}`;
}

function renderAlarmChart(stats) {
  if (!chartAlarm) chartAlarm = echarts.init($('chart-alarm'));
  const buckets = [...new Set(stats.rows.map((r) => r.bucket))].sort((a, b) => a - b);
  const labels = buckets.map((b) => {
    const d = new Date(b * 1000);
    return `${String(d.getHours()).padStart(2, '0')}:00`;
  });
  const palette = ['#0ea5e9', '#0d9f6e', '#d98207', '#db2777', '#7c3aed', '#e02442'];
  const series = stats.labels.map((lb, i) => ({
    name: '节点 ' + lb,
    type: 'bar',
    stack: 'total',
    barMaxWidth: 26,
    itemStyle: { color: palette[i % palette.length] },
    data: buckets.map((b) => {
      const hit = stats.rows.find((r) => r.bucket === b && r.label === lb);
      return hit ? hit.n : 0;
    }),
  }));
  chartAlarm.setOption({
    backgroundColor: 'transparent',
    grid: { left: 32, right: 12, top: 34, bottom: 24 },
    tooltip: { trigger: 'axis' },
    legend: { top: 0, textStyle: { color: '#5f7796', fontSize: 11 } },
    xAxis: { type: 'category', data: labels,
             axisLine: { lineStyle: { color: '#cfe0f0' } },
             axisLabel: { color: '#5f7796' } },
    yAxis: { type: 'value', minInterval: 1,
             axisLabel: { color: '#5f7796' },
             splitLine: { lineStyle: { color: '#eef3f9' } } },
    series,
  }, true);
}

/* ---------------- 数据加载 ---------------- */
async function refreshStatus() {
  if (statusBusy) return;              /* 慢网络下绝不叠加请求 */
  statusBusy = true;
  try {
    const st = await api('/api/status');
    devices = st.devices || [];
    presets = st.cmd_presets || [];
    gatewayInfo = st.gateway || {};
    topicBase = st.topic_base || '';
    if (presets.length && !$('cmd-presets').childElementCount) renderPresets();

    if (!activeMac || !devices.some((d) => d.mac === activeMac && d.online)) {
      const firstOnline = devices.find((d) => d.online) || devices[0];
      if (firstOnline) activeMac = firstOnline.mac;
    }
    if (!chartMac || !devices.some((d) => d.mac === chartMac)) {
      const firstOnline = devices.find((d) => d.online) || devices[0];
      if (firstOnline) chartMac = firstOnline.mac;
    }
    renderDevices();
    renderPicker();
    renderGateway();
    renderSummary();
  } catch (e) {
    console.warn('status', e);
  } finally {
    statusBusy = false;
  }
}

/* 把一条遥测推送应用到界面（不发任何 HTTP） */
function applyTele(m) {
  const d = devices.find((x) => x.mac === m.mac);
  if (!d) { refreshStatus(); return; }      /* 新节点：拉一次完整状态 */

  d.online = true;
  d.last_seen = m.ts || Math.floor(Date.now() / 1000);
  if (m.dpct !== undefined) d.dpct = m.dpct;
  if (m.adc !== undefined) d.adc = m.adc;
  if (m.ao !== undefined) d.ao_mv = m.ao;
  if (m.rs !== undefined) d.rs_ohm = m.rs;
  if (m.rssi !== undefined && m.rssi !== null) d.rssi = m.rssi;
  d.alarm = m.alarm ? 1 : 0;
  d.warmup = m.warmup ? 1 : 0;
  d.muted = m.muted ? 1 : 0;
  d.tele_count = (d.tele_count || 0) + 1;

  const c = cardEls.get(d.mac);
  if (c) patchCard(c, d);

  /* 曲线：当前看的节点就直接把点补进去，曲线跟着数据走，零请求 */
  if (m.mac === chartMac && m.ts) {
    const cache = getCache(chartMac, hoursSel());
    if (m.ts > cache.lastTs) {
      cache.rows.push([m.ts, m.dpct == null ? null : m.dpct, m.alarm ? 1 : 0]);
      if (cache.rows.length > SERIES_KEEP_PTS) {
        cache.rows.splice(0, cache.rows.length - SERIES_KEEP_PTS);
      }
      cache.lastTs = m.ts;
      renderSeries();
    }
  }
}

/* 取一段曲线填进缓存：只碰缓存、不碰界面（交互加载和后台预热共用） */
async function fetchInto(mac, hours, force) {
  const cache = getCache(mac, hours);
  const needFull = force === true || !cache.loaded;
  const base = `/api/series?mac=${encodeURIComponent(mac)}&hours=${hours}&fmt=2`;
  const url = needFull ? `${base}&max_points=${SERIES_KEEP_PTS}` : `${base}&since=${cache.lastTs + 1}`;
  const r = await api(url);
  const rows = r.rows || [];
  if (needFull) {
    cache.rows = rows.slice(-SERIES_KEEP_PTS);
  } else {
    for (const row of rows) {
      if (row[0] > cache.lastTs) cache.rows.push(row);
    }
    if (cache.rows.length > SERIES_KEEP_PTS) {
      cache.rows.splice(0, cache.rows.length - SERIES_KEEP_PTS);
    }
  }
  cache.loaded = true;
  cache.lastTs = cache.rows.length ? cache.rows[cache.rows.length - 1][0] : 0;
  return cache;
}

/* 当前图表的加载：force=true 整段重拉，否则按 since 取增量 */
async function loadSeries(force) {
  if (!chartMac || seriesBusy) return;
  const mac = chartMac;
  const hours = hoursSel();
  seriesBusy = true;
  try {
    await fetchInto(mac, hours, force);
    if (mac === chartMac && hours === hoursSel()) renderSeries();
    else updateSeriesNote();
  } catch (e) {
    console.warn('series', e);
  } finally {
    seriesBusy = false;
  }
}

/* 切换曲线节点：有缓存立刻出图，再后台增量校准（没缓存才整段拉） */
function switchChart(mac) {
  chartMac = mac;
  renderPicker();
  const cache = getCache(mac, hoursSel());
  if (cache.loaded) {
    renderSeries();
    loadSeries(false);
  } else {
    updateSeriesNote();
    loadSeries(true);
  }
}

/* 空闲时把其它在线节点的曲线也拉一份，之后切换就是瞬时的（串行 + 间隔，别打满隧道） */
function prefetchOtherSeries() {
  const hours = hoursSel();
  const todo = devices.filter((d) => d.online && d.mac !== chartMac)
    .map((d) => d.mac)
    .filter((mac) => !getCache(mac, hours).loaded);
  let i = 0;
  const next = () => {
    if (i >= todo.length || document.hidden) return;
    if (seriesBusy) { setTimeout(next, 600); return; }
    const mac = todo[i++];
    fetchInto(mac, hours, true)
      .then(() => setTimeout(next, 800))
      .catch(() => setTimeout(next, 2000));
  };
  setTimeout(next, 1500);
}

async function refreshEvents() {
  const r = await api('/api/events?limit=60');
  renderFeed(r.items || []);
}

async function refreshStats() {
  const r = await api('/api/stats?hours=24');
  renderAlarmChart(r);
}

/* ---------------- 实时推送 ---------------- */
function startStream() {
  if (es) es.close();
  es = new EventSource('/api/stream?token=' + encodeURIComponent(token));
  es.onmessage = (e) => {
    let msg = {};
    try { msg = JSON.parse(e.data); } catch (_) { return; }

    if (msg.type === 'tele') {
      /* 主力路径：一条遥测 = 一次纯前端更新，0 请求 */
      applyTele(msg);
      renderSummary();
    } else if (msg.type === 'alarm') {
      flash($('card-' + msg.mac));
      refreshEvents();
      refreshStats();
      loadSeries(true);          /* 报警跳变很少见，值得整段重拉 */
      refreshStatus();
    } else if (msg.type === 'state') {
      refreshStatus();
    } else if (msg.type === 'status') {
      /* 后端只是提示"状态可能有变"，交给兜底轮询即可，避免爆发式请求 */
    }
  };
  es.onerror = () => { /* EventSource 会自动重连 */ };
}

/* ---------------- 省流量：页面在后台就断开实时连接 ---------------- */
function startLive() {
  if (liveTimers.length) return;
  startStream();
  liveTimers.push(setInterval(() => refreshStatus(), STATUS_CALIB_MS));
  liveTimers.push(setInterval(() => loadSeries(false), SERIES_CALIB_MS));
}

function stopLive() {
  if (es) { es.close(); es = null; }
  liveTimers.forEach(clearInterval);
  liveTimers = [];
}

document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    stopLive();
  } else {
    refreshStatus().then(() => loadSeries(false)).catch(() => {});
    startLive();
  }
});

/* ---------------- 下发指令 ---------------- */
async function sendCmd(text) {
  const msgEl = $('cmd-msg');
  if (!activeMac) { msgEl.textContent = '请先选择一个在线节点'; return; }
  if (!text) { msgEl.textContent = '请输入要发送的内容'; return; }
  $('cmd-send').disabled = true;
  try {
    const r = await api('/api/cmd', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ mac: activeMac, text }),
    });
    msgEl.style.color = '#0d9f6e';
    msgEl.textContent = `已下发 “${r.text}”，节点执行后会在事件流里出现回执`;
    setTimeout(() => { msgEl.style.color = ''; }, 4000);
    refreshEvents();
  } catch (e) {
    msgEl.style.color = '';
    msgEl.textContent = e.message;
  } finally {
    $('cmd-send').disabled = false;
  }
}
$('cmd-send').onclick = () => sendCmd($('cmd-text').value.trim());
$('cmd-text').addEventListener('keydown', (e) => { if (e.key === 'Enter') $('cmd-send').click(); });
$('chart-hours').onchange = () => {
  /* 时间范围变了：有缓存先画出来，同时整段重拉，并重新预热其它节点 */
  const c = getCache(chartMac, hoursSel());
  if (c.loaded) renderSeries();
  loadSeries(true);
  prefetchOtherSeries();
};

/* ---------------- 启动 ---------------- */
async function start() {
  try {
    /* 首屏并发：4 个接口一起发，比串行快一倍（隧道一次往返 1.5 秒） */
    const [st] = await Promise.all([
      api('/api/status'),
      api('/api/events?limit=60').then((r) => renderFeed(r.items || [])).catch(() => {}),
      api('/api/stats?hours=24').then((r) => renderAlarmChart(r)).catch(() => {}),
    ]);
    devices = st.devices || [];
    presets = st.cmd_presets || [];
    gatewayInfo = st.gateway || {};
    topicBase = st.topic_base || '';
    const first = devices.find((d) => d.online) || devices[0];
    if (first) { activeMac = activeMac || first.mac; chartMac = chartMac || first.mac; }
    renderDevices();
    renderPicker(true);
    renderPresets();
    renderGateway();
    renderSummary();
    await loadSeries(true);
  } catch (e) {
    console.warn(e);
  }
  startLive();
  prefetchOtherSeries();
  let rt = null;
  window.addEventListener('resize', () => {
    clearTimeout(rt);
    rt = setTimeout(() => {
      chartDpct && chartDpct.resize();
      chartAlarm && chartAlarm.resize();
    }, 150);
  });
}

if (token) tryLogin(token, true);
