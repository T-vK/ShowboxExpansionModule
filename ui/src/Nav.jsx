import { go } from "./route.js";

export function Nav({ current, extra }) {
  const link = (href, label) => (
    <a href={href} class={current === href ? "current" : ""} onClick={(event) => go(event, href)}>{label}</a>
  );
  return (
    <p class="lead nav">
      {link("/remote-ui", "Showbox controls")}
      <span>·</span>
      {link("/services", "Services")}
      <span>·</span>
      {link("/actions", "Reactions")}
      {extra ? <span>· {extra}</span> : null}
    </p>
  );
}
