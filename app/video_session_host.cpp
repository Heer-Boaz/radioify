#include "app/video_session_host.h"

#include <stdexcept>
#include <utility>

namespace application_playback {

struct VideoSessionHost::Impl {
  using SessionPtr = std::unique_ptr<playback_session::VideoSession>;

  struct Opening {
    Opening(SessionPtr session,
            VideoActivationTransaction transaction,
            playback_queue::Queue::PreparedActivation activation)
        : transaction(std::move(transaction)),
          activation(std::move(activation)),
          session(std::move(session)) {}

    // Session destruction must finish replacing/stopping its audio stream
    // before an unresolved activation transaction restores displaced audio.
    VideoActivationTransaction transaction;
    playback_queue::Queue::PreparedActivation activation;
    SessionPtr session;
  };

  struct Active {
    Active(SessionPtr session, PlaybackTarget target)
        : session(std::move(session)), target(std::move(target)) {}

    SessionPtr session;
    PlaybackTarget target;
  };

  struct Completed {
    Completed(SessionPtr session, PlaybackTarget target,
              PlaybackSessionCompletion completion)
        : session(std::move(session)),
          target(std::move(target)),
          completion(std::move(completion)) {}

    SessionPtr session;
    PlaybackTarget target;
    PlaybackSessionCompletion completion;
  };

  explicit Impl(playback_session::VideoSessionFactory sessionFactory)
      : factory(std::move(sessionFactory)) {
    if (!factory) {
      throw std::invalid_argument(
          "VideoSessionHost requires a video-session factory.");
    }
  }

  SessionRef session() {
    if (auto* openingState = std::get_if<Opening>(&state)) {
      return std::ref(*openingState->session);
    }
    if (auto* activeState = std::get_if<Active>(&state)) {
      return std::ref(*activeState->session);
    }
    if (auto* completedState = std::get_if<Completed>(&state)) {
      return std::ref(*completedState->session);
    }
    return std::nullopt;
  }

  ConstSessionRef session() const {
    if (const auto* openingState = std::get_if<Opening>(&state)) {
      return std::cref(*openingState->session);
    }
    if (const auto* activeState = std::get_if<Active>(&state)) {
      return std::cref(*activeState->session);
    }
    if (const auto* completedState = std::get_if<Completed>(&state)) {
      return std::cref(*completedState->session);
    }
    return std::nullopt;
  }

  OpenFinished finishOpen(
      SessionPtr session,
      VideoActivationTransaction transaction,
      playback_queue::Queue::PreparedActivation activation,
      playback_session::OpenOutcome outcome) {
    if (std::holds_alternative<playback_session::OpenReady>(outcome)) {
      state.emplace<Active>(std::move(session), activation.route().target);
    } else {
      session.reset();
      state.emplace<std::monostate>();
    }
    return OpenFinished{std::move(transaction), std::move(activation),
                        std::move(outcome)};
  }

