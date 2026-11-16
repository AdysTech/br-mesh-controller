#include "protocol.h"
#include <cstring>

namespace {

// ---- bit reversal ----------------------------------------------------------
uint8_t reverse_8(uint8_t d) {
  uint8_t r = 0;
  for (uint8_t k = 0; k < 8; k++) r |= ((d >> k) & 1) << (7 - k);
  return r;
}

uint16_t reverse_16(uint16_t d) {
  uint16_t r = 0;
  for (uint8_t k = 0; k < 16; k++) r |= ((d >> k) & 1) << (15 - k);
  return r;
}

// ---- CRC16/CCITT-ish with per-byte bit reversal (as reverse-engineered) ----
uint16_t crc16(const uint8_t *addr, int addrLen, const uint8_t *data, uint8_t dataLen) {
  uint16_t crc = 0xffff;
  for (int8_t i = addrLen - 1; i >= 0; i--) {
    crc ^= addr[i] << 8;
    for (int ii = 0; ii < 4; ii++) {
      uint16_t tmp = crc << 1;
      if ((crc & 0x8000) != 0) tmp ^= 0x1021;
      crc = tmp << 1;
      if ((tmp & 0x8000) != 0) crc ^= 0x1021;
    }
  }
  for (uint8_t i = 0; i < dataLen; i++) {
    crc ^= reverse_8(data[i]) << 8;
    for (int ii = 0; ii < 4; ii++) {
      uint16_t tmp = crc << 1;
      if ((crc & 0x8000) != 0) tmp ^= 0x1021;
      crc = tmp << 1;
      if ((tmp & 0x8000) != 0) crc ^= 0x1021;
    }
  }
  return (~reverse_16(crc)) & 0xffff;
}

// ---- LFSR "whitening" (seed 0x25) ----------------------------------------
void whitening_init(uint8_t val, uint8_t *ctx) {
  ctx[0] = 1;
  ctx[1] = (val >> 5) & 1;
  ctx[2] = (val >> 4) & 1;
  ctx[3] = (val >> 3) & 1;
  ctx[4] = (val >> 2) & 1;
  ctx[5] = (val >> 1) & 1;
  ctx[6] = val & 1;
}

void whitening_encode(const uint8_t *data, int len, uint8_t *ctx, uint8_t *out) {
  memcpy(out, data, len);
  for (int i = 0; i < len; i++) {
    int ctx3 = ctx[3], ctx5 = ctx[5], ctx6 = ctx[6], ctx4 = ctx[4];
    int ctx52 = ctx5 ^ ctx[2];
    int ctx41 = ctx4 ^ ctx[1];
    int ctx63 = ctx6 ^ ctx3;
    int ctx630 = ctx63 ^ ctx[0];

    int c = out[i];
    out[i] = ((c & 0x80) ^ ((ctx52 ^ ctx6) << 7))
          + ((c & 0x40) ^ (ctx630 << 6))
          + ((c & 0x20) ^ (ctx41 << 5))
          + ((c & 0x10) ^ (ctx52 << 4))
          + ((c & 0x08) ^ (ctx63 << 3))
          + ((c & 0x04) ^ (ctx4 << 2))
          + ((c & 0x02) ^ (ctx5 << 1))
          + ((c & 0x01) ^ (ctx6 << 0));

    ctx[2] = ctx41;
    ctx[3] = ctx52;
    ctx[4] = ctx52 ^ ctx3;
    ctx[5] = ctx630 ^ ctx4;
    ctx[6] = ctx41 ^ ctx5;
    ctx[0] = ctx52 ^ ctx6;
    ctx[1] = ctx630;
  }
}

// ---- 16-byte FastCon body (header + 12 data bytes), encrypted -------------
std::vector<uint8_t> build_body(int type, int i2, uint8_t seq, uint8_t safeKey,
                                bool forward, const std::vector<uint8_t> &data,
                                const uint8_t key[4]) {
  int len = data.size();
  std::vector<uint8_t> p(16, 0);
  p[0] = (i2 & 0b1111) | ((type & 0b111) << 4) | (forward ? 0x80 : 0);
  p[1] = seq & 0xff;
  p[2] = safeKey;
  p[3] = 0;  // checksum, filled below
  memcpy(p.data() + 4, data.data(), len);

  uint8_t sum = 0;
  for (int j = 0; j < len + 4; j++) {
    if (j == 3) continue;
    sum = (sum + p[j]) & 0xff;
  }
  p[3] = sum;

  // header (first 4 bytes) always XORed with the factory key; data with the key
  for (int j = 0; j < 4; j++) p[j] = protocol::DEFAULT_KEY[j & 3] ^ p[j];
  for (int j = 0; j < 12; j++) p[4 + j] = key[j & 3] ^ p[4 + j];
  return p;
}

// ---- wrap the 16-byte body in the RF payload (addr + data + CRC) -----------
std::vector<uint8_t> build_rf(const std::vector<uint8_t> &body) {
  const uint8_t *addr = protocol::FASTCON_ADDR;
  const int addrLen = 3;
  const int dataLen = body.size();  // 16
  const int dataOff = 0x12;
  const int invOff = 0x0f;
  const int size = dataOff + addrLen + dataLen + 2;

  std::vector<uint8_t> b(size, 0);
  b[0x0f] = 0x71;
  b[0x10] = 0x0f;
  b[0x11] = 0x55;

  for (int j = 0; j < addrLen; j++) b[dataOff + addrLen - j - 1] = addr[j];
  for (int j = 0; j < dataLen; j++) b[dataOff + addrLen + j] = body[j];

  for (int i = invOff; i < invOff + addrLen + 3; i++) b[i] = reverse_8(b[i]);

  uint16_t crc = crc16(addr, addrLen, body.data(), dataLen);
  b[size - 2] = crc & 0xff;
  b[size - 1] = (crc >> 8) & 0xff;
  return b;
}

}  // namespace

namespace protocol {

static uint8_t s_seq = 1;  // monotonically increasing sequence counter

std::vector<uint8_t> generate_command(uint8_t type, const std::vector<uint8_t> &data,
                                      const uint8_t key[4], bool forward, int i2) {
  s_seq = (s_seq + 1) & 0xff;
  bool hasKey = false;
  for (int i = 0; i < 4; i++) if (key[i] != 0) { hasKey = true; break; }
  uint8_t safeKey = hasKey ? key[3] : 0xff;

  std::vector<uint8_t> body = build_body(type, i2, s_seq, safeKey, forward, data, key);
  if (!hasKey) {
    // No real key (WAKE): the data region is filled with the factory key.
    for (int i = 4; i < 16; i++) body[i] = DEFAULT_KEY[i & 3];
  }

  std::vector<uint8_t> rf = build_rf(body);
  uint8_t ctx[7];
  whitening_init(0x25, ctx);
  std::vector<uint8_t> out(rf.size());
  whitening_encode(rf.data(), rf.size(), ctx, out.data());

  out.erase(out.begin(), out.begin() + 15);  // drop the 15-byte preamble
  return out;
}

std::string to_adv_data(const std::vector<uint8_t> &rf) {
  std::string s;
  s += (char)(rf.size() + 3);  // AD length = payload + 3 (type + company id)
  s += (char)0xFF;             // AD type: manufacturer specific
  s += (char)0xF0;
  s += (char)0xFF;             // company id 0xFFF0 (little-endian)
  for (auto b : rf) s += (char)b;
  return s;
}

}  // namespace protocol
