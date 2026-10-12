// SPDX-License-Identifier: GPL-3.0-only
// FM2000 in an AudioWorklet: fm2000.wasm is the whole firmware against a simulated FM-1 (web/platform_web.c).
// Each render quantum asks it for 128 frames, which runs the device's clock forward; between quanta the page's
// input goes in and, about 30 times a second, the screen, the lights and the settings go out.
// After the browser builds of X0X (charlesvestal/fm1-x0x) and Felucca (hugelton/Felucca, web/emu), GPL-3.0.

const HOLD_MS = 24;     // a button or key change stays at least this long (device time) before the next
// the store (platform_web.c web_nor): only the sectors that hold something go to the page, {offset: bytes}
function storeSectors(nor) {
  const out = {};
  for (let off = 0; off < nor.length; off += 4096) {
    const s = nor.subarray(off, off + 4096);
    if (s.some((v) => v !== 0xFF)) out[off] = s.slice();
  }
  return out;
}

class FM2000 extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ex = null;
    this.sent = { draws: -1, leds: "", nv: -1, store: -1 };
    this.lastFrame = 0;
    this.period = 1 / 30;
    this.queue = [];
    this.nextOk = 0;
    this.port.onmessage = (e) => this.onMessage(e.data);
  }

  async onMessage(m) {
    if (m.type === "load") {
      try {
        const { instance } = await WebAssembly.instantiate(m.wasm, {});
        const ex = instance.exports;
        if (m.nv && m.nv.length === ex.web_nv_size()) new Uint8Array(ex.memory.buffer, ex.web_nv(), m.nv.length).set(m.nv);
        ex.web_store_erase_all();
        const nor = new Uint8Array(ex.memory.buffer, ex.web_store(), ex.web_store_size());
        for (const [k, v] of Object.entries(m.store || {}))
          if (+k % 4096 === 0 && +k + 4096 <= nor.length && v.length === 4096) nor.set(v, +k);
        ex.web_master(m.master ?? 700);
        ex.web_init();
        this.ex = ex;
        this.sent.nv = ex.web_nv_writes();
        this.sent.store = ex.web_store_writes();
        this.port.postMessage({ type: "ready", rate: ex.web_sample_rate() });
      } catch (err) {
        this.port.postMessage({ type: "error", message: String((err && err.message) || err) });
      }
      return;
    }
    const ex = this.ex;
    if (!ex) return;
    if (m.type === "buttons" || m.type === "keys") this.queue.push(m);
    else if (m.type === "enc") ex.web_enc(m.role, m.n | 0);
    else if (m.type === "master") ex.web_master(m.value | 0);
    else if (m.type === "midi") ex.web_midi(m.data[0] | 0, m.data[1] | 0, m.data[2] | 0);
    else if (m.type === "visible") this.period = m.on ? 1 / 30 : 1 / 4;
  }

  input() {
    const ex = this.ex, now = ex.web_now_ms();
    while (this.queue.length && now - this.nextOk >= 0) {
      const m = this.queue.shift();
      if (m.type === "buttons") ex.web_buttons(m.mask >>> 0);
      else ex.web_keys(m.mask >>> 0);
      this.nextOk = now + HOLD_MS;
    }
  }

  publish() {
    const ex = this.ex, mem = ex.memory.buffer, msg = { type: "frame" }, transfer = [];
    const leds = [ex.web_lit_buttons() >>> 0, ex.web_lit_keys() >>> 0], key = leds.join(",");
    if (key !== this.sent.leds) { this.sent.leds = key; msg.leds = leds; }
    const draws = ex.web_screen_draws();
    if (draws !== this.sent.draws) {
      this.sent.draws = draws;
      msg.fb = new Uint16Array(mem, ex.web_screen(), 240 * 240).slice();
      transfer.push(msg.fb.buffer);
    }
    const nv = ex.web_nv_writes();
    if (nv !== this.sent.nv) {
      this.sent.nv = nv;
      msg.nv = new Uint8Array(mem, ex.web_nv(), ex.web_nv_size()).slice();
    }
    const sw = ex.web_store_writes();
    if (sw !== this.sent.store) {                 // a save is done within one main-loop pass: whole here
      this.sent.store = sw;
      msg.store = storeSectors(new Uint8Array(mem, ex.web_store(), ex.web_store_size()));
    }
    if (msg.leds || msg.fb || msg.nv || msg.store) this.port.postMessage(msg, transfer);
  }

  process(inputs, outputs) {
    const ex = this.ex;
    if (!ex) return true;
    if (this.queue.length) this.input();
    const out = outputs[0], n = out[0].length;
    ex.web_render(n);
    out[0].set(new Float32Array(ex.memory.buffer, ex.web_out_l(), n));
    if (out[1]) out[1].set(new Float32Array(ex.memory.buffer, ex.web_out_r(), n));
    if (currentTime - this.lastFrame >= this.period) {
      this.lastFrame = currentTime;
      this.publish();
    }
    return true;
  }
}
registerProcessor("fm2000", FM2000);
