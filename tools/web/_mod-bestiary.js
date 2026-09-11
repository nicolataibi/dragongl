/* ============================================================
   DRAGONGL · DATA STUDIO — Bestiary module config
   ============================================================ */
window.DGL_CFG = (() => {

  const ARCH = {
    melee:   { c: '#f87171', l: 'Melee' },
    brute:   { c: '#d97706', l: 'Brute' },
    caster:  { c: '#a78bfa', l: 'Caster' },
    dragon:  { c: '#fb7185', l: 'Dragon' },
    swarm:   { c: '#4ade80', l: 'Swarm' },
    boss:    { c: '#fbbf24', l: 'Boss' },
  };
  const arch = a => ARCH[a] || { c: '#94a3b8', l: a || 'unknown' };

  const CTYPES = {
    Aberration: '#c084fc', Beast: '#a3e635', Celestial: '#fde68a',
    Construct: '#9ca3af', Creature: '#94a3b8', Dragon: '#fb7185',
    Elemental: '#38bdf8', Fey: '#4ade80', Fiend: '#f87171',
    Humanoid: '#60a5fa', Monstrosity: '#fb923c', Ooze: '#2dd4bf',
    Plant: '#34d399', Spirit: '#67e8f9', Undead: '#cbd5e1',
  };
  const ctypeOf = m => {
    const t = /Type:\s*([A-Za-z]+(?:\s[A-Za-z]+)?)\.\s/.exec(m.description || '');
    return t ? t[1] : null;
  };

  const DAMAGETYPES = ['acid', 'bludgeoning', 'cold', 'fire', 'force', 'lightning',
    'necrotic', 'piercing', 'poison', 'psychic', 'radiant', 'slashing', 'thunder'];
  const CONDITIONS = ['blinded', 'charmed', 'deafened', 'frightened', 'grappled',
    'paralyzed', 'petrified', 'poisoned', 'prone', 'restrained', 'stunned', 'unconscious'];

  const defRow = (title, arr, color) => `
    <div class="defrow"><span class="defl">${title}</span>
      <div class="defchips">${Array.isArray(arr) && arr.length
        ? arr.map(c => chip(c, color)).join(' ')
        : dash}</div></div>`;

  const defChips = m => {
    const r = (m.damage_resistances || []).length,
          i = (m.damage_immunities || []).length,
          v = (m.damage_vulnerabilities || []).length;
    const out = [];
    if (r) out.push(chip(`R ${r}`, '#38bdf8'));
    if (i) out.push(chip(`I ${i}`, '#34d399'));
    if (v) out.push(chip(`V ${v}`, '#f87171'));
    return out.length ? out.join(' ') : dash;
  };

  return {
    key: 'monsters',
    file: '../../data/bestiary.json',
    plural: 'monsters',
    singular: 'monster',
    searchFields: ['name', 'description'],
    distTitle: 'Troop composition by archetype',

    defaults: () => ({
      name: '', archetype: 'melee',
      hp_avg: 10, ac: 10, xp: 10, gold: 0,
      speed: 2, sight_range: 5,
      damage_dice: 1, damage_sides: 4,
      floor_min: 1, floor_max: 30,
      description: '',
      damage_resistances: [], damage_immunities: [],
      damage_vulnerabilities: [], condition_immunities: [],
    }),

    columns: [
      { id: 'name', label: 'Name', sortVal: m => (m.name || '').toLowerCase(),
        render: m => `<span class="cell-name">${esc(m.name)}</span>` },
      { id: 'type', label: 'Type', sortVal: m => ctypeOf(m) || 'zzz',
        render: m => { const t = ctypeOf(m); return t ? badge(t, CTYPES[t] || '#94a3b8') : dash; } },
      { id: 'archetype', label: 'Archetype', sortVal: m => m.archetype || '',
        render: m => badge(arch(m.archetype).l, arch(m.archetype).c) },
      { id: 'hp_avg', label: 'HP', cls: 'num', sortVal: m => num(m.hp_avg),
        render: m => fmtN(m.hp_avg) },
      { id: 'ac', label: 'AC', cls: 'num', sortVal: m => num(m.ac),
        render: m => fmtN(m.ac) },
      { id: 'dmg', label: 'Damage', sortVal: m => num(m.damage_dice) * num(m.damage_sides),
        render: m => m.damage_dice || m.damage_sides
          ? `<span class="dice">${esc(m.damage_dice)}d${esc(m.damage_sides)}</span>` : dash },
      { id: 'xp', label: 'XP', cls: 'num', sortVal: m => num(m.xp),
        render: m => fmtN(m.xp) },
      { id: 'gold', label: 'Gold', cls: 'num', sortVal: m => num(m.gold),
        render: m => fmtN(m.gold) },
      { id: 'defs', label: 'Defenses', noSort: true,
        render: m => defChips(m) },
    ],

    filters: [
      { id: 'arch', label: 'Archetype',
        options: () => Object.entries(ARCH).map(([k, v]) => [k, v.l]),
        apply: (m, f) => !f || m.archetype === f },
      { id: 'minxp', label: 'Min XP',
        options: () => [['100', 'XP ≥ 100'], ['500', 'XP ≥ 500'], ['2000', 'XP ≥ 2,000'],
                        ['10000', 'XP ≥ 10,000'], ['50000', 'XP ≥ 50,000']],
        apply: (m, f) => !f || num(m.xp) >= +f },
      { id: 'minhp', label: 'Min HP',
        options: () => [['50', 'HP ≥ 50'], ['100', 'HP ≥ 100'], ['500', 'HP ≥ 500'],
                        ['1000', 'HP ≥ 1,000'], ['5000', 'HP ≥ 5,000']],
        apply: (m, f) => !f || num(m.hp_avg) >= +f },
    ],

    distFilter: d => ({ id: 'arch', value: d.key }),

    stats(rows) {
      const n = rows.length || 1;
      const sum = k => rows.reduce((a, m) => a + num(m[k]), 0);
      return [
        { label: 'Total monsters', value: fmtN(rows.length), sub: 'entries in bestiary.json' },
        { label: 'Avg HP', value: fmtN(Math.round(sum('hp_avg') / n)), sub: 'hit points (mean)' },
        { label: 'Avg AC', value: fmtN(Math.round(sum('ac') / n)), sub: 'armor class (mean)' },
        { label: 'Avg XP', value: fmtN(Math.round(sum('xp') / n)), sub: 'experience (mean)' },
        { label: 'Total gold', value: fmtN(sum('gold')), sub: 'sum of treasure hoards' },
      ];
    },

    dist(rows) {
      const map = {};
      for (const m of rows) (map[m.archetype] = map[m.archetype] || { key: m.archetype, count: 0 }).count++;
      return Object.values(map).sort((a, b) => b.count - a.count)
        .map(x => ({ label: arch(x.key).l, count: x.count, color: arch(x.key).c, key: x.key }));
    },

    detail(m) {
      const t = ctypeOf(m);
      return `
        <div class="d-head">
          ${t ? badge(t, CTYPES[t] || '#94a3b8') : ''}
          ${badge(arch(m.archetype).l, arch(m.archetype).c)}
          ${m.damage_dice ? `<span class="dice">${esc(m.damage_dice)}d${esc(m.damage_sides)}</span>` : ''}
          ${m.speed ? chip('speed ' + esc(m.speed), '#94a3b8') : ''}
        </div>
        <div class="kv-grid">
          ${kv('HP average', fmtN(m.hp_avg))}
          ${kv('Armor class', fmtN(m.ac))}
          ${kv('Experience', fmtN(m.xp))}
          ${kv('Gold', fmtN(m.gold), true)}
          ${kv('Speed', esc(m.speed ?? '—'))}
          ${kv('Sight range', esc(m.sight_range ?? '—'))}
          ${kv('Damage dice', m.damage_dice ? `${esc(m.damage_dice)}d${esc(m.damage_sides)}` : '—')}
          ${kv('Floor range', m.floor_min !== undefined ? `${esc(m.floor_min)} – ${esc(m.floor_max)}` : '—')}
        </div>
        <div class="dsection"><h4>Damage profile</h4>
          ${defRow('Resistances', m.damage_resistances, '#38bdf8')}
          ${defRow('Immunities', m.damage_immunities, '#34d399')}
          ${defRow('Vulnerabilities', m.damage_vulnerabilities, '#f87171')}
          ${defRow('Condition immunities', m.condition_immunities, '#e2e8f0')}
        </div>
        <div class="dsection"><h4>Description</h4>
          <p class="desc-text">${m.description ? keepBr(m.description) : dash}</p>
        </div>`;
    },

    formFields: [
      { id: 'name', label: 'Name', type: 'text', req: true },
      { id: 'archetype', label: 'Archetype', type: 'select',
        options: () => Object.entries(ARCH).map(([k, v]) => [k, v.l]) },
      { id: 'hp_avg', label: 'HP average', type: 'number' },
      { id: 'ac', label: 'Armor class', type: 'number' },
      { id: 'xp', label: 'Experience', type: 'number' },
      { id: 'gold', label: 'Gold', type: 'number' },
      { id: 'speed', label: 'Speed', type: 'number' },
      { id: 'sight_range', label: 'Sight range', type: 'number' },
      { id: 'damage_dice', label: 'Damage dice', type: 'number' },
      { id: 'damage_sides', label: 'Damage sides', type: 'number' },
      { id: 'floor_min', label: 'Floor min', type: 'number' },
      { id: 'floor_max', label: 'Floor max', type: 'number' },
      { id: 'damage_resistances', label: 'Damage resistances', type: 'list', full: true,
        datalist: DAMAGETYPES, placeholder: 'acid, cold, fire …' },
      { id: 'damage_immunities', label: 'Damage immunities', type: 'list', full: true,
        datalist: DAMAGETYPES, placeholder: 'bludgeoning, poison …' },
      { id: 'damage_vulnerabilities', label: 'Damage vulnerabilities', type: 'list', full: true,
        datalist: DAMAGETYPES, placeholder: 'poison, psychic …' },
      { id: 'condition_immunities', label: 'Condition immunities', type: 'list', full: true,
        datalist: CONDITIONS, placeholder: 'frightened, poisoned …' },
      { id: 'description', label: 'Description', type: 'textarea', full: true,
        hint: 'Keep the “Type: …” prefix — the creature type badge is parsed from it. <br> line breaks are supported.' },
    ],

    applyForm(o, d) {
      o.name = d.name;
      o.archetype = d.archetype || 'melee';
      o.hp_avg = +d.hp_avg || 0;
      o.ac = +d.ac || 0;
      o.xp = +d.xp || 0;
      o.gold = +d.gold || 0;
      o.speed = +d.speed || 2;
      o.sight_range = +d.sight_range || 5;
      o.damage_dice = +d.damage_dice || 1;
      o.damage_sides = +d.damage_sides || 4;
      o.floor_min = +d.floor_min || 0;
      o.floor_max = +d.floor_max || 0;
      o.damage_resistances = splitList(d.damage_resistances);
      o.damage_immunities = splitList(d.damage_immunities);
      o.damage_vulnerabilities = splitList(d.damage_vulnerabilities);
      o.condition_immunities = splitList(d.condition_immunities);
      o.description = d.description;
    },
  };
})();
