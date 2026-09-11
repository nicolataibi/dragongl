/* ============================================================
   DRAGONGL · DATA STUDIO — Items module config
   ============================================================ */
window.DGL_CFG = (() => {

  const RAR = {
    MUNDANE:     { c: '#94a3b8', l: 'Mundane' },
    COMMON:      { c: '#cbd5e1', l: 'Common' },
    UNCOMMON:    { c: '#4ade80', l: 'Uncommon' },
    RARE:        { c: '#38bdf8', l: 'Rare' },
    VERY_RARE:   { c: '#c084fc', l: 'Very Rare' },
    LEGENDARY:   { c: '#fb923c', l: 'Legendary' },
    ARTIFACT:    { c: '#fbbf24', l: 'Artifact' },
  };
  const normRar = r => String(r || '').trim().replace(/\s+/g, '_').toUpperCase();
  const rar = r => RAR[normRar(r)] || null;

  const CAT = {
    ADVENTURING_GEAR: { c: '#22d3ee', l: 'Adventuring Gear' },
    ARMOR:            { c: '#94a3b8', l: 'Armor' },
    BOOK:             { c: '#60a5fa', l: 'Book' },
    JUNK:             { c: '#78716c', l: 'Junk' },
    OTHER:            { c: '#64748b', l: 'Other' },
    POISONS:          { c: '#84cc16', l: 'Poisons' },
    POTIONS_OILS:     { c: '#34d399', l: 'Potions & Oils' },
    RING:             { c: '#c084fc', l: 'Ring' },
    TOOLS:            { c: '#fb923c', l: 'Tools' },
    WEAPON:           { c: '#f87171', l: 'Weapon' },
    WONDROUS_ITEMS:   { c: '#fbbf24', l: 'Wondrous Items' },
  };
  const cat = c => CAT[c] || { c: '#64748b', l: c || '—' };

  const propsOf = m => splitList(m.properties);

  return {
    key: 'items',
    file: '../../data/items.json',
    plural: 'items',
    singular: 'item',
    searchFields: ['name', 'description'],
    distTitle: 'Inventory composition by category',

    defaults: () => ({
      name: '', image: '', description: '',
      category: 'OTHER', rarity: '', classification: '',
      ac: '', damage: '', damage_type: '', properties: '', cost: '0',
    }),

    columns: [
      { id: 'name', label: 'Name', sortVal: m => (m.name || '').toLowerCase(),
        render: m => `<span class="cell-name">${esc(m.name)}</span>` },
      { id: 'category', label: 'Category', sortVal: m => m.category || '',
        render: m => badge(cat(m.category).l, cat(m.category).c) },
      { id: 'rarity', label: 'Rarity', sortVal: m => normRar(m.rarity),
        render: m => { const r = rar(m.rarity); return r ? badge(r.l, r.c) : dash; } },
      { id: 'classification', label: 'Classification', sortVal: m => m.classification || '',
        render: m => m.classification ? `<span class="dimtxt">${esc(m.classification)}</span>` : dash },
      { id: 'ac', label: 'AC', cls: 'num', sortVal: m => num(m.ac),
        render: m => m.ac ? esc(m.ac) : dash },
      { id: 'damage', label: 'Damage', sortVal: m => m.damage || '',
        render: m => m.damage
          ? `<span class="dice">${esc(m.damage)}</span>${m.damage_type ? ' <span class="dimtxt">' + esc(m.damage_type) + '</span>' : ''}`
          : dash },
      { id: 'cost', label: 'Cost', cls: 'num', sortVal: m => num(m.cost),
        render: m => num(m.cost) ? fmtN(m.cost) : dash },
      { id: 'props', label: 'Properties', noSort: true,
        render: m => { const p = propsOf(m); return p.length ? p.map(x => chip(x, '#7dd3fc')).join(' ') : dash; } },
    ],

    filters: [
      { id: 'cat', label: 'Category',
        options: () => Object.entries(CAT).map(([k, v]) => [k, v.l]),
        apply: (m, f) => !f || m.category === f },
      { id: 'rar', label: 'Rarity',
        options: () => Object.entries(RAR).map(([k, v]) => [k, v.l]),
        apply: (m, f) => !f || normRar(m.rarity) === f },
    ],

    distFilter: d => ({ id: 'cat', value: d.key }),

    stats(rows) {
      const cats = new Set(rows.map(r => r.category));
      const leg = rows.filter(r => ['LEGENDARY', 'ARTIFACT'].includes(normRar(r.rarity))).length;
      const avg = Math.round(rows.reduce((a, r) => a + num(r.cost), 0) / (rows.length || 1));
      return [
        { label: 'Total items', value: fmtN(rows.length), sub: 'entries in items.json' },
        { label: 'Categories', value: fmtN(cats.size), sub: 'distinct loot groups' },
        { label: 'Avg cost', value: fmtN(avg), sub: 'mean item price' },
        { label: 'Legendary+', value: fmtN(leg), sub: 'legendary & artifacts' },
      ];
    },

    dist(rows) {
      const map = {};
      for (const m of rows) (map[m.category] = map[m.category] || { key: m.category, count: 0 }).count++;
      return Object.values(map).sort((a, b) => b.count - a.count)
        .map(x => ({ label: cat(x.key).l, count: x.count, color: cat(x.key).c, key: x.key }));
    },

    detail(m) {
      const r = rar(m.rarity);
      const p = propsOf(m);
      return `
        <div class="d-head">
          ${badge(cat(m.category).l, cat(m.category).c)}
          ${r ? badge(r.l, r.c) : ''}
          ${m.classification ? chip(m.classification, '#94a3b8') : ''}
          ${m.material ? chip('material: ' + m.material, '#a3e635') : ''}
        </div>
        <div class="kv-grid">
          ${kv('Armor class', m.ac ? esc(m.ac) : '—')}
          ${kv('Damage', m.damage ? esc(m.damage) + (m.damage_type ? ' ' + esc(m.damage_type) : '') : '—')}
          ${kv('Cost', num(m.cost) ? fmtN(m.cost) : '—', true)}
          ${kv('Image ref', m.image ? esc(m.image) : '—')}
        </div>
        ${p.length ? `<div class="dsection"><h4>Properties</h4>
          <div class="defchips">${p.map(x => chip(x, '#7dd3fc')).join(' ')}</div></div>` : ''}
        <div class="dsection"><h4>Description</h4>
          <p class="desc-text">${m.description ? keepBr(m.description) : dash}</p>
        </div>`;
    },

    formFields: [
      { id: 'name', label: 'Name', type: 'text', req: true },
      { id: 'image', label: 'Image ref', type: 'text', hint: 'Image file name used by the client renderer (may be empty).' },
      { id: 'category', label: 'Category', type: 'select',
        options: () => Object.entries(CAT).map(([k, v]) => [k, v.l]) },
      { id: 'rarity', label: 'Rarity', type: 'select',
        options: () => Object.entries(RAR).map(([k, v]) => [k, v.l]) },
      { id: 'classification', label: 'Classification', type: 'text',
        hint: 'e.g. “Light Armor”, “Martial Melee Weapons” (may be empty).' },
      { id: 'ac', label: 'Armor class', type: 'text', placeholder: 'e.g. 11 + Dex' },
      { id: 'damage', label: 'Damage', type: 'text', placeholder: 'e.g. 1d6' },
      { id: 'damage_type', label: 'Damage type', type: 'text',
        datalist: ['bludgeoning', 'piercing', 'slashing', 'fire', 'cold', 'acid', 'poison', 'thunder', 'psychic'] },
      { id: 'cost', label: 'Cost', type: 'number' },
      { id: 'material', label: 'Material', type: 'text', hint: 'Optional (read by the engine when present).' },
      { id: 'properties', label: 'Properties', type: 'list', full: true,
        placeholder: 'light, thrown, heavy …' },
      { id: 'description', label: 'Description', type: 'textarea', full: true,
        hint: '<br> line breaks are supported and preserved.' },
    ],

    applyForm(o, d) {
      o.name = d.name;
      o.image = d.image;
      o.category = d.category || 'OTHER';
      o.rarity = d.rarity;
      o.classification = d.classification;
      o.ac = d.ac;
      o.damage = d.damage;
      o.damage_type = d.damage_type;
      o.cost = String(+d.cost || 0);
      o.material = d.material;
      o.properties = d.properties;
      o.description = d.description;
    },
  };
})();
