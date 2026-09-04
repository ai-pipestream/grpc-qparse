#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace grpc_qparse {

// Bounds for the content-addressed document cache. Defaults follow the
// fleet's GRPC_QPARSE_* env convention; a zero max_documents disables
// caching entirely.
struct DocumentCacheConfig {
  // GRPC_QPARSE_CACHE_MAX_DOCUMENTS, default 8.
  size_t max_documents = 8;
  // GRPC_QPARSE_CACHE_MAX_BYTES, default 2 GiB. A single document larger
  // than this is never cached.
  uint64_t max_bytes = 2ull * 1024 * 1024 * 1024;
};

// Reads the cache bounds from the environment, falling back to the defaults
// on unset or unparseable values.
DocumentCacheConfig DocumentCacheConfigFromEnv();

// The byte cache behind PdfDocument.sha256: an LRU keyed by the lowercase
// hex digest of the document bytes. Shared across requests, so all access is
// mutex-guarded; the handed-out buffers are shared with the engine's
// per-request decoders, which only read them.
class DocumentCache {
 public:
  explicit DocumentCache(DocumentCacheConfig config = {}) : config_(config) {}

  // The cached bytes for a digest, or nullptr on a miss. A hit promotes the
  // entry to most-recently-used.
  std::shared_ptr<std::string> Lookup(const std::string& sha256_hex);

  // Caches the bytes under their verified digest, subject to the configured
  // bounds; least-recently-used entries are evicted first. Re-inserting a
  // known digest just refreshes its recency.
  void Insert(const std::string& sha256_hex, std::shared_ptr<std::string> bytes);

 private:
  struct Entry {
    std::shared_ptr<std::string> bytes;
    std::list<std::string>::iterator lru_it;
  };

  const DocumentCacheConfig config_;
  std::mutex mutex_;
  // Digests, most-recently-used first.
  std::list<std::string> lru_;
  std::unordered_map<std::string, Entry> entries_;
  uint64_t total_bytes_ = 0;
};

}  // namespace grpc_qparse
