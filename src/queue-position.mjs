// Keep identity across live list changes. If it disappeared, prefer the next
// surviving neighbour, then the previous one (including at the end of a list).
export function retainedKey(previous, current, keys) {
  const available = new Set(keys);
  if (available.has(current)) return current;
  const at = previous.indexOf(current);
  if (at >= 0) {
    for (let i = at + 1; i < previous.length; i++) if (available.has(previous[i])) return previous[i];
    for (let i = at - 1; i >= 0; i--) if (available.has(previous[i])) return previous[i];
  }
  return keys[0];
}

// Status changes must not re-sort a queue underneath somebody working through
// failures. Keep this visit's order; new entries join at the end.
export function retainedOrder(previous, keys) {
  const available = new Set(keys), known = new Set(previous);
  return [...previous.filter(key => available.has(key)), ...keys.filter(key => !known.has(key))];
}
