#include "sha256.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

namespace grpc_qparse {
namespace {

// FIPS 180-4, section 4.2.2: the first 32 bits of the fractional parts of
// the cube roots of the first sixty-four primes.
constexpr std::array<uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu,
    0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr size_t kBlockBytes = 64;

uint32_t ReadBe32(const unsigned char* bytes) {
  return (static_cast<uint32_t>(bytes[0]) << 24) |
         (static_cast<uint32_t>(bytes[1]) << 16) |
         (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}

void WriteBe32(uint32_t value, unsigned char* out) {
  out[0] = static_cast<unsigned char>(value >> 24);
  out[1] = static_cast<unsigned char>(value >> 16);
  out[2] = static_cast<unsigned char>(value >> 8);
  out[3] = static_cast<unsigned char>(value);
}

// One compression round over a single 64-byte block, as FIPS 180-4 section
// 6.2.2 states it.
void Compress(std::array<uint32_t, 8>& state, const unsigned char* block) {
  std::array<uint32_t, 64> schedule{};
  for (size_t index = 0; index < 16; ++index) {
    schedule[index] = ReadBe32(block + index * 4);
  }
  for (size_t index = 16; index < 64; ++index) {
    const uint32_t previous = schedule[index - 15];
    const uint32_t recent = schedule[index - 2];
    const uint32_t sigma0 =
        std::rotr(previous, 7) ^ std::rotr(previous, 18) ^ (previous >> 3);
    const uint32_t sigma1 =
        std::rotr(recent, 17) ^ std::rotr(recent, 19) ^ (recent >> 10);
    schedule[index] =
        schedule[index - 16] + sigma0 + schedule[index - 7] + sigma1;
  }

  auto [a, b, c, d, e, f, g, h] = state;
  for (size_t index = 0; index < 64; ++index) {
    const uint32_t sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
    const uint32_t choice = (e & f) ^ (~e & g);
    const uint32_t temp1 =
        h + sum1 + choice + kRoundConstants[index] + schedule[index];
    const uint32_t sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = sum0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  const std::array<uint32_t, 8> round{a, b, c, d, e, f, g, h};
  for (size_t index = 0; index < state.size(); ++index) {
    state[index] += round[index];
  }
}

std::string Sha256(std::string_view data) {
  // FIPS 180-4, section 5.3.3: the first 32 bits of the fractional parts of
  // the square roots of the first eight primes.
  std::array<uint32_t, 8> state{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                                0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                                0x1f83d9abu, 0x5be0cd19u};

  const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
  size_t offset = 0;
  for (; offset + kBlockBytes <= data.size(); offset += kBlockBytes) {
    Compress(state, bytes + offset);
  }

  // The tail: the remainder, the 0x80 terminator, zero padding, and the
  // 64-bit big-endian bit length. Two blocks cover every case because the
  // remainder is under 64 bytes.
  std::array<unsigned char, kBlockBytes * 2> tail{};
  const size_t remainder = data.size() - offset;
  if (remainder > 0) std::memcpy(tail.data(), bytes + offset, remainder);
  tail[remainder] = 0x80;
  const size_t tail_blocks = remainder + 1 + 8 > kBlockBytes ? 2 : 1;
  const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
  unsigned char* length_field = tail.data() + tail_blocks * kBlockBytes - 8;
  WriteBe32(static_cast<uint32_t>(bits >> 32), length_field);
  WriteBe32(static_cast<uint32_t>(bits), length_field + 4);
  for (size_t block = 0; block < tail_blocks; ++block) {
    Compress(state, tail.data() + block * kBlockBytes);
  }

  std::string digest(32, '\0');
  for (size_t word = 0; word < state.size(); ++word) {
    WriteBe32(state[word],
              reinterpret_cast<unsigned char*>(digest.data()) + word * 4);
  }
  return digest;
}

}  // namespace

std::string Sha256Hex(std::string_view data) {
  static constexpr char kDigits[] = "0123456789abcdef";
  const std::string digest = Sha256(data);
  std::string hex;
  hex.reserve(digest.size() * 2);
  for (const char byte : digest) {
    const auto value = static_cast<unsigned char>(byte);
    hex.push_back(kDigits[value >> 4]);
    hex.push_back(kDigits[value & 0x0f]);
  }
  return hex;
}

}  // namespace grpc_qparse
