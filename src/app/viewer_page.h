#pragma once
#include <string>

namespace morton {

/// The single-file browser visualiser, with `default_ws` baked in as the
/// endpoint it connects to when no `?ws=` override is supplied.
inline std::string viewer_page(const std::string& default_ws) {
    static const char* kBefore = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Morton</title>
<style>
  :root { color-scheme: dark; }
  body { margin: 0; background: #07090d; color: #d8dee9; font: 13px ui-monospace, SFMono-Regular, Menlo, monospace; }
  #stage { display: block; width: 100vw; height: 100vh; }
  #hud { position: fixed; top: 12px; left: 12px; padding: 10px 14px; background: rgba(10,14,20,.82);
         border: 1px solid #1d2530; border-radius: 6px; line-height: 1.7; pointer-events: none; }
  #hud b { color: #7fd1ff; font-weight: 600; }
  #shards { position: fixed; top: 12px; right: 12px; padding: 10px 14px; background: rgba(10,14,20,.82);
            border: 1px solid #1d2530; border-radius: 6px; line-height: 1.7; }
  .dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%; margin-right: 6px; }
  .down { opacity: .45; text-decoration: line-through; }
</style>
</head>
<body>
<canvas id="stage"></canvas>
<div id="hud"></div>
<div id="shards"></div>
<script>
const params = new URLSearchParams(location.search);
const endpoints = (params.get('ws') || ')HTML";

    static const char* kAfter = R"HTML(').split(',').filter(Boolean);
const palette = ['#7fd1ff', '#ffb35c', '#8bec9b', '#e58cff', '#ff7a8a', '#f5e17a'];

const canvas = document.getElementById('stage');
const ctx = canvas.getContext('2d');
const shards = new Map();
let worldSize = 2048, regionsPerAxis = 2;

function resize() {
  const ratio = window.devicePixelRatio || 1;
  canvas.width = Math.floor(innerWidth * ratio);
  canvas.height = Math.floor(innerHeight * ratio);
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
}
addEventListener('resize', resize);
resize();

function connect(endpoint, index) {
  const state = { endpoint, index, color: palette[index % palette.length], live: false,
                  entities: [], players: 0, tick: 0, tickP99: 0, owned: [], frames: 0, lastFrameAt: 0 };
  shards.set(endpoint, state);

  const open = () => {
    const socket = new WebSocket('ws://' + endpoint);
    socket.onopen = () => { state.live = true; };
    socket.onclose = () => { state.live = false; setTimeout(open, 1000); };
    socket.onmessage = event => {
      const frame = JSON.parse(event.data);
      state.id = frame.shard;
      state.entities = frame.entities || [];
      state.players = frame.players || 0;
      state.tick = frame.tick || 0;
      state.tickP99 = frame.tick_p99_ms || 0;
      state.owned = frame.owned_regions || [];
      state.frames++;
      state.lastFrameAt = performance.now();
      worldSize = frame.world_size || worldSize;
      regionsPerAxis = frame.regions_per_axis || regionsPerAxis;
    };
  };
  open();
}

if (endpoints.length === 0) endpoints.push(location.host);
endpoints.forEach(connect);

function ownerOf(region) {
  for (const state of shards.values()) {
    if (state.live && state.owned.includes(region)) return state;
  }
  return null;
}

function draw() {
  const size = Math.min(innerWidth, innerHeight) - 48;
  const originX = (innerWidth - size) / 2;
  const originY = (innerHeight - size) / 2;
  const scale = size / worldSize;

  ctx.clearRect(0, 0, innerWidth, innerHeight);
  ctx.fillStyle = '#0b0f16';
  ctx.fillRect(originX, originY, size, size);

  const cells = regionsPerAxis;
  const cell = size / cells;
  for (let y = 0; y < cells; y++) {
    for (let x = 0; x < cells; x++) {
      const region = y * cells + x;
      const owner = ownerOf(region);
      ctx.fillStyle = owner ? owner.color + '14' : '#ffffff06';
      ctx.fillRect(originX + x * cell, originY + y * cell, cell, cell);
      ctx.strokeStyle = '#1d2530';
      ctx.strokeRect(originX + x * cell, originY + y * cell, cell, cell);
      ctx.fillStyle = owner ? owner.color + '99' : '#41505f';
      ctx.fillText((owner ? owner.id || owner.endpoint : 'unowned') + '  r' + region,
                   originX + x * cell + 8, originY + y * cell + 16);
    }
  }

  let players = 0, entities = 0, tickP99 = 0, live = 0;
  for (const state of shards.values()) {
    if (!state.live) continue;
    live++;
    players += state.players;
    tickP99 = Math.max(tickP99, state.tickP99);
    for (const entity of state.entities) {
      entities++;
      const px = originX + entity.x * scale;
      const py = originY + entity.y * scale;
      const isPlayer = entity.kind === 0;
      ctx.beginPath();
      ctx.arc(px, py, isPlayer ? 3.2 : 1.6, 0, Math.PI * 2);
      ctx.fillStyle = isPlayer ? state.color : state.color + '55';
      ctx.fill();
    }
  }

  document.getElementById('hud').innerHTML =
    '<b>morton</b> distributed simulation<br>' +
    'shards live <b>' + live + '</b> / ' + shards.size + '<br>' +
    'players <b>' + players + '</b><br>' +
    'entities <b>' + entities + '</b><br>' +
    'worst tick p99 <b>' + tickP99.toFixed(2) + ' ms</b>';

  let rows = '';
  for (const state of shards.values()) {
    const age = state.lastFrameAt ? (performance.now() - state.lastFrameAt) : 0;
    rows += '<div class="' + (state.live ? '' : 'down') + '">' +
            '<span class="dot" style="background:' + state.color + '"></span>' +
            (state.id || state.endpoint) + '  tick ' + state.tick +
            '  ' + state.players + 'p  ' + state.tickP99.toFixed(1) + 'ms' +
            (state.live ? '  +' + Math.round(age) + 'ms' : '  offline') + '</div>';
  }
  document.getElementById('shards').innerHTML = rows;

  requestAnimationFrame(draw);
}
requestAnimationFrame(draw);
</script>
</body>
</html>
)HTML";

    return std::string(kBefore) + default_ws + kAfter;
}

}  // namespace morton
