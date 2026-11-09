/**
 * Configurador do ESP32-S3 Stream Deck, via Web Serial API (Chrome/Edge).
 * Fala diretamente com o dispositivo pelo protocolo JSON-por-linha descrito
 * em src/SerialProtocol.h — sem instalar nada, sem servidor.
 */

const KEYS = Array.from({ length: 12 }, (_, i) => `F${i + 13}`); // F13..F24
const MAX_BUTTONS = 8;
const MIN_BUTTONS = 1;
const DEFAULT_COLOR = "#3366cc";
// Menor que o buffer de recepcao padrao (256 bytes) do USB CDC do ESP32: um
// bloco maior que isso pode estourar o buffer e perder bytes silenciosamente.
const CHUNK_SIZE = 512;
// Fundo da grade: deve bater com BG_WIDTH/BG_HEIGHT no firmware (paisagem).
const BG_WIDTH = 480;
const BG_HEIGHT = 320;
const UPLOAD_TIMEOUT_MS = 10000;
const ICON_SIZE = 64; // deve bater com Icon::SIZE no firmware

class ProtocolError extends Error {}

class DeckLink {
  constructor() {
    this.port = null;
    this.writer = null;
    this.reader = null;
    this.buffer = "";
    this.pending = []; // resolvers esperando a proxima linha
    this.backlog = []; // linhas que chegaram antes de alguem esperar por elas
  }

  async connect() {
    this.port = await navigator.serial.requestPort();
    await this.port.open({ baudRate: 115200 });
    this.writer = this.port.writable.getWriter();
    this._readLoop(); // roda em background ate a porta fechar
  }

  async _readLoop() {
    const textStream = this.port.readable.pipeThrough(new TextDecoderStream());
    this.reader = textStream.getReader();
    try {
      while (true) {
        const { value, done } = await this.reader.read();
        if (done) break;
        this.buffer += value;
        let idx;
        while ((idx = this.buffer.indexOf("\n")) >= 0) {
          const line = this.buffer.slice(0, idx).replace(/\r$/, "");
          this.buffer = this.buffer.slice(idx + 1);
          this._dispatchLine(line);
        }
      }
    } catch (err) {
      this._failPending(err);
    }
  }

  _dispatchLine(line) {
    if (!line) return;
    let parsed, parseError;
    try {
      parsed = JSON.parse(line);
    } catch (err) {
      parseError = new ProtocolError(`resposta invalida do dispositivo: ${line}`);
    }

    // Eventos espontaneos do dispositivo (ex: {"event":"status_request"} ao
    // tocar no botao de status) nao sao resposta a nenhum comando.
    if (parsed && parsed.event) return;

    const waiter = this.pending.shift();
    if (!waiter) {
      // Ninguem esperando ainda (ex: a confirmacao chegou antes do app
      // registrar o listener) — guarda pra o proximo _waitForLine() pegar.
      this.backlog.push(parseError ? { error: parseError } : { value: parsed });
      return;
    }
    if (parseError) waiter.reject(parseError);
    else waiter.resolve(parsed);
  }

  _failPending(err) {
    while (this.pending.length) this.pending.shift().reject(err);
  }

  _waitForLine(timeoutMs = 0) {
    if (this.backlog.length > 0) {
      const item = this.backlog.shift();
      return item.error ? Promise.reject(item.error) : Promise.resolve(item.value);
    }
    return new Promise((resolve, reject) => {
      const waiter = { resolve, reject };
      if (timeoutMs > 0) {
        const timer = setTimeout(() => {
          const i = this.pending.indexOf(waiter);
          if (i >= 0) this.pending.splice(i, 1);
          reject(new ProtocolError("o dispositivo parou de responder (timeout)"));
        }, timeoutMs);
        waiter.resolve = (v) => { clearTimeout(timer); resolve(v); };
        waiter.reject = (e) => { clearTimeout(timer); reject(e); };
      }
      this.pending.push(waiter);
    });
  }

  async _writeText(str) {
    await this.writer.write(new TextEncoder().encode(str));
  }

