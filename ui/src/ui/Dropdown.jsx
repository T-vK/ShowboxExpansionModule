import { createPortal } from "preact/compat";
import { useEffect, useLayoutEffect, useState } from "preact/hooks";

function menuStyle(rect, minWidth) {
  const gap = 6;
  const maxW = Math.max(160, window.innerWidth - 16);
  const spaceBelow = window.innerHeight - rect.bottom - 12;
  const spaceAbove = rect.top - 12;
  const openUp = spaceBelow < 180 && spaceAbove > spaceBelow;
  const height = Math.max(120, openUp ? spaceAbove : spaceBelow);
  const width = Math.min(maxW, Math.max(rect.width, minWidth));
  const left = Math.min(Math.max(8, rect.left), Math.max(8, window.innerWidth - width - 8));
  return {
    position: "fixed",
    left: left + "px",
    width: width + "px",
    maxWidth: maxW + "px",
    maxHeight: height + "px",
    overflowX: "clip",
    overflowY: "auto",
    overscrollBehavior: "contain",
    top: openUp ? "auto" : rect.bottom + gap + "px",
    bottom: openUp ? window.innerHeight - rect.top + gap + "px" : "auto",
    zIndex: 120
  };
}

export function Dropdown({ anchorRef, open, wide, children, onClose }) {
  const [rect, setRect] = useState(null);
  const anchor = anchorRef && anchorRef.current;

  useLayoutEffect(() => {
    if (!open || !anchor) {
      setRect(null);
      return;
    }
    const measure = () => {
      if (anchorRef.current) setRect(anchorRef.current.getBoundingClientRect());
    };
    measure();
    window.addEventListener("resize", measure);
    window.addEventListener("scroll", measure, true);
    document.addEventListener("fullscreenchange", measure);
    document.addEventListener("webkitfullscreenchange", measure);
    return () => {
      window.removeEventListener("resize", measure);
      window.removeEventListener("scroll", measure, true);
      document.removeEventListener("fullscreenchange", measure);
      document.removeEventListener("webkitfullscreenchange", measure);
    };
  }, [open, anchor, anchorRef]);

  useEffect(() => {
    if (!open) return;
    const onPointer = (event) => {
      const node = event.target;
      if (anchorRef.current && anchorRef.current.contains(node)) return;
      if (node.closest && node.closest(".picker-menu")) return;
      onClose && onClose();
    };
    const onKey = (event) => {
      if (event.key === "Escape") onClose && onClose();
    };
    window.addEventListener("pointerdown", onPointer);
    window.addEventListener("keydown", onKey);
    return () => {
      window.removeEventListener("pointerdown", onPointer);
      window.removeEventListener("keydown", onKey);
    };
  }, [open, anchorRef, onClose]);

  if (!open || !rect || typeof document === "undefined") return null;
  const mount = document.fullscreenElement || document.webkitFullscreenElement || document.body;
  return createPortal(
    <div class={"picker-menu" + (wide ? " wide" : "")} style={menuStyle(rect, wide ? 260 : 168)}>
      {children}
    </div>,
    mount
  );
}
