#pragma once
// ============================================================================
//  BRMesh / FastCon broadcast protocol codec
//
//  The bulbs are not a real BLE mesh: each command is a single BLE broadcast
//  (advertising) frame. A 16-byte "FastCon body" is built, encrypted against a
//  4-byte mesh key, wrapped in an RF payload (CRC16 + custom bit-reversal),
//  then "whitened" with an LFSR before the first 15 bytes (which are
//  protocol preamble) are stripped. What remains is the manufacturer-specific
//  BLE advertising data the bulbs listen for.
//
//  This module is pure (no BLE / WiFi / HA), so it is trivially testable.
// ============================================================================
#include <cstdint>
#include <string>
#include <vector>

namespace protocol {

// Factory / default mesh key the bulbs ship with. Until we write our own key,
// every bulb answers under this one. (hex 5e 36 7b c4)
static const uint8_t DEFAULT_KEY[4] = { 0x5e, 0x36, 0x7b, 0xc4 };

// Broadcast (FastCon) adapter address - a fixed 3-byte group address.
static const uint8_t FASTCON_ADDR[3] = { 0xC1, 0xC2, 0xC3 };

// Command type ids (encoded in the high nibble of the body header byte).
enum Command : uint8_t {
  WAKE    = 0,  // broadcast "alive/wake" - bulbs respond with their identity
  SET_KEY = 2,  // write a new mesh key + light number onto a bulb
  CONTROL = 5,  // control: on/off, brightness, color temperature, RGB
};

// Build the final RF payload (whitened, 15 preamble bytes stripped) for a
// command.
//
//   type    WAKE / SET_KEY / CONTROL
//   data    the command body (6B wake, 12B set-key, 8B control)
//   key     the 4-byte mesh key to encrypt under (all-zeros => the factory
//           key is injected, used for WAKE)
//   forward direction bit (control = true, wake/set-key = false)
//   i2      light-id high byte (0 here; ids are < 256)
std::vector<uint8_t> generate_command(uint8_t type,
                                      const std::vector<uint8_t> &data,
                                      const uint8_t key[4],
                                      bool forward,
                                      int i2 = 0);

// Wrap an RF payload in the BLE manufacturer-specific advertising structure
// (length byte + 0xFF type + 0xFFF0 company id + payload).
std::string to_adv_data(const std::vector<uint8_t> &rf);

}  // namespace protocol
