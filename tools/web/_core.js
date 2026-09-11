/* ============================================================
   DRAGONGL · DATA STUDIO — core CRUD engine
   Shared by bestiary.html / items.html / spells.html
   Expects window.DGL_CFG to be defined by the module script.
   ============================================================ */
'use strict';

/* ---------- tiny helpers (shared with modules) ---------- */
const DGL = (() => {
  const esc = s => String(s ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const keepBr = s => String(s ?? '').replace(/<(?!\/?br\s*\/?>)/gi, '&lt;'); // keep only <br>
  const num = v => { const n = Number(v); return Number.isFinite(n) ? n : 0; };
  const fmtN = n => num(n).toLocaleString('it-IT');
  const badge = (l, c) => `<span class="badge" style="color:${c};background:${c}1c;border-color:${c}59">${esc(l)}</span>`;
  const chip = (l, c) => `<span class="chip" style="color:${c};background:${c}16;border-color:${c}4d">${esc(l)}</span>`;
  const kv = (l, v, acc=false) => `<div class="kv"><div class="k">${l}</div><div class="v${acc?' acc':''}">${v}</div></div>`;
  const dash = '<span class="dimtxt">—</span>';
  const splitList = v => String(v ?? '').split(',').map(s => s.trim()).filter(Boolean);
  const I = {
    eye:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M2 12s3-7 10-7 10 7 10 7-3 7-10 7-10-7-10-7Z"/><circle cx="12" cy="12" r="3"/></svg>',
    pen:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M17 3a2.85 2.83 0 1 1 4 4L7.5 20.5 2 22l1.5-5.5Z"/></svg>',
    copy:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect width="14" height="14" x="8" y="8" rx="2"/><path d="M4 16c-1.1 0-2-.9-2-2V4c0-1.1.9-2 2-2h10c1.1 0 2 .9 2 2"/></svg>',
    trash:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 6h18"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/><path d="M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>',
    x:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M18 6 6 18"/><path d="m6 6 12 12"/></svg>',
    save:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"/><polyline points="17 21 17 13 7 13 7 21"/><polyline points="7 3 7 8 15 8"/></svg>',
    download:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" x2="12" y1="15" y2="3"/></svg>',
    refresh:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8"/><path d="M3 3v5h5"/></svg>',
    folder:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m6 14 1.5-2.9A2 2 0 0 1 9.24 10H20a2 2 0 0 1 1.94 2.5l-1.54 6a2 2 0 0 1-1.95 1.5H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h3.9a2 2 0 0 1 1.69.9l.81 1.2a2 2 0 0 0 1.67.9H18a2 2 0 0 1 2 2v2"/></svg>',
    file:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M15 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V7Z"/><path d="M14 2v4a2 2 0 0 0 2 2h4"/></svg>',
    plus:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12h14"/><path d="M12 5v14"/></svg>',
    alert:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m21.73 18-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3Z"/><line x1="12" x2="12" y1="9" y2="13"/><line x1="12" x2="12.01" y1="17" y2="17"/></svg>',
    star:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polygon points="12 2 15.09 8.26 22 9.27 17 14.14 18.18 21.02 12 17.77 5.82 21.02 7 14.14 2 9.27 8.91 8.26 12 2"/></svg>',
    layers:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m12.83 2.18a2 2 0 0 0-1.66 0L2.6 6.08a1 1 0 0 0 0 1.83l8.58 3.91a2 2 0 0 0 1.66 0l8.58-3.9a1 1 0 0 0 0-1.83Z"/><path d="m22 17.65-9.17 4.16a2 2 0 0 1-1.66 0L2 17.65"/><path d="m22 12.65-9.17 4.16a2 2 0 0 1-1.66 0L2 12.65"/></svg>',
    search:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="11" cy="11" r="8"/><path d="m21 21-4.3-4.3"/></svg>',
    inbox:'<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="22 12 16 12 14 15 10 15 8 12 2 12"/><path d="M5.45 5.11 2 12v6a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2v-6l-3.45-6.89A2 2 0 0 0 16.76 4H7.24a2 2 0 0 0-1.79 1.11z"/></svg>',
  };
  return { esc, keepBr, num, fmtN, badge, chip, kv, dash, splitList, I };
})();
const { esc, keepBr, num, fmtN, badge, chip, kv, dash, splitList, I } = DGL;

/* ---------- state ---------- */
const CFG = window.DGL_CFG;
const $ = s => document.querySelector(s);
const $$ = s => [...document.querySelectorAll(s)];

const S = {
  raw: null, rows: [], snap: null, view: [],
  page: 1, pageSize: 50,
  sort: { id: 'name', dir: 1 },
  q: '', filters: {},
  handle: null, fileName: '',
  dirty: false, loaded: false,
  editRef: null,
};
try { S.pageSize = +localStorage.getItem('dgl.psize') || 50; } catch (e) {}

/* ---------- toasts ---------- */
function toast(msg, kind = 'info', ms = 3800) {
  const t = document.createElement('div');
  t.className = 'toast t-' + kind;
  t.innerHTML = msg;
  $('#toasts').appendChild(t);
  setTimeout(() => { t.classList.add('out'); setTimeout(() => t.remove(), 320); }, ms);
}

/* ---------- data loading ---------- */
async function autoLoad() {
  try {
    const r = await fetch(CFG.file, { cache: 'no-store' });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const obj = await r.json();
    ingest(obj, CFG.file.split('/').pop(), null);
  } catch (e) {
    showLoadPanel();
  }
}

function ingest(obj, name, handle) {
  if (!obj || !Array.isArray(obj[CFG.key])) {
    toast(`<b>Invalid file:</b> missing array <code>${esc(CFG.key)}</code> at top level.`, 'err', 6000);
    throw new Error('invalid json structure');
  }
  S.raw = obj;
  S.rows = obj[CFG.key];
  S.fileName = name || CFG.file.split('/').pop();
  S.handle = handle || null;
  S.snap = structuredClone(S.rows);
  S.loaded = true; S.dirty = false;
  S.q = ''; S.filters = {}; S.page = 1;
  const q = $('#q'); if (q) q.value = '';
  hideLoadPanel();
  buildToolbar();
  renderAll();
  updateSaveUI();
  toast(`<b>${fmtN(S.rows.length)} ${esc(CFG.plural)}</b> loaded from <b>${esc(S.fileName)}</b>${S.handle ? ' — direct write enabled' : ''}`, 'ok');
}

function showLoadPanel() {
  if (!S.loaded) $('#lpMsg').innerHTML =
    `Automatic loading of <code>${esc(CFG.file)}</code> is not possible in this context (browsers block local file reads when the page is opened via <b>file://</b>).`;
  else $('#lpMsg').innerHTML = `Select a new <b>${esc(S.fileName || CFG.file.split('/').pop())}</b> to replace the current data set.`;
  $('#lpPicker').hidden = !window.showOpenFilePicker;
  $('#loadPanel').hidden = false;
}
function hideLoadPanel() { $('#loadPanel').hidden = true; }

async function readFile(file) {
  if (!file) return;
  try {
    const obj = JSON.parse(await file.text());
    ingest(obj, file.name, null);
  } catch (e) {
    toast(`<b>Cannot read “${esc(file.name)}”:</b> ${esc(e.message)}`, 'err', 6000);
  }
}

async function openWithPicker() {
  try {
    const [h] = await window.showOpenFilePicker({
      types: [{ description: 'JSON data file', accept: { 'application/json': ['.json'] } }],
    });
    const f = await h.getFile();
    ingest(JSON.parse(await f.text()), f.name, h);
  } catch (e) { /* user cancelled */ }
}

/* ---------- saving ---------- */
function serial() { return JSON.stringify(S.raw, null, 2) + '\n'; }

function downloadText(text, name) {
  const b = new Blob([text], { type: 'application/json' });
  const u = URL.createObjectURL(b);
  const a = document.createElement('a');
  a.href = u; a.download = name; a.click();
  setTimeout(() => URL.revokeObjectURL(u), 5000);
}

async function doSave() {
  if (!S.loaded) return;
  if (!S.dirty) { toast('No pending changes to save.', 'info'); return; }
  const text = serial();
  if (S.handle && S.handle.createWritable) {
    try {
      const w = await S.handle.createWritable();
      await w.write(text); await w.close();
      S.snap = structuredClone(S.rows); S.dirty = false;
      renderAll(); updateSaveUI();
      toast(`<b>Saved</b> to <b>${esc(S.handle.name)}</b> on disk.`, 'ok');
    } catch (e) {
      toast(`<b>Write failed:</b> ${esc(e.message)}`, 'err', 6000);
    }
  } else {
    downloadText(text, S.fileName);
    S.snap = structuredClone(S.rows); S.dirty = false;
    renderAll(); updateSaveUI();
    toast(`<b>Exported ${esc(S.fileName)}</b> — replace the file in the <code>data/</code> folder to commit the changes.`, 'ok', 6500);
  }
}

function doReload() {
  if (!S.loaded) return;
  if (S.dirty && !confirm('Discard unsaved changes and restore the last saved state?')) return;
  S.raw[CFG.key] = structuredClone(S.snap);
  S.rows = S.raw[CFG.key];
  S.dirty = false; S.page = 1;
  renderAll(); updateSaveUI();
  toast('Restored last saved state.', 'warn');
}

function touch() { S.dirty = true; updateSaveUI(); }

function updateSaveUI() {
  const st = $('#statusChip');
  if (!S.loaded) { st.className = 'status st-bad'; st.innerHTML = `<span class="dot"></span>No data`; }
  else if (S.dirty) { st.className = 'status st-warn'; st.innerHTML = `<span class="dot"></span>Unsaved changes`; }
  else { st.className = 'status st-ok'; st.innerHTML = `<span class="dot"></span>In sync`; }
  $('#btnSave').innerHTML = (S.handle ? I.save : I.download) + (S.handle ? ' Save to file' : ' Export JSON');
  $('#btnNew').disabled = !S.loaded;
}

/* ---------- filtering / sorting ---------- */
function activeFilterCount() {
  return CFG.filters.filter(f => {
    const v = S.filters[f.id];
    return v !== undefined && v !== null && v !== '' && v !== false;
  }).length;
}

function applyFilters() {
  const q = S.q.toLowerCase();
  const [sf, sf2] = CFG.searchFields;
  S.view = S.rows.filter(m => {
    if (q && !(`${m[sf] ?? ''} ${m[sf2] ?? ''} ${m.name ?? ''}`.toLowerCase().includes(q))) return false;
    return CFG.filters.every(f => f.apply(m, S.filters[f.id] ?? null));
  });
  const c = CFG.columns.find(c => c.id === S.sort.id);
  if (c) {
    const sv = x => (c.sortVal ? c.sortVal(x) : (x[c.id] ?? ''));
    S.view = [...S.view].sort((a, b) => {
      const va = sv(a), vb = sv(b);
      const r = (va < vb) ? -1 : (va > vb ? 1 : 0);
      return r * S.sort.dir || (a.name < b.name ? -1 : 1);
    });
  }
  const pages = Math.max(1, Math.ceil(S.view.length / S.pageSize));
  S.page = Math.min(S.page, pages);
}

function setFilter(id, value) {
  S.filters[id] = value;
  S.page = 1;
  renderAll();
}

function clearFilters() {
  S.q = ''; S.filters = {}; S.page = 1;
  const q = $('#q'); if (q) q.value = '';
  $$('#filterBox select').forEach(s => s.value = '');
  $$('#filterBox input[type=checkbox]').forEach(c => c.checked = false);
  renderAll();
}

/* ---------- rendering ---------- */
function renderAll() {
  applyFilters();
  renderStats();
  renderDist();
  renderTable();
  renderPager();
  $('#countLbl').innerHTML = S.loaded
    ? `<b>${fmtN(S.view.length)}</b> / ${fmtN(S.rows.length)} shown`
    : '';
  $('#btnClear').hidden = !(S.q || activeFilterCount());
  const active = activeFilterCount();
  if (CFG.distFilter) {
    $$('#distLegend .leg').forEach(el => {
      el.classList.toggle('active', el.dataset.active === '1');
    });
  }
  document.body.classList.toggle('has-data', S.loaded);
}

function renderStats() {
  const row = $('#statsRow');
  if (!S.loaded) { row.hidden = true; row.innerHTML = ''; return; }
  row.hidden = false;
  const palette = ['var(--accent)', '#38bdf8', '#34d399', '#fbbf24', '#a78bfa'];
  row.innerHTML = CFG.stats(S.rows).map((s, i) => `
    <div class="stat" style="--sc:${palette[i % palette.length]}">
      <div class="s-label">${esc(s.label)}</div>
      <div class="s-value">${s.value}</div>
      <div class="s-sub">${esc(s.sub || '')}</div>
    </div>`).join('');
}

function renderDist() {
  const el = $('#distPanel');
  if (!S.loaded) { el.hidden = true; return; }
  el.hidden = false;
  const d = CFG.dist(S.rows);
  const total = d.reduce((a, x) => a + x.count, 0) || 1;
  el.innerHTML = `
    <div class="dist-head">${I.layers}<span class="dist-title">${esc(CFG.distTitle)}</span>
      <span class="dist-hint">click a segment legend to filter</span></div>
    <div class="dist-bar">${d.map(x =>
      `<span style="width:${(100 * x.count / total).toFixed(2)}%;background:${x.color}" title="${esc(x.label)} — ${fmtN(x.count)} (${(100 * x.count / total).toFixed(1)}%)"></span>`).join('')}</div>
    <div class="dist-legend" id="distLegend">${d.map(x => {
      const f = CFG.distFilter ? CFG.distFilter(x) : null;
      const isCur = f && S.filters[f.id] === f.value;
      return `<button class="leg" style="--lc:${x.color}" data-id="${f ? f.id : ''}" data-val="${esc(f ? f.value : '')}" data-active="${isCur ? 1 : 0}"><i></i>${esc(x.label)} <b>${fmtN(x.count)}</b></button>`;
    }).join('')}</div>`;
}

function renderTable() {
  if (!S.loaded) {
    $('#thead').innerHTML = '';
    $('#tbody').innerHTML = '';
    return;
  }
  $('#thead').innerHTML = '<tr>' + CFG.columns.map(c => `
    <th data-col="${c.id}" class="${c.cls || ''} ${S.sort.id === c.id ? 'sorted' : ''}">
      ${esc(c.label)}<span class="arr">${S.sort.id === c.id ? (S.sort.dir > 0 ? '▲' : '▼') : ''}</span></th>`).join('') +
    '<th class="no-sort acts-h">Actions</th></tr>';

  const start = (S.page - 1) * S.pageSize;
  const slice = S.view.slice(start, start + S.pageSize);
  if (!slice.length) {
    $('#tbody').innerHTML = `<tr><td colspan="${CFG.columns.length + 1}"><div class="empty">${I.inbox}<h3>No entries match</h3><p>Adjust the search or clear the active filters.</p></div></td></tr>`;
    return;
  }
  $('#tbody').innerHTML = slice.map((m, i) =>
    `<tr data-i="${start + i}">` +
    CFG.columns.map(c => `<td class="${c.cls || ''}">${c.render(m)}</td>`).join('') +
    `<td class="acts">
      <button class="act" data-act="view" title="View details">${I.eye}</button>
      <button class="act a-edit" data-act="edit" title="Edit">${I.pen}</button>
      <button class="act" data-act="dup" title="Duplicate">${I.copy}</button>
      <button class="act a-del" data-act="del" title="Delete">${I.trash}</button>
    </td></tr>`).join('');
}

function renderPager() {
  if (!S.loaded) { $('#pager').innerHTML = ''; return; }
  const total = S.view.length;
  const pages = Math.max(1, Math.ceil(total / S.pageSize));
  const start = total ? (S.page - 1) * S.pageSize + 1 : 0;
  const end = Math.min(total, S.page * S.pageSize);
  $('#pager').innerHTML = `
    <div class="pg-info">Showing <b>${fmtN(start)}–${fmtN(end)}</b> of <b>${fmtN(total)}</b> entries</div>
    <div class="pg-ctrl">
      <label class="psize">Rows
        <select class="fsel" id="psize">${[25, 50, 100, 200].map(n =>
          `<option value="${n}" ${n === S.pageSize ? 'selected' : ''}>${n}</option>`).join('')}</select>
      </label>
      <button class="btn btn-ghost btn-sm" id="pgPrev" ${S.page <= 1 ? 'disabled' : ''}>‹ Prev</button>
      <span class="pg-num">${S.page} / ${pages}</span>
      <button class="btn btn-ghost btn-sm" id="pgNext" ${S.page >= pages ? 'disabled' : ''}>Next ›</button>
    </div>`;
  $('#psize').onchange = e => {
    S.pageSize = +e.target.value; S.page = 1;
    try { localStorage.setItem('dgl.psize', S.pageSize); } catch (err) {}
    renderAll();
  };
  $('#pgPrev').onclick = () => { if (S.page > 1) { S.page--; renderAll(); } };
  $('#pgNext').onclick = () => { if (S.page < pages) { S.page++; renderAll(); } };
}

/* ---------- toolbar ---------- */
function buildToolbar() {
  const box = $('#filterBox');
  box.innerHTML = '';
  for (const f of CFG.filters) {
    if (f.type === 'check') {
      const el = document.createElement('label');
      el.className = 'fwrap';
          const cur = S.filters[f.id];
      el.innerHTML = `<span class="flabel-inline">${esc(f.label)}</span>
        <label class="switchline"><input type="checkbox" id="f_${f.id}" ${cur ? 'checked' : ''}><span class="switch"></span><span class="swtxt">${cur ? 'On' : 'Off'}</span></label>`;
      const inp = el.querySelector('input');
      inp.addEventListener('change', () => {
        S.filters[f.id] = inp.checked;
        el.querySelector('.swtxt').textContent = inp.checked ? 'On' : 'Off';
        S.page = 1; renderAll();
      });
      box.appendChild(el);
    } else {
      const opts = (typeof f.options === 'function' ? f.options(S.rows) : f.options) || [];
      const sel = document.createElement('select');
      sel.className = 'fsel'; sel.id = 'f_' + f.id;
      sel.innerHTML = `<option value="">${esc(f.label)}: all</option>` +
        opts.map(o => `<option value="${esc(o[0])}" ${S.filters[f.id] === o[0] ? 'selected' : ''}>${esc(o[1])}</option>`).join('');
      sel.addEventListener('change', () => setFilter(f.id, sel.value));
      box.appendChild(sel);
    }
  }
}

/* ---------- modal ---------- */
function openModal(title, bodyHtml, footerHtml, opts = {}) {
  closeModal(true);
  const ov = document.createElement('div');
  ov.className = 'overlay';
  ov.innerHTML = `
    <div class="modal ${opts.wide ? 'modal-wide' : ''}" role="dialog" aria-modal="true">
      <div class="modal-h"><h3>${title}</h3><button class="act" id="mClose" title="Close">${I.x}</button></div>
      <div class="modal-b">${bodyHtml}</div>
      ${footerHtml ? `<div class="modal-f">${footerHtml}</div>` : ''}
    </div>`;
  $('#modalRoot').appendChild(ov);
  document.body.style.overflow = 'hidden';
  ov.addEventListener('mousedown', e => { if (e.target === ov) closeModal(); });
  $('#mClose').onclick = () => closeModal();
  if (opts.onOpen) opts.onOpen(ov);
  return ov;
}
function closeModal(silent) {
  const m = $('#modalRoot .overlay');
  if (m) m.remove();
  if (!silent || !$('#modalRoot .overlay')) document.body.style.overflow = '';
}

/* ---------- detail ---------- */
function showDetail(m) {
  const ov = openModal(esc(m.name), CFG.detail(m), `
    <button class="btn" id="dDup">${I.copy} Duplicate</button>
    <button class="btn" id="dDel">${I.trash} Delete</button>
    <button class="btn btn-primary" id="dEdit">${I.pen} Edit</button>`,
    { wide: true, onOpen() {
      $('#dEdit').onclick = () => showForm(m);
      $('#dDup').onclick = () => duplicate(m);
      $('#dDel').onclick = () => confirmDelete(m);
    }});
}

/* ---------- form ---------- */
function fieldHtml(f, target) {
  const cur = target ? target[f.id] : undefined;
  const full = f.full ? 'full' : '';
  const label = `<label class="flabel" for="fld_${f.id}">${esc(f.label)}${f.req ? ' <span class="req">*</span>' : ''}</label>`;
  const hint = f.hint ? `<div class="fhint">${esc(f.hint)}</div>` : '';
  switch (f.type) {
    case 'text':
    case 'textarea': {
      const tag = f.type === 'textarea' ? 'textarea' : 'input';
      const val = esc(cur ?? '');
      const dl = f.datalist
        ? `<datalist id="dl_${f.id}">${f.datalist.map(d => `<option value="${esc(d)}">`).join('')}</datalist>`
        : '';
      const control = tag === 'input'
        ? `<input id="fld_${f.id}" data-fld="${f.id}" type="text" ${f.datalist ? `list="dl_${f.id}"` : ''} ${f.req ? 'required' : ''} value="${val}">`
        : `<textarea id="fld_${f.id}" data-fld="${f.id}" ${f.req ? 'required' : ''}>${val}</textarea>`;
      return `<div class="fg ${full}">${label}${control}${dl}${hint}</div>`;
    }
    case 'number':
      return `<div class="fg ${full}">${label}<input id="fld_${f.id}" data-fld="${f.id}" type="number" step="any" value="${esc(cur ?? '')}">${hint}</div>`;
    case 'select': {
      const opts = (typeof f.options === 'function' ? f.options(S.rows) : f.options) || [];
      let h = `<option value="">${esc(f.emptyLabel || '—')}&nbsp;</option>`;
      h += opts.map(o => `<option value="${esc(o[0])}" ${String(cur ?? '') === String(o[0]) ? 'selected' : ''}>${esc(o[1])}</option>`).join('');
      return `<div class="fg ${full}">${label}<select id="fld_${f.id}" data-fld="${f.id}">${h}</select>${hint}</div>`;
    }
    case 'list':
      return `<div class="fg ${full}">${label}
        <input id="fld_${f.id}" data-fld="${f.id}" type="text" list="dl_${f.id}" value="${esc(cur ?? '')}" placeholder="${esc(f.placeholder || '')}">
        <datalist id="dl_${f.id}">${(f.datalist || []).map(d => `<option value="${esc(d)}">`).join('')}</datalist>${hint}</div>`;
    case 'toggle': {
      const on = !!cur;
      return `<div class="fg ${full} toggle-row">${label}
        <label class="switchline"><input type="checkbox" data-fld="${f.id}" ${on ? 'checked' : ''}><span class="switch"></span><span class="swtxt">${on ? 'Yes' : 'No'}</span></label>${hint}</div>`;
    }
    case 'checks': {
      const sel = Array.isArray(cur) ? cur : splitList(cur ?? '');
      return `<div class="fg full">${label}<div class="cbgrid">${(f.options || []).map(o =>
        `<label class="cb"><input type="checkbox" data-fld="${f.id}" value="${esc(o)}" ${sel.includes(o) ? 'checked' : ''}><span>${esc(o)}</span></label>`).join('')}</div>${hint}</div>`;
    }
    case 'vfx': {
      const v = (cur && typeof cur === 'object') ? cur : { type: 0, r: 0.8, g: 0.8, b: 0.8 };
      const sl = (id, lab, val, color) => `
        <div class="vfx-slider fg">
          <label class="flabel" style="color:${color}">${lab} <output id="o_${id}">${(+val).toFixed(2)}</output></label>
          <input type="range" data-fld="${id}" min="0" max="1" step="0.01" value="${val}">
        </div>`;
      return `<div class="fg full"><div class="fgroup"><h5>${I.star} VFX profile</h5>
        <div class="vfx-row">
          <div class="vfx-orbwrap"><div class="orb-lg" id="vfxOrb"></div><div class="fhint" id="vfxRgb"></div></div>
          <div class="fg"><label class="flabel">Effect type</label>
            <select data-fld="vfx_type">${[0, 1, 2].map(t => `<option value="${t}" ${+v.type === t ? 'selected' : ''}>${t}</option>`).join('')}</select></div>
          ${sl('vfx_r', 'Red', v.r, '#f87171')}${sl('vfx_g', 'Green', v.g, '#4ade80')}${sl('vfx_b', 'Blue', v.b, '#60a5fa')}
        </div></div></div>`;
    }
    default:
      return `<div class="fg ${full}">${label}<input id="fld_${f.id}" data-fld="${f.id}" type="text" value="${esc(cur ?? '')}"></div>`;
  }
}

function bindVfxLive(ov) {
  const orb = ov.querySelector('#vfxOrb');
  if (!orb) return;
  const upd = () => {
    const r = ov.querySelector('[data-fld=vfx_r]').value;
    const g = ov.querySelector('[data-fld=vfx_g]').value;
    const b = ov.querySelector('[data-fld=vfx_b]').value;
    const rr = (+r * 255) | 0, gg = (+g * 255) | 0, bb = (+b * 255) | 0;
    ov.querySelector('#o_vfx_r').textContent = (+r).toFixed(2);
    ov.querySelector('#o_vfx_g').textContent = (+g).toFixed(2);
    ov.querySelector('#o_vfx_b').textContent = (+b).toFixed(2);
    ov.querySelector('#vfxRgb').textContent = `rgb(${rr},${gg},${bb})`;
    orb.style.background = `rgb(${rr},${gg},${bb})`;
    orb.style.boxShadow = `0 0 18px 4px rgba(${rr},${gg},${bb},.55), inset 0 0 8px rgba(255,255,255,.35)`;
  };
  ['vfx_r', 'vfx_g', 'vfx_b'].forEach(id => ov.querySelector(`[data-fld=${id}]`).addEventListener('input', upd));
  upd();
}

function collectForm(root) {
  const d = {};
  for (const f of CFG.formFields) {
    if (f.type === 'checks') {
      d[f.id] = [...root.querySelectorAll(`[data-fld="${f.id}"]:checked`)].map(i => i.value);
    } else if (f.type === 'vfx') {
      const q = s => +root.querySelector(`[data-fld="${s}"]`).value;
      d[f.id] = { type: q('vfx_type'), r: q('vfx_r'), g: q('vfx_g'), b: q('vfx_b') };
    } else if (f.type === 'toggle') {
      d[f.id] = !!root.querySelector(`[data-fld="${f.id}"]`)?.checked;
    } else {
      const el = root.querySelector(`[data-fld="${f.id}"]`);
      d[f.id] = el ? el.value.trim() : '';
    }
  }
  return d;
}

function showForm(target) {
  S.editRef = target;
  const body = `<form id="dglForm" class="fgrid" novalidate>${CFG.formFields.map(f => fieldHtml(f, target)).join('')}</form>`;
  const footer = `<button type="button" class="btn" id="fCancel">${I.x} Cancel</button>
    <button type="submit" form="dglForm" class="btn btn-primary">${target ? I.save + ' Save changes' : I.plus + ' Create ' + esc(CFG.singular)}</button>`;
  openModal(target ? `Edit — ${esc(target.name)}` : `New ${esc(CFG.singular)}`, body, footer, {
    wide: true,
    onOpen(ov) {
      $('#fCancel').onclick = () => closeModal();
      ov.querySelector('#dglForm').addEventListener('submit', e => { e.preventDefault(); submitForm(ov); });
      bindVfxLive(ov);
      const fi = ov.querySelector('input[type=text],input:not([type]),select,textarea');
      if (fi) fi.focus();
    },
  });
}

function submitForm(ov) {
  const d = collectForm(ov);
  for (const f of CFG.formFields) {
    if (!f.req) continue;
    const v = Array.isArray(d[f.id]) ? d[f.id].length : String(d[f.id] ?? '').trim();
    if (!v) { toast(`“<b>${esc(f.label)}</b>” is required.`, 'err'); ov.querySelector(`[data-fld="${f.id}"]`)?.focus(); return; }
  }
  const target = S.editRef;
  let name;
  if (target) {
    const next = structuredClone(target);
    CFG.applyForm(next, d);
    Object.assign(target, next); // mutates the object in place (extra unknown keys preserved)
    name = target.name;
  } else {
    const o = CFG.defaults();
    CFG.applyForm(o, d);
    S.rows.push(o);
    name = o.name;
  }
  touch(); closeModal(); renderAll();
  toast(target ? `<b>Updated</b> “${esc(name)}”.` : `<b>Created</b> “${esc(name)}”.`, 'ok');
}

/* ---------- delete / duplicate ---------- */
function confirmDelete(m) {
  openModal('Confirm deletion', `
    <div class="delwarn">${I.alert}<div class="delmsg">
      You are about to permanently remove the following <b>${esc(CFG.singular)}</b> from the data set:<br><br>
      <b>${esc(m.name)}</b><br>
      <span class="dimtxt">This action affects the JSON file the next time you save or export.</span>
    </div></div>`,
    `<button class="btn" id="dNo">${I.x} Cancel</button>
     <button class="btn btn-danger" id="dYes">${I.trash} Delete ${esc(CFG.singular)}</button>`,
    { onOpen() {
      $('#dNo').onclick = () => closeModal();
      $('#dYes').onclick = () => {
        const i = S.rows.indexOf(m);
        if (i > -1) S.rows.splice(i, 1);
        touch(); closeModal(); renderAll();
        toast(`<b>Deleted</b> “${esc(m.name)}”.`, 'warn');
      };
    }});
}

function duplicate(m) {
  const c = structuredClone(m);
  c.name = (m.name || 'Untitled') + ' (copy)';
  const i = S.rows.indexOf(m);
  S.rows.splice(i + 1, 0, c);
  touch(); renderAll();
  toast(`<b>Duplicated</b> as “${esc(c.name)}”.`, 'ok');
}

/* ---------- static bindings / init ---------- */
let qTimer;
function init() {
  $('#btnNew').onclick = () => showForm(null);
  $('#btnSave').onclick = doSave;
  $('#btnReload').onclick = doReload;
  $('#btnOpen').onclick = () => {
    if (window.showOpenFilePicker && S.loaded) openWithPicker();
    else showLoadPanel();
  };
  $('#btnClear').onclick = clearFilters;
  $('#q').addEventListener('input', e => {
    clearTimeout(qTimer);
    qTimer = setTimeout(() => { S.q = e.target.value.trim(); S.page = 1; renderAll(); }, 140);
  });
  $('#thead').addEventListener('click', e => {
    const th = e.target.closest('th[data-col]');
    if (!th) return;
    const id = th.dataset.col;
    if (S.sort.id === id) S.sort.dir *= -1;
    else { S.sort.id = id; S.sort.dir = 1; }
    renderAll();
  });
  $('#tbody').addEventListener('click', e => {
    const btn = e.target.closest('button[data-act]');
    if (!btn) return;
    const m = S.view[+btn.closest('tr').dataset.i];
    if (!m) return;
    ({ view: () => showDetail(m), edit: () => showForm(m), dup: () => duplicate(m), del: () => confirmDelete(m) })[btn.dataset.act]();
  });
  // dist legend
  $('#distPanel').addEventListener('click', e => {
    const leg = e.target.closest('.leg');
    if (!leg || !leg.dataset.id) return;
    setFilter(leg.dataset.id, S.filters[leg.dataset.id] === leg.dataset.val ? '' : leg.dataset.val);
  });
  // load panel
  const zone = $('#lpZone');
  $('#lpBrowse').onclick = () => $('#filePick').click();
  $('#filePick').addEventListener('change', e => readFile(e.target.files[0]));
  $('#lpPicker').onclick = openWithPicker;
  zone.addEventListener('dragover', e => { e.preventDefault(); zone.classList.add('over'); });
  zone.addEventListener('dragleave', () => zone.classList.remove('over'));
  zone.addEventListener('drop', e => {
    e.preventDefault(); zone.classList.remove('over');
    readFile(e.dataTransfer.files[0]);
  });
  // keyboard
  document.addEventListener('keydown', e => {
    if (e.key === 'Escape') closeModal();
    if (e.key === '/' && !/input|textarea|select/i.test(document.activeElement.tagName)) {
      e.preventDefault(); $('#q').focus();
    }
  });
  window.addEventListener('beforeunload', e => {
    if (S.dirty) { e.preventDefault(); e.returnValue = ''; }
  });
  autoLoad();
}
document.addEventListener('DOMContentLoaded', init);
