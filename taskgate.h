#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct GateScope {
  bool blocking = false;
  std::string category = "other";
  std::string tag;
};

struct GateToken {
  uint64_t gen = 0;
  uint64_t id = 0;
  bool blocking = false;
  bool active = false;
  std::string category = "other";
  std::string tag;
  std::string group;
};

struct GateTokenSummary {
  uint64_t id = 0;
  std::string category;
  bool blocking = false;
  std::string tag;
};

struct GateSnapshot {
  uint64_t gen = 0;
  bool ready = true;
  uint64_t blockingPending = 0;
  std::unordered_map<std::string, uint64_t> countsByCat;
  std::unordered_map<std::string, uint64_t> blockingByCat;
  std::vector<GateTokenSummary> live;
};

class TaskGate;

class GateGroup {
 public:
  GateGroup() = default;
  GateGroup(std::string name, TaskGate* gate);

  uint64_t bump();
  GateToken begin(const GateScope& scope = {});
  void end(GateToken& token);
  GateToken ensure(GateToken& token, bool active, const GateScope& scope);
  void endCategory(const std::string& category);
  void endAll();

  bool ready() const;
  size_t liveCount() const;
  bool readyFor(const std::string& category) const;
  bool readyForBlocking(const std::string& category) const;
  GateSnapshot snapshot() const;

 private:
  std::string name_;
  TaskGate* gate_ = nullptr;
};

class TaskGate {
 public:
  GateGroup group(const std::string& name);
  std::unordered_map<std::string, GateSnapshot> snapshotAll() const;

 private:
  struct Bucket {
    mutable std::mutex mutex;
    uint64_t gen = 0;
    uint64_t nextId = 1;
    uint64_t blockingPending = 0;
    std::unordered_map<std::string, uint64_t> countsByCat;
    std::unordered_map<uint64_t, GateToken> live;
  };
  Bucket& bucket(const std::string& name) const;

  mutable std::mutex bucketsMutex_;
  mutable std::unordered_map<std::string, std::unique_ptr<Bucket>> buckets_;

  friend class GateGroup;
};
