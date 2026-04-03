#include "taskgate.h"

#include <stdexcept>

GateGroup::GateGroup(std::string name, TaskGate* gate)
    : name_(std::move(name)), gate_(gate) {}

TaskGate::Bucket& TaskGate::bucket(const std::string& name) const {
  {
    std::lock_guard<std::mutex> lock(bucketsMutex_);
    auto it = buckets_.find(name);
    if (it != buckets_.end()) {
      return *it->second;
    }
  }
  std::lock_guard<std::mutex> lock(bucketsMutex_);
  auto it = buckets_.find(name);
  if (it == buckets_.end()) {
    auto bucket = std::make_unique<Bucket>();
    it = buckets_.emplace(name, std::move(bucket)).first;
  }
  return *it->second;
}

GateGroup TaskGate::group(const std::string& name) {
  bucket(name);
  return GateGroup(name, this);
}

std::unordered_map<std::string, GateSnapshot> TaskGate::snapshotAll() const {
  std::vector<std::string> names;
  {
    std::lock_guard<std::mutex> lock(bucketsMutex_);
    names.reserve(buckets_.size());
    for (const auto& pair : buckets_) {
      names.push_back(pair.first);
    }
  }
  std::unordered_map<std::string, GateSnapshot> out;
  out.reserve(names.size());
  for (const auto& name : names) {
    out.emplace(name, GateGroup(name, const_cast<TaskGate*>(this)).snapshot());
  }
  return out;
}

uint64_t GateGroup::bump() {
  if (!gate_) {
    throw std::runtime_error("GateGroup.bump() called on null gate");
  }
  auto& bucket = gate_->bucket(name_);
  std::lock_guard<std::mutex> lock(bucket.mutex);
  bucket.gen++;
  bucket.blockingPending = 0;
  bucket.countsByCat.clear();
  bucket.live.clear();
  return bucket.gen;
}

GateToken GateGroup::begin(const GateScope& scope) {
  if (!gate_) {
    throw std::runtime_error("GateGroup.begin() called on null gate");
  }
  auto& bucket = gate_->bucket(name_);
  std::lock_guard<std::mutex> lock(bucket.mutex);
  GateToken token;
  token.gen = bucket.gen;
  token.id = bucket.nextId++;
  token.blocking = scope.blocking;
  token.active = true;
  token.category = scope.category.empty() ? "other" : scope.category;
  token.tag = scope.tag;
  token.group = name_;
  bucket.live.emplace(token.id, token);
  bucket.countsByCat[token.category]++;
  if (token.blocking) {
    bucket.blockingPending++;
  }
  return token;
}

void GateGroup::end(GateToken& token) {
  if (!gate_) {
    throw std::runtime_error("GateGroup.end() called on null gate");
  }
  if (!token.active) {
    return;
  }
  if (token.group != name_) {
    throw std::runtime_error("GateGroup.end() called with foreign token");
  }
  auto& bucket = gate_->bucket(name_);
  std::lock_guard<std::mutex> lock(bucket.mutex);
  if (token.gen != bucket.gen) {
    token.active = false;
    return;
  }
  auto it = bucket.live.find(token.id);
  if (it == bucket.live.end()) {
    throw std::runtime_error("GateGroup.end() called on unknown token");
  }
  const GateToken& live = it->second;
  auto catIt = bucket.countsByCat.find(live.category);
  if (catIt != bucket.countsByCat.end()) {
    if (catIt->second > 1) {
      catIt->second--;
    } else {
      bucket.countsByCat.erase(catIt);
    }
  }
  if (live.blocking) {
    if (bucket.blockingPending == 0) {
      throw std::runtime_error("GateGroup.end() blockingPending underflow");
    }
    bucket.blockingPending--;
  }
  bucket.live.erase(it);
  token.active = false;
}

GateToken GateGroup::ensure(GateToken& token, bool active,
                            const GateScope& scope) {
  if (active) {
    if (!token.active) {
      token = begin(scope);
    }
  } else if (token.active) {
    end(token);
  }
  return token;
}

void GateGroup::endCategory(const std::string& category) {
  if (!gate_) {
    throw std::runtime_error("GateGroup.endCategory() called on null gate");
  }
  auto& bucket = gate_->bucket(name_);
  std::lock_guard<std::mutex> lock(bucket.mutex);
  for (auto it = bucket.live.begin(); it != bucket.live.end();) {
    if (it->second.category == category) {
      const GateToken& live = it->second;
      auto catIt = bucket.countsByCat.find(live.category);
      if (catIt != bucket.countsByCat.end()) {
        if (catIt->second > 1) {
          catIt->second--;
        } else {
          bucket.countsByCat.erase(catIt);
        }
      }
      if (live.blocking) {
        if (bucket.blockingPending == 0) {
          throw std::runtime_error(
              "GateGroup.endCategory() blockingPending underflow");
        }
        bucket.blockingPending--;
      }
      it = bucket.live.erase(it);
    } else {
      ++it;
    }
  }
}

void GateGroup::endAll() {
  if (!gate_) {
    throw std::runtime_error("GateGroup.endAll() called on null gate");
  }
  auto& bucket = gate_->bucket(name_);
  std::lock_guard<std::mutex> lock(bucket.mutex);
  bucket.live.clear();
  bucket.countsByCat.clear();
  bucket.blockingPending = 0;
}

bool GateGroup::ready() const {
  if (!gate_) return true;
  auto& bucket = gate_->bucket(name_);
  std::lock_guard<std::mutex> lock(bucket.mutex);
  return bucket.blockingPending == 0;
}

size_t GateGroup::liveCount() const {
  if (!gate_) return 0;
  auto& bucket = gate_->bucket(name_);
  std::lock_guard<std::mutex> lock(bucket.mutex);
  return bucket.live.size();
}

bool GateGroup::readyFor(const std::string& category) const {
  if (!gate_) return true;
  auto& bucket = gate_->bucket(name_);
  std::lock_guard<std::mutex> lock(bucket.mutex);
  auto it = bucket.countsByCat.find(category);
  if (it == bucket.countsByCat.end()) return true;
  return it->second == 0;
}

bool GateGroup::readyForBlocking(const std::string& category) const {
  if (!gate_) return true;
  auto& bucket = gate_->bucket(name_);
  std::lock_guard<std::mutex> lock(bucket.mutex);
  for (const auto& pair : bucket.live) {
    const GateToken& token = pair.second;
    if (token.category == category && token.blocking) {
      return false;
    }
  }
  return true;
}

GateSnapshot GateGroup::snapshot() const {
  GateSnapshot snapshot;
  if (!gate_) return snapshot;
  auto& bucket = gate_->bucket(name_);
  std::lock_guard<std::mutex> lock(bucket.mutex);
  snapshot.gen = bucket.gen;
  snapshot.ready = (bucket.blockingPending == 0);
  snapshot.blockingPending = bucket.blockingPending;
  snapshot.countsByCat = bucket.countsByCat;
  snapshot.live.reserve(bucket.live.size());
  for (const auto& pair : bucket.live) {
    const GateToken& token = pair.second;
    GateTokenSummary summary;
    summary.id = token.id;
    summary.category = token.category;
    summary.blocking = token.blocking;
    summary.tag = token.tag;
    snapshot.live.push_back(std::move(summary));
    if (token.blocking) {
      snapshot.blockingByCat[token.category]++;
    }
  }
  return snapshot;
}
