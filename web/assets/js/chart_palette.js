const CATEGORY_COLORS = new Map([
  ['餐饮', '#E69F00'],
  ['交通', '#0072B2'],
  ['住房', '#CC79A7'],
  ['学习', '#009E73'],
  ['医疗', '#D55E00'],
  ['娱乐', '#56B4E9']
]);

const FALLBACK_COLORS = ['#E69F00', '#0072B2', '#CC79A7', '#009E73', '#D55E00', '#56B4E9', '#F0E442', '#6F4E9C'];

export function categoryColor(category) {
  const normalized = String(category).trim();
  const knownColor = CATEGORY_COLORS.get(normalized);
  if (knownColor) {
    return knownColor;
  }
  let hash = 2166136261;
  for (const character of normalized) {
    hash ^= character.codePointAt(0);
    hash = Math.imul(hash, 16777619);
  }
  return FALLBACK_COLORS[(hash >>> 0) % FALLBACK_COLORS.length];
}