  playback_session::VideoSessionFactory factory;
  std::variant<std::monostate, Opening, Active, Completed> state;
};

VideoSessionHost::VideoSessionHost(
    playback_session::VideoSessionFactory factory)
    : impl_(std::make_unique<Impl>(std::move(factory))) {}

VideoSessionHost::~VideoSessionHost() = default;

bool VideoSessionHost::empty() const {
  return std::holds_alternative<std::monostate>(impl_->state);
}

bool VideoSessionHost::opening() const {
  return std::holds_alternative<Impl::Opening>(impl_->state);
}

bool VideoSessionHost::completionPending() const {
  return std::holds_alternative<Impl::Completed>(impl_->state);
}

bool VideoSessionHost::ready() const {
  const auto* activeState = std::get_if<Impl::Active>(&impl_->state);
  return activeState && activeState->session->ready();
}

VideoSessionHost::SessionRef VideoSessionHost::session() {
  return impl_->session();
}

VideoSessionHost::ConstSessionRef VideoSessionHost::session() const {
  return impl_->session();
}

std::optional<PlaybackShellTerminalRole> VideoSessionHost::terminalRole()
    const {
  ConstSessionRef current = session();
  if (!current) return std::nullopt;
  return current->get().terminalRole();
}

std::optional<playback_session::ViewSnapshot>
VideoSessionHost::viewSnapshot() const {
  const auto* activeState = std::get_if<Impl::Active>(&impl_->state);
  return activeState ? activeState->session->viewSnapshot()
                     : std::nullopt;
}

std::optional<playback_session::TransitionSnapshot>
VideoSessionHost::transitionSnapshot() const {
  ConstSessionRef current = session();
  return current ? current->get().transitionSnapshot() : std::nullopt;
}

std::vector<NativeWaitHandle> VideoSessionHost::activityWaitHandles() const {
  ConstSessionRef current = session();
  return current ? current->get().activityWaitHandles()
                 : std::vector<NativeWaitHandle>{};
}

wake_schedule::Deadline VideoSessionHost::nextWakeDeadline() const {
  ConstSessionRef current = session();
  return current ? current->get().nextWakeDeadline() : std::nullopt;
}

bool VideoSessionHost::capturesBrowserInput() const {
  ConstSessionRef current = session();
  return current && current->get().capturesBrowserInput();
}

void VideoSessionHost::setExternalInputModal(bool modal) {
  if (SessionRef current = session()) {
    current->get().setExternalInputModal(modal);
  }
}

bool VideoSessionHost::handleInputEvent(const InputEvent& event) {
  SessionRef current = session();
  return current && current->get().handleInputEvent(event);
}

bool VideoSessionHost::pollWindowInput(InputEvent& event) {
  SessionRef current = session();
  return current && current->get().pollWindowInput(event);
}

bool VideoSessionHost::handleWindowInputEvent(const InputEvent& event) {
  SessionRef current = session();
  return current && current->get().handleWindowInputEvent(event);
}

bool VideoSessionHost::toggleWindowPresentation() {
  SessionRef current = session();
  return current && current->get().toggleWindowPresentation();
}

bool VideoSessionHost::togglePictureInPicture() {
  SessionRef current = session();
  return current && current->get().togglePictureInPicture();
}

bool VideoSessionHost::toggleFullscreen() {
  SessionRef current = session();
  return current && current->get().toggleFullscreen();
}

bool VideoSessionHost::activatePresentation() {
  SessionRef current = session();
  return current && current->get().activatePresentation();
}

std::vector<playback_session::Event> VideoSessionHost::drainEvents() {
  SessionRef current = session();
  return current ? current->get().drainEvents()
                 : std::vector<playback_session::Event>{};
}

void VideoSessionHost::mediaTaskFinished(
    const playback_media_processing::Completion& completion) {
  if (SessionRef current = session()) {
    current->get().mediaTaskFinished(completion);
  }
}

void VideoSessionHost::mediaTaskActivityChanged(
    std::optional<playback_media_processing::Activity> activity) {
  if (SessionRef current = session()) {
    current->get().mediaTaskActivityChanged(std::move(activity));
  }
}

bool VideoSessionHost::requestQuit() {
  SessionRef current = session();
  if (!current) return false;
  current->get().requestQuit();
  return true;
}

bool VideoSessionHost::handoffRequestDeferred() const {
  if (!std::holds_alternative<Impl::Active>(impl_->state)) return false;
  ConstSessionRef current = session();
  return current && current->get().handoffRequestDeferred();
}

std::optional<playback_session_exit::RequestId>
VideoSessionHost::requestHandoff() {
  if (!std::holds_alternative<Impl::Active>(impl_->state)) {
    return std::nullopt;
  }
  SessionRef current = session();
  return current ? current->get().requestHandoff() : std::nullopt;
}

bool VideoSessionHost::resolveHandoff(
    playback_session_exit::RequestId requestId, bool accepted) {
  SessionRef current = session();
  return current && current->get().resolveHandoff(requestId, accepted);
}

bool VideoSessionHost::abortHandoff(
    playback_session_exit::RequestId requestId) {
  SessionRef current = session();
  return current && current->get().abortHandoff(requestId);
}

VideoSessionHost::StartResult VideoSessionHost::start(
    playback_session::VideoSessionRequest request,
    VideoActivationTransaction transaction,
    playback_queue::Queue::PreparedActivation activation) {
  if (!empty()) {
    return StartRejected{VideoSessionStartFailure::HostOccupied};
  }
  request.file = playbackTargetFile(activation.route().target);

  std::unique_ptr<playback_session::VideoSession> current =
      impl_->factory(std::move(request));
  if (!current) {
    return StartRejected{
        VideoSessionStartFailure::FactoryReturnedNoSession};
  }

  std::optional<playback_session::OpenOutcome> outcome =
      current->startOpen();
  if (!outcome) {
    impl_->state.emplace<Impl::Opening>(
        std::move(current), std::move(transaction), std::move(activation));
    return OpenPending{};
  }
  return impl_->finishOpen(std::move(current), std::move(transaction),
                           std::move(activation), std::move(*outcome));
}

std::optional<VideoSessionHost::OpenFinished>
VideoSessionHost::pumpOpen() {
  auto* openingState = std::get_if<Impl::Opening>(&impl_->state);
  if (!openingState) return std::nullopt;

  std::optional<playback_session::OpenOutcome> outcome =
      openingState->session->pumpOpen();
  if (!outcome) return std::nullopt;

  std::unique_ptr<playback_session::VideoSession> current =
      std::move(openingState->session);
  VideoActivationTransaction transaction =
      std::move(openingState->transaction);
  playback_queue::Queue::PreparedActivation activation =
      std::move(openingState->activation);
  impl_->state.emplace<std::monostate>();
  return impl_->finishOpen(std::move(current), std::move(transaction),
                           std::move(activation), std::move(*outcome));
}

bool VideoSessionHost::pumpPlayback() {
  auto* activeState = std::get_if<Impl::Active>(&impl_->state);
  if (!activeState) return false;
  std::optional<PlaybackSessionCompletion> completion =
      activeState->session->pump();
  if (!completion) return false;

  std::unique_ptr<playback_session::VideoSession> current =
      std::move(activeState->session);
  PlaybackTarget target = std::move(activeState->target);
  impl_->state.emplace<Impl::Completed>(
      std::move(current), std::move(target), std::move(*completion));
  return true;
}

std::optional<VideoSessionHost::SessionFinished>
VideoSessionHost::takeCompletion() {
  auto* completedState = std::get_if<Impl::Completed>(&impl_->state);
  if (!completedState) return std::nullopt;
  SessionFinished finished{std::move(completedState->target),
                           std::move(completedState->completion)};
  impl_->state.emplace<std::monostate>();
  return finished;
}

}  // namespace application_playback
