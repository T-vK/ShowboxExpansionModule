export function pathOf() {
  return window.location.pathname.replace(/\/$/, "") || "/";
}

export function go(event, href) {
  if (event.metaKey || event.ctrlKey || event.shiftKey || event.altKey || event.button !== 0) {
    return;
  }
  event.preventDefault();
  if (window.location.pathname !== href) {
    history.pushState({}, "", href);
  }
  window.dispatchEvent(new PopStateEvent("popstate"));
}
