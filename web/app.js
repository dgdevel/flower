// flower POC: show the server clock, streamed over Server-Sent Events.
const clock = document.getElementById("clock");
const status = document.getElementById("status");

// Relative URL: also works behind path-prefixed proxies (a page served at
// /proxy/8080/ resolves this to /proxy/8080/api/time, which the proxy maps
// to /api/time on the server).
const es = new EventSource("api/time");

es.onopen = () => {
  status.textContent = "live — server-sent events at /api/time";
  status.classList.remove("connecting");
  status.classList.add("live");
};

es.onmessage = (e) => {
  const t = JSON.parse(e.data);
  clock.textContent = t.iso;
  document.title = t.iso;
};

es.onerror = () => {
  status.textContent = "connection lost — reconnecting…";
  status.classList.remove("live");
  status.classList.add("connecting");
};
