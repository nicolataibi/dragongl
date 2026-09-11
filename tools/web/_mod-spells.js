/* ============================================================
   DRAGONGL · DATA STUDIO — Spells module config
   ============================================================ */
window.DGL_CFG = (() => {

  const SCHOOLS = {
    abjuration:   { c: '#8b5cf6', l: 'Abjuration' },
    conjuration:  { c: '#22c55e', l: 'Conjuration' },
    divination:   { c: '#3b82f6', l: 'Divination' },
    enchantment:  { c: '#f472b6', l: 'Enchantment' },
    evocation:    { c: '#f97316', l: 'Evocation' },
    illusion:     { c: '#c084fc', l: 'Illusion' },
    martial:      { c: '#64748b', l: 'Martial' },
    necromancy:   { c: '#cbd5e1', l: 'Necromancy' },
    transmutation:{ c: '#eab308', l: 'Transmutation' },
  };
  const school = s => SCHOOLS[s] || { c: '#94a3b8', l: s || '—' };

  const LEVELS = {
    cantrip: { c: '#94a3b8', l: 'Cantrip', n: 0 },
    '1': { c: '#4ade80', l: '1st', n: 1 }, '2': { c: '#2dd4bf', l: '2nd', n: 2 },
    '3': { c: '#38bdf8', l: '3rd', n: 3 }, '4': { c: '#60a5fa', l: '4th', n: 4 },
    '5': { c: '#818cf8', l: '5th', n: 5 }, '6': { c: '#a78bfa', l: '6th', n: 6 },
    '7': { c: '#c084fc', l: '7th', n: 7 }, '8': { c: '#e879f9', l: '8th', n: 8 },
    '9': { c: '#f43f5e', l: '9th', n: 9 },
  };
  const lvl = l => LEVELS[l] || { c: '#94a3b8', l: String(l || '—'), n: 0 };

  const CLASSES = ['barbarian', 'bard', 'cleric', 'druid', 'fighter', 'monk',
    'paladin', 'ranger', 'rogue', 'sorcerer', 'warlock', 'wizard'];
  const classesOf = m => splitList(m.classes);

  const vfxOrb = v => {
    if (!v || typeof v !== 'object') return dash;
    const r = Math.round(num(v.r) * 255), g = Math.round(num(v.g) * 255), b = Math.round(num(v.b) * 255);
    return `<span class="orb" style="background:rgb(${r},${g},${b});box-shadow:0 0 10px 2px rgba(${r},${g},${b},.6),inset 0 0 5px rgba(255,255,255,.35)"></span>`;
  };

  return {
    key: 'spells',
    file: '../../data/spells.json',
    plural: 'spells',
    singular: 'spell',
    searchFields: ['name', 'description'],
    distTitle: 'Arcane spectrum by school',

    defaults: () => ({
      name: '', image: '', description: '', higher_levels: '',
      level: 'cantrip', school: 'evocation', classes: 'wizard',
      duration: 'Instantaneous', ritual: '', casting_time: '1 action', range: '30 feet',
      component_v: '1', component_s: '', component_m: '', materials: '',
      vfx: { type: 0, r: 0.8, g: 0.8, b: 0.8 },
    }),

    columns: [
      { id: 'name', label: 'Spell', sortVal: m => (m.name || '').toLowerCase(),
        render: m => `<span class="cell-name">${esc(m.name)}</span>` },
      { id: 'level', label: 'Level', sortVal: m => lvl(m.level).n,
        render: m => badge(lvl(m.level).l, lvl(m.level).c) },
      { id: 'school', label: 'School', sortVal: m => m.school || '',
        render: m => badge(school(m.school).l, school(m.school).c) },
      { id: 'casting_time', label: 'Cast time', sortVal: m => m.casting_time || '',
        render: m => m.casting_time ? `<span class="dimtxt">${esc(m.casting_time)}</span>` : dash },
      { id: 'range', label: 'Range', sortVal: m => m.range || '',
        render: m => m.range ? esc(m.range) : dash },
      { id: 'duration', label: 'Duration', sortVal: m => m.duration || '',
        render: m => m.duration ? `<span class="dimtxt">${esc(m.duration)}</span>` : dash },
      { id: 'ritual', label: 'Ritual', noSort: true,
        render: m => m.ritual === '1'
          ? `<span title="Ritual casting" style="color:#fbbf24">${I.star}</span>`
          : dash },
      { id: 'classes', label: 'Classes', noSort: true,
        render: m => { const c = classesOf(m);
          return c.length ? c.slice(0, 3).map(x => chip(x, '#93c5fd')).join(' ') +
            (c.length > 3 ? ` <span class="dimtxt">+${c.length - 3}</span>` : '') : dash; } },
      { id: 'vfx', label: 'VFX', noSort: true, render: m => vfxOrb(m.vfx) },
    ],

    filters: [
      { id: 'level', label: 'Level',
        options: () => Object.entries(LEVELS).map(([k, v]) => [k, v.l]),
        apply: (m, f) => !f || m.level === f },
      { id: 'school', label: 'School',
        options: () => Object.entries(SCHOOLS).map(([k, v]) => [k, v.l]),
        apply: (m, f) => !f || m.school === f },
      { id: 'cls', label: 'Class',
        options: () => CLASSES.map(c => [c, c[0].toUpperCase() + c.slice(1)]),
        apply: (m, f) => !f || classesOf(m).includes(f) },
      { id: 'ritual', label: 'Ritual only', type: 'check',
        apply: (m, f) => !f || m.ritual === '1' },
    ],

    distFilter: d => ({ id: 'school', value: d.key }),

    stats(rows) {
      const cantrips = rows.filter(r => r.level === 'cantrip').length;
      const rituals = rows.filter(r => r.ritual === '1').length;
      const innate = rows.filter(r => r.innate).length;
      const avg = (rows.reduce((a, r) => a + lvl(r.level).n, 0) / (rows.length || 1)).toFixed(1);
      return [
        { label: 'Total spells', value: fmtN(rows.length), sub: 'entries in spells.json' },
        { label: 'Cantrips', value: fmtN(cantrips), sub: '0-level known spells' },
        { label: 'Avg level', value: avg, sub: 'numeric level (cantrip = 0)' },
        { label: 'Rituals', value: fmtN(rituals), sub: 'castable as ritual' },
        { label: 'Innate', value: fmtN(innate), sub: 'class innate magic' },
      ];
    },

    dist(rows) {
      const map = {};
      for (const m of rows) (map[m.school] = map[m.school] || { key: m.school, count: 0 }).count++;
      return Object.values(map).sort((a, b) => b.count - a.count)
        .map(x => ({ label: school(x.key).l, count: x.count, color: school(x.key).c, key: x.key }));
    },

    detail(m) {
      const c = classesOf(m);
      const v = m.vfx;
      const vr = v ? `${Math.round(num(v.r) * 255)},${Math.round(num(v.g) * 255)},${Math.round(num(v.b) * 255)}` : '';
      return `
        <div class="d-head">
          ${badge(lvl(m.level).l, lvl(m.level).c)}
          ${badge(school(m.school).l, school(m.school).c)}
          ${m.ritual === '1' ? `<span title="Ritual casting" style="color:#fbbf24;display:inline-flex">${I.star}</span>` : ''}
          ${m.innate ? chip('innate', '#fbbf24') : ''}
        </div>
        <div class="kv-grid">
          ${kv('Casting time', esc(m.casting_time || '—'))}
          ${kv('Range', esc(m.range || '—'))}
          ${kv('Duration', esc(m.duration || '—'))}
          ${kv('Components',
            [m.component_v && 'V', m.component_s && 'S', m.component_m && 'M'].filter(Boolean)
              .map(x => chip(x, '#e2e8f0')).join(' ') || '—')}
          ${kv('Materials', m.materials ? esc(m.materials) : '—')}
          ${kv('Image ref', m.image ? esc(m.image) : '—')}
        </div>
        ${c.length ? `<div class="dsection"><h4>Class features</h4>
          <div class="defchips">${c.map(x => chip(x, '#93c5fd')).join(' ')}</div></div>` : ''}
        ${v ? `<div class="dsection"><h4>Visual effect</h4>
          <div class="dvfx">${vfxOrb(v)}<span class="dimtxt">type ${esc(v.type)} · rgb(${vr})</span></div></div>` : ''}
        <div class="dsection"><h4>Effect</h4>
          <p class="desc-text">${m.description ? keepBr(m.description) : dash}</p>
        </div>
        ${m.higher_levels ? `<div class="dsection"><h4>Higher levels</h4>
          <p class="desc-text">${keepBr(m.higher_levels)}</p></div>` : ''}`;
    },

    formFields: [
      { id: 'name', label: 'Spell name', type: 'text', req: true },
      { id: 'level', label: 'Level', type: 'select',
        options: () => Object.entries(LEVELS).map(([k, v]) => [k, v.l]) },
      { id: 'school', label: 'School', type: 'select',
        options: () => Object.entries(SCHOOLS).map(([k, v]) => [k, v.l]) },
      { id: 'image', label: 'Image ref', type: 'text' },
      { id: 'classes', label: 'Caster classes', type: 'checks', full: true, options: CLASSES },
      { id: 'casting_time', label: 'Casting time', type: 'text', placeholder: '1 action' },
      { id: 'range', label: 'Range', type: 'text', placeholder: '30 feet' },
      { id: 'duration', label: 'Duration', type: 'text', placeholder: 'Instantaneous' },
      { id: 'ritual', label: 'Ritual', type: 'toggle' },
      { id: 'innate', label: 'Innate class magic', type: 'toggle' },
      { id: 'component_v', label: 'Verbal (V)', type: 'toggle' },
      { id: 'component_s', label: 'Somatic (S)', type: 'toggle' },
      { id: 'component_m', label: 'Material (M)', type: 'toggle' },
      { id: 'materials', label: 'Material cost', type: 'text',
        hint: 'Required when the Material component is enabled.' },
      { id: 'vfx', label: 'VFX', type: 'vfx', full: true },
      { id: 'description', label: 'Description', type: 'textarea', full: true, req: false,
        hint: '<br> line breaks are supported and preserved.' },
      { id: 'higher_levels', label: 'Higher levels', type: 'textarea', full: true },
    ],

    applyForm(o, d) {
      o.name = d.name;
      o.image = d.image;
      o.level = d.level || 'cantrip';
      o.school = d.school || 'evocation';
      o.classes = (d.classes || []).join(',');
      o.casting_time = d.casting_time;
      o.range = d.range;
      o.duration = d.duration;
      o.ritual = d.ritual ? '1' : '';
      o.innate = d.innate ? 1 : 0;
      o.component_v = d.component_v ? '1' : '';
      o.component_s = d.component_s ? '1' : '';
      o.component_m = d.component_m ? '1' : '';
      o.materials = d.materials;
      o.vfx = d.vfx;
      o.description = d.description;
      o.higher_levels = d.higher_levels;
    },
  };
})();
