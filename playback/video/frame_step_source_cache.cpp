#include "playback/video/frame_step_source_cache.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace playback_video_frame_step_prefetch {
namespace {

int64_t frameEndUs(const SourceFrame& frame) {
  if (frame.durationUs <= 0 ||
      frame.ptsUs > (std::numeric_limits<int64_t>::max)() - frame.durationUs) {
    return frame.ptsUs;
  }
  return frame.ptsUs + frame.durationUs;
}

bool validFrame(const std::shared_ptr<const SourceFrame>& frame) {
  return frame && frame->ptsUs >= 0 && frame->sourcePtsUs >= 0 &&
         frame->durationUs > 0 && frame->sourceDurationUs > 0 &&
         frame->identity.ptsUs == frame->sourcePtsUs &&
         frame->identity.durationUs == frame->sourceDurationUs;
}

template <typename Nodes>
auto findNode(Nodes& nodes, const FrameIdentity& identity) {
  return std::find_if(nodes.begin(), nodes.end(), [&](const auto& node) {
    return node.frame && sameIdentity(node.frame->identity, identity);
  });
}

bool containsIdentity(const std::vector<FrameIdentity>& identities,
                      const FrameIdentity& candidate) {
  return std::any_of(identities.begin(), identities.end(),
                     [&](const FrameIdentity& identity) {
                       return sameIdentity(identity, candidate);
                     });
}

}  // namespace

bool SourceFrameCache::commitDecodedRun(
    playback_video_frame_step::Direction direction,
    const FrameIdentity& joinIdentity,
    std::shared_ptr<const SourceFrame> joinFrame,
    const std::vector<std::shared_ptr<const SourceFrame>>& decodedFrames) {
  std::vector<std::shared_ptr<const SourceFrame>> run;
  run.reserve(decodedFrames.size() + 1);
  if (direction == playback_video_frame_step::Direction::Previous) {
    run.insert(run.end(), decodedFrames.begin(), decodedFrames.end());
    if (joinFrame) {
      run.push_back(joinFrame);
    }
  } else {
    if (joinFrame) {
      run.push_back(joinFrame);
    }
    run.insert(run.end(), decodedFrames.begin(), decodedFrames.end());
  }

  std::vector<Node> committed = nodes_;
  auto join = findNode(committed, joinIdentity);
  if (join == committed.end()) {
    if (!validFrame(joinFrame) ||
        !sameIdentity(joinFrame->identity, joinIdentity)) {
      return false;
    }
    committed.push_back(Node{std::move(joinFrame), std::nullopt, std::nullopt});
  } else if (joinFrame &&
             !sameIdentity(join->frame->identity, joinFrame->identity)) {
    return false;
  }

  if (run.empty()) {
    // The join was already cached and this refill reached a media boundary.
    nodes_.swap(committed);
    return true;
  }

  std::vector<FrameIdentity> runIdentities;
  runIdentities.reserve(run.size());
  for (const auto& frame : run) {
    if (!validFrame(frame) ||
        containsIdentity(runIdentities, frame->identity)) {
      return false;
    }
    if (!runIdentities.empty()) {
      const auto& previous = run[runIdentities.size() - 1];
      if (frame->ptsUs < previous->ptsUs ||
          frame->sourcePtsUs < previous->sourcePtsUs) {
        return false;
      }
    }
    runIdentities.push_back(frame->identity);
    if (findNode(committed, frame->identity) == committed.end()) {
      committed.push_back(Node{frame, std::nullopt, std::nullopt});
    }
  }

  const bool joinsRun =
      direction == playback_video_frame_step::Direction::Previous
          ? sameIdentity(runIdentities.back(), joinIdentity)
          : sameIdentity(runIdentities.front(), joinIdentity);
  if (!joinsRun) {
    // When the join was already cached it is intentionally omitted from the
    // payload. Add it to the local sequence solely for link validation.
    if (direction == playback_video_frame_step::Direction::Previous) {
      runIdentities.push_back(joinIdentity);
    } else {
      runIdentities.insert(runIdentities.begin(), joinIdentity);
    }
  }

  for (size_t index = 1; index < runIdentities.size(); ++index) {
    auto left = findNode(committed, runIdentities[index - 1]);
    auto right = findNode(committed, runIdentities[index]);
    if (left == committed.end() || right == committed.end()) {
      return false;
    }
    if (right->frame->ptsUs < left->frame->ptsUs ||
        right->frame->sourcePtsUs < left->frame->sourcePtsUs) {
      return false;
    }
    if ((left->next && !sameIdentity(*left->next, right->frame->identity)) ||
        (right->previous &&
         !sameIdentity(*right->previous, left->frame->identity))) {
      return false;
    }
    left->next = right->frame->identity;
    right->previous = left->frame->identity;
  }

  nodes_.swap(committed);
  return true;
}

