#include "document_cache.h"

#include <cstdlib>

namespace grpc_qparse {
namespace {

uint64_t EnvOr(const char* name, uint64_t fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return fallback;
  char* end = nullptr;
  uint64_t parsed = std::strtoull(value, &end, 10);
  if (end == value || *end != '\0') return fallback;
  return parsed;
}

}  // namespace

DocumentCacheConfig DocumentCacheConfigFromEnv() {
  DocumentCacheConfig config;
  config.max_documents = static_cast<size_t>(
      EnvOr("GRPC_QPARSE_CACHE_MAX_DOCUMENTS", config.max_documents));
  config.max_bytes = EnvOr("GRPC_QPARSE_CACHE_MAX_BYTES", config.max_bytes);
  return config;
}

std::shared_ptr<std::string> DocumentCache::Lookup(
    const std::string& sha256_hex) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(sha256_hex);
  if (it == entries_.end()) return nullptr;
  lru_.splice(lru_.begin(), lru_, it->second.lru_it);
  return it->second.bytes;
}

void DocumentCache::Insert(const std::string& sha256_hex,
                           std::shared_ptr<std::string> bytes) {
  const uint64_t size = bytes->size();
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = entries_.find(sha256_hex);
  if (it != entries_.end()) {
    lru_.splice(lru_.begin(), lru_, it->second.lru_it);
    return;
  }
  if (config_.max_documents == 0 || size > config_.max_bytes) return;
  lru_.push_front(sha256_hex);
  entries_.emplace(sha256_hex, Entry{std::move(bytes), lru_.begin()});
  total_bytes_ += size;
  while (entries_.size() > config_.max_documents ||
         total_bytes_ > config_.max_bytes) {
    const std::string& oldest = lru_.back();
    total_bytes_ -= entries_.at(oldest).bytes->size();
    entries_.erase(oldest);
    lru_.pop_back();
  }
}

}  // namespace grpc_qparse
