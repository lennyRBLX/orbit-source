import { Component, createRef, useLayoutEffect, useRef, type ReactNode } from "react";
import { retainedKey } from "./queue-position.mjs";

export function useRetainedSelection(keys: string[], current: string | null | undefined) {
  const previous = useRef<string[]>([]);
  const selected = retainedKey(previous.current, current, keys) as string | undefined;
  useLayoutEffect(() => { previous.current = keys; });
  return selected;
}

type Props = { children: ReactNode; resetKey?: string };
type Snapshot = {
  keys: string[]; key: string | undefined; offset: number;
  scroller: HTMLElement; horizontal: boolean; focus: string | undefined;
};
const selector = "[data-queue-key]";
const keyOf = (row: HTMLElement) => row.dataset.queueKey!;
function scrollParent(row: HTMLElement) {
  for (let node = row.parentElement; node; node = node.parentElement) {
    const style = getComputedStyle(node);
    if (/(auto|scroll)/.test(style.overflow + style.overflowX + style.overflowY)) return node;
  }
  return document.scrollingElement as HTMLElement;
}

// React's pre-commit snapshot sees the old geometry even when an async refresh
// removes a row. Restoring before paint also accounts for browser scroll anchoring.
export class QueuePosition extends Component<Props, Record<string, never>, Snapshot | null> {
  root = createRef<HTMLDivElement>();
  focused: string | undefined;
  getSnapshotBeforeUpdate(previous: Props): Snapshot | null {
    if (previous.resetKey !== this.props.resetKey) { this.focused = undefined; return null; }
    const rows = [...(this.root.current?.querySelectorAll<HTMLElement>(selector) || [])];
    if (!rows.length) return null;
    const scroller = scrollParent(rows[0]);
    const horizontal = scroller.dataset.scroll === "x";
    const bounds = scroller === document.scrollingElement
      ? { left: 0, right: innerWidth, top: 0, bottom: innerHeight }
      : scroller.getBoundingClientRect();
    const visible = rows.filter(row => {
      const r = row.getBoundingClientRect();
      return r.width && r.height && r.right > bounds.left && r.left < bounds.right && r.bottom > bounds.top && r.top < bounds.bottom;
    });
    // Anchor the first visible item, not a resumed item that may leave its group.
    const row = visible[0];
    return {
      keys: rows.map(keyOf), key: row && keyOf(row),
      offset: row ? row.getBoundingClientRect()[horizontal ? "left" : "top"] : 0,
      scroller, horizontal, focus: this.focused,
    };
  }
  componentDidUpdate(_props: Props, _state: Record<string, never>, snapshot: Snapshot | null) {
    if (!snapshot || !this.root.current) return;
    const rows = [...this.root.current.querySelectorAll<HTMLElement>(selector)];
    const keys = rows.map(keyOf);
    const key = retainedKey(snapshot.keys, snapshot.key, keys);
    const row = rows.find(row => keyOf(row) === key);
    if (snapshot.key && row && snapshot.scroller.isConnected) {
      const delta = row.getBoundingClientRect()[snapshot.horizontal ? "left" : "top"] - snapshot.offset;
      if (snapshot.horizontal) snapshot.scroller.scrollLeft += delta;
      else snapshot.scroller.scrollTop += delta;
    }
    // Never steal focus from an open dialog or another control. Disabled/removed
    // action buttons can leave focus on body; hand it to a surviving queue item.
    if (snapshot.focus && document.activeElement === document.body && !document.querySelector('dialog[open], [role="dialog"]')) {
      const focusKey = retainedKey(snapshot.keys, snapshot.focus, keys);
      const focusRow = rows.find(row => keyOf(row) === focusKey);
      const button = focusRow?.querySelector<HTMLElement>('button:not(:disabled), [tabindex="0"]');
      (button || focusRow)?.focus({ preventScroll: true });
      this.focused = focusKey;
    }
  }
  render() {
    return <div ref={this.root} style={{ display: "contents" }} onFocusCapture={event => {
      const target = event.target as HTMLElement;
      const row = target.closest<HTMLElement>(selector);
      if (row) this.focused = keyOf(row);
      else if (!target.closest('dialog, [role="dialog"]')) this.focused = undefined;
    }}>{this.props.children}</div>;
  }
}