std::shared_ptr<const SourceFrame> SourceFrameCache::find(
    const FrameIdentity& identity) const {
  auto found = findNode(nodes_, identity);
  return found == nodes_.end() ? std::shared_ptr<const SourceFrame>{}
                               : found->frame;
}

FrameWindow SourceFrameCache::windowAround(const FrameIdentity& anchorIdentity,
                                           int64_t beforeDurationUs,
                                           int64_t afterDurationUs,
                                           size_t maximumFrameCount) const {
  FrameWindow window;
  if (beforeDurationUs < 0 || afterDurationUs < 0 || maximumFrameCount == 0) {
    return window;
  }
  auto anchor = findNode(nodes_, anchorIdentity);
  if (anchor == nodes_.end()) {
    return window;
  }

  std::vector<std::shared_ptr<const SourceFrame>> before;
  const Node* cursor = &*anchor;
  while (beforeDurationUs > 0 && cursor->previous &&
         before.size() + 1 < maximumFrameCount) {
    auto previous = findNode(nodes_, *cursor->previous);
    if (previous == nodes_.end()) {
      return {};
    }
    before.push_back(previous->frame);
    cursor = &*previous;
    if (anchor->frame->ptsUs - cursor->frame->ptsUs >= beforeDurationUs) {
      break;
    }
  }
  std::reverse(before.begin(), before.end());

  window.frames = before;
  window.anchorIndex = window.frames.size();
  window.frames.push_back(anchor->frame);
  cursor = &*anchor;
  while (afterDurationUs > 0 && cursor->next &&
         window.frames.size() < maximumFrameCount) {
    auto next = findNode(nodes_, *cursor->next);
    if (next == nodes_.end()) {
      return {};
    }
    window.frames.push_back(next->frame);
    cursor = &*next;
    if (frameEndUs(*cursor->frame) - frameEndUs(*anchor->frame) >=
        afterDurationUs) {
      break;
    }
  }
  return window;
}

bool SourceFrameCache::retainWindow(const FrameIdentity& anchorIdentity,
                                    int64_t beforeDurationUs,
                                    int64_t afterDurationUs,
                                    size_t maximumFrameCount) {
  FrameWindow retained = windowAround(anchorIdentity, beforeDurationUs,
                                      afterDurationUs, maximumFrameCount);
  if (!retained.valid()) {
    return false;
  }
  const auto retainedIdentity = [&](const FrameIdentity& identity) {
    return std::any_of(retained.frames.begin(), retained.frames.end(),
                       [&](const std::shared_ptr<const SourceFrame>& frame) {
                         return sameIdentity(frame->identity, identity);
                       });
  };
  for (Node& node : nodes_) {
    if (!retainedIdentity(node.frame->identity)) {
      continue;
    }
    if (node.previous && !retainedIdentity(*node.previous)) {
      node.previous.reset();
    }
    if (node.next && !retainedIdentity(*node.next)) {
      node.next.reset();
    }
  }
  nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
                              [&](const Node& node) {
                                return !retainedIdentity(node.frame->identity);
                              }),
               nodes_.end());
  return true;
}

void SourceFrameCache::clear() { nodes_.clear(); }

}  // namespace playback_video_frame_step_prefetch