  async _command(obj) {
    const linePromise = this._waitForLine(UPLOAD_TIMEOUT_MS);
    await this._writeText(JSON.stringify(obj) + "\n");
    const resp = await linePromise;
    if (!resp.ok) throw new ProtocolError(resp.error || "erro desconhecido");
    return resp;
  }

  // Envia `bytes` em blocos pequenos, esperando confirmacao de cada um antes
  // de mandar o proximo (lockstep — nunca mais de um bloco "em voo", pra nao
  // estourar o buffer de recepcao do USB CDC do dispositivo). O comando que
  // inicia a transferencia (ex: put_gif/put_icon) deve ja ter sido enviado e
  // confirmado por quem chama isto.
  //
  // O firmware manda um ack {"received":N} (N cumulativo) a cada leitura da
  // serial, e uma leitura pode pegar menos bytes que um bloco inteiro — entao
  // pode haver mais de um ack por bloco. Por isso o bloco so conta como
  // confirmado quando received >= bytes enviados ate agora, e nao "um ack
  // por bloco".
  async _sendBytesLockstep(bytes, onProgress) {
    let sent = 0;
    while (sent < bytes.length) {
      const chunk = bytes.slice(sent, Math.min(sent + CHUNK_SIZE, bytes.length));
      await this.writer.write(chunk);
      sent += chunk.length;

      let ack;
      do {
        try {
          ack = await this._waitForLine(UPLOAD_TIMEOUT_MS);
        } catch (err) {
          throw new ProtocolError(`${err.message} apos ${sent}/${bytes.length} bytes enviados`);
        }
        if (!ack.ok) throw new ProtocolError(ack.error || "erro desconhecido no upload");
        if (ack.written !== undefined) break; // fim antecipado
      } while ((ack.received ?? sent) < sent);

      if (onProgress) onProgress(sent, bytes.length);
    }

    // Pula acks "received" atrasados ate a confirmacao final ("written").
    let final;
    do {
      final = await this._waitForLine(UPLOAD_TIMEOUT_MS);
      if (!final.ok) throw new ProtocolError(final.error || "erro desconhecido no upload");
    } while (final.written === undefined);
    return final;
  }

  ping() {
    return this._command({ cmd: "ping" });
  }

  async getConfig() {
    const resp = await this._command({ cmd: "get_config" });
    return resp.config;
  }

  setConfig(config) {
    return this._command({ cmd: "set_config", ...config });
  }

  deleteGif() {
    return this._command({ cmd: "delete_gif" });
  }

  reboot() {
    return this._command({ cmd: "reboot" });
  }

  async putGif(file, onProgress) {
    const buf = new Uint8Array(await file.arrayBuffer());
    await this._command({ cmd: "put_gif", size: buf.length });
    return this._sendBytesLockstep(buf, onProgress);
  }

  async putBg(bytes, onProgress) {
    await this._command({ cmd: "put_bg", size: bytes.length });
    return this._sendBytesLockstep(bytes, onProgress);
  }

  deleteBg() {
    return this._command({ cmd: "delete_bg" });
  }

  async putIcon(index, rgbaBytes, onProgress) {
    await this._command({ cmd: "put_icon", index, size: rgbaBytes.length });
    return this._sendBytesLockstep(rgbaBytes, onProgress);
  }

  deleteIcon(index) {
    return this._command({ cmd: "delete_icon", index });
  }

  async close() {
    try { await this.reader?.cancel(); } catch {}
    try { this.writer?.releaseLock(); } catch {}
    try { await this.port?.close(); } catch {}
  }
}

// ---------------------------------------------------------------------
// imagem -> RGBA8888 cru (ICON_SIZE x ICON_SIZE), "cover fit" centralizado
// ---------------------------------------------------------------------

// Usa <img> em vez de createImageBitmap(): lida melhor com SVGs que so tem
// viewBox (sem width/height explicitos), que fazem createImageBitmap falhar
// com "The image could not be decoded" em varios navegadores.
function loadImageElement(url) {
  return new Promise((resolve, reject) => {
    const img = new Image();
    img.onload = () => resolve(img);
    img.onerror = () => reject(new ProtocolError("nao foi possivel carregar essa imagem (formato nao suportado?)"));
    img.src = url;
  });
}

