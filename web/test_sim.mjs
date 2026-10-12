// SPDX-License-Identifier: GPL-3.0-only
// The simulator without a browser: node web/test_sim.mjs [out-prefix]
// Loads sim/fm2000.wasm, plays a few seconds with the panel driven as the page would, and writes the screen
// (PPM, at the times SHOTS asks) and the sound (WAV). Fails on a dead screen or silence.
import { readFileSync, writeFileSync } from "node:fs";

const out = process.argv[2] || "build/sim/sim";
const { instance } = await WebAssembly.instantiate(readFileSync(new URL("../sim/fm2000.wasm", import.meta.url)), {});
const ex = instance.exports;
const BTN = { PLAY: 12, OCT_UP: 1 }, ENC = { SELECT: 0, ALGORITHM: 1, PRESETS: 6, KNOB1: 2 };
const RATE = ex.web_sample_rate(), Q = 128;

ex.web_init();
let ms = 0, pcm = [];
const run = (msTo) => {                       // render in quanta of 128 frames, as the worklet does
  while (ex.web_now_ms() < msTo) {
    ex.web_render(Q);
    const l = new Float32Array(ex.memory.buffer, ex.web_out_l(), Q), r = new Float32Array(ex.memory.buffer, ex.web_out_r(), Q);
    for (let i = 0; i < Q; i++) pcm.push(l[i], r[i]);
  }
  ms = msTo;
};
const tap = (bit) => { ex.web_buttons(1 << bit); run(ms + 30); ex.web_buttons(0); run(ms + 30); };
const shot = (name) => {
  const fb = new Uint16Array(ex.memory.buffer, ex.web_screen(), 240 * 240);
  const px = Buffer.alloc(240 * 240 * 3);
  for (let i = 0; i < fb.length; i++) {
    const c = fb[i];
    px[i * 3] = ((c >> 11) & 31) * 255 / 31; px[i * 3 + 1] = ((c >> 5) & 63) * 255 / 63; px[i * 3 + 2] = (c & 31) * 255 / 31;
  }
  writeFileSync(`${out}-${name}.ppm`, Buffer.concat([Buffer.from("P6 240 240 255\n"), px]));
  let lit = 0;
  for (const c of fb) if (c) lit++;
  return lit;
};

run(200);
const lit0 = shot("0-arrange");
ex.web_keys(1 << 7); run(700); ex.web_keys(0); run(900);          // a note on track 1
ex.web_enc(ENC.SELECT, 8); ex.web_enc(ENC.ALGORITHM, 2); run(1000);
shot("1-cursor");
ex.web_enc(ENC.SELECT, -8); ex.web_enc(ENC.ALGORITHM, -2); run(1100);
tap(BTN.PLAY);
run(1100 + 9000);                             // ~5 bars at 140 BPM
const lit1 = shot("2-playing");
tap(BTN.PLAY);
run(ms + 500);

let peak = 0, sum = 0;
for (const v of pcm) { peak = Math.max(peak, Math.abs(v)); sum += v * v; }
const rms = Math.sqrt(sum / pcm.length);
const wav = Buffer.alloc(44 + pcm.length * 2);
wav.write("RIFF", 0); wav.writeUInt32LE(36 + pcm.length * 2, 4); wav.write("WAVEfmt ", 8);
wav.writeUInt32LE(16, 16); wav.writeUInt16LE(1, 20); wav.writeUInt16LE(2, 22); wav.writeUInt32LE(RATE, 24);
wav.writeUInt32LE(RATE * 4, 28); wav.writeUInt16LE(4, 32); wav.writeUInt16LE(16, 34); wav.write("data", 36);
wav.writeUInt32LE(pcm.length * 2, 40);
pcm.forEach((v, i) => wav.writeInt16LE(Math.round(Math.max(-1, Math.min(1, v)) * 32767), 44 + i * 2));
writeFileSync(`${out}.wav`, wav);
console.log(`sim: ${ms} ms, screen lit ${lit0} / ${lit1} px, sound peak ${peak.toFixed(3)} rms ${rms.toFixed(4)}`);
if (lit0 < 2000 || lit1 < 2000 || rms < 0.01) { console.log("sim: FAIL"); process.exit(1); }
