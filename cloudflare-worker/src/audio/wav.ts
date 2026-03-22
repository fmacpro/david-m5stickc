export function downsampleWav16MonoTo8bitRate(
  input: Uint8Array,
  targetRate = 8000
): Uint8Array {
  if (input.byteLength < 44) return input;
  if (readAscii(input, 0, 4) !== "RIFF" || readAscii(input, 8, 4) !== "WAVE") {
    return input;
  }

  let fmtOffset = -1;
  let fmtLen = 0;
  let dataOffset = -1;
  let dataLen = 0;
  let off = 12;
  while (off + 8 <= input.byteLength) {
    const id = readAscii(input, off, 4);
    const lenRaw = readLe32(input, off + 4);
    const chunkData = off + 8;
    const len =
      lenRaw === 0xffffffff || chunkData + lenRaw > input.byteLength
        ? Math.max(0, input.byteLength - chunkData)
        : lenRaw;
    if (id === "fmt " && fmtOffset < 0) {
      fmtOffset = chunkData;
      fmtLen = len;
    } else if (id === "data" && dataOffset < 0) {
      dataOffset = chunkData;
      dataLen = len;
    }
    // Chunks are word-aligned.
    off = chunkData + len + (len & 1);
  }
  if (fmtOffset < 0 || dataOffset < 0) return input;
  if (fmtLen < 16 || dataLen <= 0) return input;

  const audioFormat = readLe16(input, fmtOffset + 0);
  const channels = readLe16(input, fmtOffset + 2);
  const sampleRate = readLe32(input, fmtOffset + 4);
  const bitsPerSample = readLe16(input, fmtOffset + 14);
  if (audioFormat !== 1) return input; // PCM only
  if (channels !== 1 || bitsPerSample !== 16 || sampleRate <= 0) return input;
  if (dataOffset + dataLen > input.byteLength) return input;

  const sourceSamples = dataLen / 2;
  const outSamples = Math.max(1, Math.floor((sourceSamples * targetRate) / sampleRate));
  const outDataLen = outSamples;
  const out = new Uint8Array(44 + outDataLen);

  out.set(input.subarray(0, 44), 0);
  writeLe32(out, 4, 36 + outDataLen);
  writeLe32(out, 24, targetRate);
  writeLe32(out, 28, targetRate);
  writeLe16(out, 32, 1);
  writeLe16(out, 34, 8);
  writeLe32(out, 40, outDataLen);

  for (let j = 0; j < outSamples; j += 1) {
    const srcIndex = Math.floor((j * sampleRate) / targetRate);
    const srcByte = dataOffset + srcIndex * 2;
    const sample = (input[srcByte] | (input[srcByte + 1] << 8)) << 16 >> 16;
    const u8 = Math.max(0, Math.min(255, (sample >> 8) + 128));
    out[44 + j] = u8;
  }
  return out;
}

export function uint8ToArrayBuffer(bytes: Uint8Array): ArrayBuffer {
  const out = new ArrayBuffer(bytes.byteLength);
  new Uint8Array(out).set(bytes);
  return out;
}

function readAscii(buf: Uint8Array, off: number, len: number): string {
  return new TextDecoder().decode(buf.subarray(off, off + len));
}

function readLe16(buf: Uint8Array, off: number): number {
  return buf[off] | (buf[off + 1] << 8);
}

function readLe32(buf: Uint8Array, off: number): number {
  return (
    buf[off] |
    (buf[off + 1] << 8) |
    (buf[off + 2] << 16) |
    (buf[off + 3] << 24)
  ) >>> 0;
}

function writeLe16(buf: Uint8Array, off: number, v: number): void {
  buf[off] = v & 0xff;
  buf[off + 1] = (v >> 8) & 0xff;
}

function writeLe32(buf: Uint8Array, off: number, v: number): void {
  buf[off] = v & 0xff;
  buf[off + 1] = (v >> 8) & 0xff;
  buf[off + 2] = (v >> 16) & 0xff;
  buf[off + 3] = (v >> 24) & 0xff;
}