// SVGs sem width/height no elemento raiz (so com viewBox) as vezes nao tem
// tamanho intrinseco nenhum quando carregados via <img> — ler o viewBox e
// injetar width/height explicitos preserva a proporcao real (sem isso, um
// fallback quadrado faria logos retangulares ficarem distorcidos/"tortos").
async function fileToImageElement(file) {
  const isSvg = file.type === "image/svg+xml" || /\.svg$/i.test(file.name);
  if (!isSvg) {
    const url = URL.createObjectURL(file);
    try {
      return await loadImageElement(url);
    } finally {
      URL.revokeObjectURL(url);
    }
  }

  let text = await file.text();
  if (!/<svg[^>]*\swidth=/.test(text)) {
    const m = text.match(/viewBox=["']\s*[\d.\-]+\s+[\d.\-]+\s+([\d.\-]+)\s+([\d.\-]+)\s*["']/);
    if (m) {
      text = text.replace(/<svg/, `<svg width="${m[1]}" height="${m[2]}"`);
    }
  }
  const blobUrl = URL.createObjectURL(new Blob([text], { type: "image/svg+xml" }));
  try {
    return await loadImageElement(blobUrl);
  } finally {
    URL.revokeObjectURL(blobUrl);
  }
}

// Imagem -> RGB565 little-endian (formato nativo do ESP32), BG_WIDTH x
// BG_HEIGHT, recortada ("cover") e centralizada. Sem alpha: fundo opaco.
async function imageFileToRGB565(file) {
  const img = await fileToImageElement(file);
  const canvas = document.createElement("canvas");
  canvas.width = BG_WIDTH;
  canvas.height = BG_HEIGHT;
  const ctx = canvas.getContext("2d");

  const iw = img.naturalWidth || BG_WIDTH;
  const ih = img.naturalHeight || BG_HEIGHT;
  const scale = Math.max(BG_WIDTH / iw, BG_HEIGHT / ih);
  const w = iw * scale;
  const h = ih * scale;
  ctx.fillStyle = "#000";
  ctx.fillRect(0, 0, BG_WIDTH, BG_HEIGHT);
  ctx.drawImage(img, (BG_WIDTH - w) / 2, (BG_HEIGHT - h) / 2, w, h);

  const px = ctx.getImageData(0, 0, BG_WIDTH, BG_HEIGHT).data;
  const out = new Uint8Array(BG_WIDTH * BG_HEIGHT * 2);
  for (let i = 0, o = 0; i < px.length; i += 4, o += 2) {
    const v = ((px[i] & 0xF8) << 8) | ((px[i + 1] & 0xFC) << 3) | (px[i + 2] >> 3);
    out[o] = v & 0xFF;
    out[o + 1] = v >> 8;
  }
  return out;
}

async function imageFileToRGBA(file, size) {
  const img = await fileToImageElement(file);
  const canvas = document.createElement("canvas");
  canvas.width = size;
  canvas.height = size;
  const ctx = canvas.getContext("2d");

  const iw = img.naturalWidth || size;
  const ih = img.naturalHeight || size;
  const scale = Math.max(size / iw, size / ih);
  const w = iw * scale;
  const h = ih * scale;
  ctx.clearRect(0, 0, size, size);
  ctx.drawImage(img, (size - w) / 2, (size - h) / 2, w, h);

  const imageData = ctx.getImageData(0, 0, size, size);
  return new Uint8Array(imageData.data); // ja esta em RGBA, linha por linha
}

// ---------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------

let link = null;
let selectedGifFile = null;

const el = (id) => document.getElementById(id);
const rowsEl = el("rows");
const rowTemplate = el("rowTemplate");
const logEl = el("log");
const statusEl = el("status");

function log(msg) {
  const time = new Date().toLocaleTimeString();
  logEl.textContent += `[${time}] ${msg}\n`;
  logEl.scrollTop = logEl.scrollHeight;
}

function setStatus(text, cls) {
  statusEl.textContent = text;
  statusEl.className = `status ${cls || ""}`.trim();
}

function updateTypeVisibility(node) {
  const type = node.querySelector(".type").value;
  node.querySelector(".type-key").classList.toggle("hidden", type !== "key");
  node.querySelector(".type-macro").classList.toggle("hidden", type !== "macro");
}

function addRow(data) {
  if (rowsEl.children.length >= MAX_BUTTONS) {
    alert(`Maximo de ${MAX_BUTTONS} botoes.`);
    return;
  }
  const node = rowTemplate.content.firstElementChild.cloneNode(true);
  const keySelect = node.querySelector(".key");
  for (const k of KEYS) {
    const opt = document.createElement("option");
    opt.value = k;
    opt.textContent = k;
    keySelect.appendChild(opt);
  }

  node.querySelector(".label").value = data?.label || "";
  node.querySelector(".type").value = data?.type || "key";
  keySelect.value = KEYS.includes(data?.key) ? data.key : KEYS[0];
  node.querySelector(".macro-text").value = data?.macro_text || "";
  node.querySelector(".macro-winr").checked = !!data?.macro_winr;
  node.querySelector(".macro-enter").checked = !!data?.macro_enter;
  node.querySelector(".color").value = data?.color || DEFAULT_COLOR;

  const iconStatus = node.querySelector(".icon-status");
  iconStatus.textContent = data?.has_icon ? "icone gravado no dispositivo" : "sem icone";

  node.querySelector(".type").addEventListener("change", () => updateTypeVisibility(node));
  updateTypeVisibility(node);

  node.querySelector(".remove").addEventListener("click", () => {
    if (rowsEl.children.length <= MIN_BUTTONS) {
      alert("Pelo menos um botao e necessario.");
      return;
    }
    node.remove();
  });

  node.querySelector(".icon-input").addEventListener("change", (e) => {
    const file = e.target.files[0];
    if (!file) return;
    handleAsync(async () => {
      const preview = node.querySelector(".icon-preview");
      preview.src = URL.createObjectURL(file);
      preview.classList.remove("hidden");

      const l = await requireLink();
      iconStatus.textContent = "enviando...";
      const rgba = await imageFileToRGBA(file, ICON_SIZE);
      const index = Array.from(rowsEl.children).indexOf(node);
      try {
        await l.putIcon(index, rgba);
      } catch (err) {
        iconStatus.textContent = "falhou";
        throw err;
      }
      iconStatus.textContent = "icone enviado";
      log(`Icone do botao ${index + 1} enviado.`);
    });
  });

  node.querySelector(".icon-remove").addEventListener("click", () => handleAsync(async () => {
    const l = await requireLink();
    const index = Array.from(rowsEl.children).indexOf(node);
    await l.deleteIcon(index);
    node.querySelector(".icon-preview").classList.add("hidden");
    iconStatus.textContent = "sem icone";
    log(`Icone do botao ${index + 1} removido.`);
  }));

  rowsEl.appendChild(node);
}

function readRows() {
  return Array.from(rowsEl.children).map((node) => {
    const type = node.querySelector(".type").value;
    const key = node.querySelector(".key").value;
    const label = node.querySelector(".label").value.trim() || key;
    return {
      label,
      type,
      key,
      macro_text: node.querySelector(".macro-text").value,
      macro_winr: node.querySelector(".macro-winr").checked,
      macro_enter: node.querySelector(".macro-enter").checked,
      color: node.querySelector(".color").value,
    };
  });
}

function applyConfigToUI(cfg) {
  rowsEl.innerHTML = "";
  for (const b of cfg.buttons || []) addRow(b);
  el("idleSeconds").value = Math.round((cfg.idle_timeout_ms ?? 30000) / 1000);
  el("boxOpacity").value = cfg.box_opacity ?? 70;
  el("boxOpacityValue").textContent = `${el("boxOpacity").value}%`;
  log("Config carregada do dispositivo.");
}

async function requireLink() {
  if (!link) {
    alert("Conecte-se ao dispositivo primeiro.");
    throw new ProtocolError("desconectado");
  }
  return link;
}

async function handleAsync(fn) {
  try {
    await fn();
  } catch (err) {
    log(`Erro: ${err.message || err}`);
    if (!(err instanceof ProtocolError)) console.error(err);
  }
}

el("connectBtn").addEventListener("click", () => handleAsync(async () => {
  if (link) {
    await link.close();
    link = null;
    setStatus("desconectado");
    el("connectBtn").textContent = "Conectar";
    return;
  }

  const newLink = new DeckLink();
  await newLink.connect();
  const info = await newLink.ping();
  link = newLink;
  setStatus(`conectado, fw ${info.fw}`, "connected");
  el("connectBtn").textContent = "Desconectar";
  log(`Conectado: ${JSON.stringify(info)}`);
}));

el("addRowBtn").addEventListener("click", () => addRow());

el("loadConfigBtn").addEventListener("click", () => handleAsync(async () => {
  const l = await requireLink();
  const cfg = await l.getConfig();
  applyConfigToUI(cfg);
}));

el("saveConfigBtn").addEventListener("click", () => handleAsync(async () => {
  const l = await requireLink();
  const config = {
    buttons: readRows(),
    idle_timeout_ms: Number(el("idleSeconds").value) * 1000,
    box_opacity: Number(el("boxOpacity").value),
  };
  await l.setConfig(config);
  log("Config salva no dispositivo.");
}));

el("gifInput").addEventListener("change", (e) => {
  const file = e.target.files[0];
  selectedGifFile = file || null;
  el("gifInfo").textContent = file
    ? `${file.name} (${Math.round(file.size / 1024)} KB)`
    : "nenhum arquivo selecionado";
});

el("sendGifBtn").addEventListener("click", () => handleAsync(async () => {
  if (!selectedGifFile) {
    alert("Escolha um arquivo GIF primeiro.");
    return;
  }
  const l = await requireLink();
  const progress = el("gifProgress");
  progress.value = 0;
  const result = await l.putGif(selectedGifFile, (sent, total) => {
    progress.value = Math.round((sent / total) * 100);
  });
  log(`GIF enviado (${result.written} bytes).`);
}));

el("deleteGifBtn").addEventListener("click", () => handleAsync(async () => {
  const l = await requireLink();
  await l.deleteGif();
  log("Tela de descanso removida.");
}));

el("boxOpacity").addEventListener("input", () => {
  el("boxOpacityValue").textContent = `${el("boxOpacity").value}%`;
});

let selectedBgFile = null;

el("bgInput").addEventListener("change", (e) => {
  const file = e.target.files[0];
  selectedBgFile = file || null;
  const preview = el("bgPreview");
  if (file) {
    preview.src = URL.createObjectURL(file);
    preview.classList.remove("hidden");
  } else {
    preview.classList.add("hidden");
  }
});

el("sendBgBtn").addEventListener("click", () => handleAsync(async () => {
  if (!selectedBgFile) {
    alert("Escolha uma imagem primeiro.");
    return;
  }
  const l = await requireLink();
  const progress = el("bgProgress");
  progress.value = 0;
  log("Convertendo imagem de fundo...");
  const bytes = await imageFileToRGB565(selectedBgFile);
  const result = await l.putBg(bytes, (sent, total) => {
    progress.value = Math.round((sent / total) * 100);
  });
  log(`Fundo enviado (${result.written} bytes).`);
}));

el("deleteBgBtn").addEventListener("click", () => handleAsync(async () => {
  const l = await requireLink();
  await l.deleteBg();
  log("Fundo removido.");
}));

el("rebootBtn").addEventListener("click", () => handleAsync(async () => {
  const l = await requireLink();
  await l.reboot();
  log("Comando de reinicio enviado.");
}));

// ---------------------------------------------------------------------
// inicializacao
// ---------------------------------------------------------------------

if (!("serial" in navigator)) {
  el("unsupported").classList.remove("hidden");
  for (const btn of document.querySelectorAll("button")) btn.disabled = true;
} else {
  for (let i = 0; i < 6; i++) addRow();
}
