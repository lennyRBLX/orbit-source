export type Place = { top: number; left: number };

// Where a tile sits on screen, so Back can put its list there again.
export function placeOf(tile: Element | null): Place | null {
  if (!tile) return null;
  const { top, left } = tile.getBoundingClientRect();
  return { top, left };
}

/* Scrolls the tile's scrolling ancestors, nearest first, until it is back at
 * `place` on screen: a game page replaces the list, so the list comes back at
 * the top (the Store look's page, Panorama's sideways page). The page itself
 * is the last ancestor; other boxes scroll only along an axis they allow. */
export function restorePlace(tile: HTMLElement, place: Place) {
  const page = tile.ownerDocument.scrollingElement;
  for (let box = tile.parentElement; box; box = box.parentElement) {
    const now = tile.getBoundingClientRect();
    const dx = Math.round(now.left - place.left);
    const dy = Math.round(now.top - place.top);
    if (!dx && !dy) return;
    const style = box === page ? null : getComputedStyle(box);
    if (!style || /auto|scroll/.test(style.overflowX)) box.scrollLeft += dx;
    if (!style || /auto|scroll/.test(style.overflowY)) box.scrollTop += dy;
  }
}
