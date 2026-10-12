// DeskFeed 最小生产端示例（不依赖任何私有生产端、无第三方依赖）。
//
// 只实现协议要求的那一件事：对 GET /snapshot 返回一份合法的快照 JSON。
// 它还带几个开关，方便离线验证与验收脚本覆盖各种边界（见 examples/producer/README.md）。
//
//   node minimal-producer.mjs [--port 8099]
//
// 开关（查询参数）：
//   ?clear=1              返回 {"version":1,"items":[]}（合法清空，不是失败）
//   ?expired=1            返回带条目但 expiresAtMs 已过期的快照
//   ?no-expiry=1          不带 expiresAtMs
//   ?ttl-ms=<n>           本次快照的存活时间（默认 60000）
//   ?bad=1                返回一份结构非法（version=2）的正文
//   ?status=<code>        直接返回该状态码（例如 204），不带快照正文
//   ?slow-headers=<ms>    延迟写响应头（慢头部）
//   ?slow-chunk=<ms>      分块发送正文，每块之间等待（慢滴流）

import { createServer } from 'node:http';

const args = process.argv.slice(2);
let port = Number(process.env.PORT ?? 8099);
for (let index = 0; index < args.length; index += 1) {
  if (args[index] === '--port' && index + 1 < args.length) {
    port = Number(args[index + 1]);
  }
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, Number(ms) || 0));

function snapshot(now, queries) {
  const ttl = Number(queries.get('ttl-ms') ?? 60000);
  const expires = queries.get('expired') === '1' ? now - 1000 : now + ttl;
  const items = [
    { id: 'cpu', label: 'CPU', value: '54℃', state: 'ok', detail: '最近一次采样结果' },
    { id: 'disk', label: '磁盘', value: '剩余 68%', state: 'warn' },
  ];
  const body = { version: 1, items };
  if (queries.get('no-expiry') !== '1') {
    body.expiresAtMs = expires;
  }
  return body;
}

const server = createServer(async (request, response) => {
  const url = new URL(request.url, `http://${request.headers.host ?? '127.0.0.1'}`);
  const queries = url.searchParams;
  if (url.pathname !== '/snapshot') {
    response.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
    response.end('not found\n');
    return;
  }

  if (queries.has('status')) {
    response.writeHead(Number(queries.get('status')), { 'Content-Type': 'text/plain' });
    response.end();
    return;
  }
  if (queries.get('slow-headers')) {
    await sleep(queries.get('slow-headers'));
  }

  let text;
  if (queries.get('clear') === '1') {
    text = JSON.stringify({ version: 1, items: [] });
  } else if (queries.get('bad') === '1') {
    text = JSON.stringify({ version: 2, items: [] });
  } else {
    text = JSON.stringify(snapshot(Date.now(), queries));
  }

  response.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
  if (queries.get('slow-chunk')) {
    const chunk = Math.max(1, Math.ceil(text.length / 4));
    for (let offset = 0; offset < text.length; offset += chunk) {
      response.write(text.slice(offset, offset + chunk));
      await sleep(queries.get('slow-chunk'));
    }
    response.end();
    return;
  }
  response.end(text);
});

server.listen(port, '127.0.0.1', () => {
  console.log(`deskfeed example producer listening on http://127.0.0.1:${port}/snapshot`);
});
