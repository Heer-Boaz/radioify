#include "midiaudio.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr uint32_t kDefaultTempoUsPerQuarter = 500000u;
constexpr uint32_t kMinTempoUsPerQuarter = 1000u;
constexpr uint32_t kMaxTempoUsPerQuarter = 10000000u;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kHalfPi = 1.57079632679489661923f;
constexpr float kMasterGain = 0.25f;
constexpr float kAttackSeconds = 0.004f;
constexpr float kDecaySeconds = 0.090f;
constexpr float kSustainLevel = 0.72f;
constexpr float kReleaseSeconds = 0.130f;
constexpr float kTailSeconds = 0.90f;
constexpr size_t kMaxVoices = 96;

void setError(std::string* error, const char* message) {
  if (error && message) {
    *error = message;
  }
}

uint16_t readU16BE(const uint8_t* data) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8u) |
                               static_cast<uint16_t>(data[1]));
}

uint32_t readU32BE(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24u) |
         (static_cast<uint32_t>(data[1]) << 16u) |
         (static_cast<uint32_t>(data[2]) << 8u) |
         static_cast<uint32_t>(data[3]);
}

bool readVarLen(const uint8_t* data, size_t size, size_t* pos,
                uint32_t* outValue) {
  if (!data || !pos || !outValue) return false;
  uint64_t value = 0;
  for (int i = 0; i < 5; ++i) {
    if (*pos >= size) return false;
    uint8_t b = data[(*pos)++];
    value = (value << 7u) | static_cast<uint64_t>(b & 0x7Fu);
    if ((b & 0x80u) == 0u) {
      if (value > std::numeric_limits<uint32_t>::max()) return false;
      *outValue = static_cast<uint32_t>(value);
      return true;
    }
  }
  return false;
}

bool readFileBytes(const std::filesystem::path& path,
                   std::vector<uint8_t>* outBytes) {
  if (!outBytes) return false;
  outBytes->clear();
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return false;
  in.seekg(0, std::ios::end);
  std::streamoff size = in.tellg();
  if (size < 0) return false;
  in.seekg(0, std::ios::beg);
  outBytes->resize(static_cast<size_t>(size));
  if (!outBytes->empty()) {
    in.read(reinterpret_cast<char*>(outBytes->data()),
            static_cast<std::streamsize>(outBytes->size()));
  }
  return in.good() || in.eof();
}

float clamp01(float value) { return std::clamp(value, 0.0f, 1.0f); }

float softClip(float x) { return x / (1.0f + std::fabs(x)); }

double noteToFrequency(double midiNote) {
  return 440.0 * std::exp2((midiNote - 69.0) / 12.0);
}

enum class RawEventKind : uint8_t {
  NoteOn,
  NoteOff,
  ProgramChange,
  ControlChange,
  PitchBend,
  Tempo,
};

struct RawEvent {
  uint64_t tick = 0;
  uint64_t order = 0;
  RawEventKind kind = RawEventKind::NoteOn;
  uint8_t channel = 0;
  uint8_t data1 = 0;
  uint8_t data2 = 0;
  uint32_t tempoUsPerQuarter = 0;
};

enum class ScheduledEventKind : uint8_t {
  NoteOn,
  NoteOff,
  ProgramChange,
  ControlChange,
  PitchBend,
};

struct ScheduledEvent {
  uint64_t frame = 0;
  uint64_t order = 0;
  ScheduledEventKind kind = ScheduledEventKind::NoteOn;
  uint8_t channel = 0;
  uint8_t data1 = 0;
  uint8_t data2 = 0;
};

enum class Waveform : uint8_t {
  Sine,
  Triangle,
  Saw,
  Square,
  Noise,
};

Waveform waveformForProgram(uint8_t program) {
  switch (program / 8u) {
    case 0:
      return Waveform::Sine;
    case 1:
      return Waveform::Triangle;
    case 2:
      return Waveform::Saw;
    case 3:
      return Waveform::Square;
    case 4:
      return Waveform::Saw;
    case 5:
      return Waveform::Square;
    case 6:
      return Waveform::Triangle;
    case 7:
      return Waveform::Sine;
    case 8:
      return Waveform::Square;
    case 9:
      return Waveform::Saw;
    case 10:
      return Waveform::Triangle;
    default:
      return Waveform::Sine;
  }
}

struct ChannelState {
  uint8_t program = 0;
  uint8_t volume = 100;
  uint8_t expression = 127;
  uint8_t pan = 64;
  uint16_t pitchBend = 8192;
  uint8_t pitchBendRangeSemis = 2;
  bool sustain = false;

  float gain() const {
    float vol = static_cast<float>(volume) / 127.0f;
    float expr = static_cast<float>(expression) / 127.0f;
    return vol * expr;
  }

  float panNorm() const {
    return clamp01(static_cast<float>(pan) / 127.0f);
  }

  float bendSemitones() const {
    float normalized =
        (static_cast<float>(pitchBend) - 8192.0f) / 8192.0f;
    return normalized * static_cast<float>(pitchBendRangeSemis);
  }
};

struct Voice {
  bool active = false;
  bool released = false;
  bool keyHeld = false;
  bool sustained = false;
  uint8_t channel = 0;
  uint8_t note = 0;
  float velocity = 0.0f;
  float frequency = 440.0f;
  double phase = 0.0;
  float level = 0.0f;
  float sustainLevel = kSustainLevel;
  float attackRate = 0.0f;
  float decayRate = 0.0f;
  float releaseRate = 0.0f;
  Waveform waveform = Waveform::Sine;
  uint32_t noiseState = 0x12345678u;
};

bool parseTrackEvents(const uint8_t* data, size_t size, uint64_t* nextOrder,
                      std::vector<RawEvent>* out, uint64_t* outEndTick,
                      std::string* error) {
  if (!data || !nextOrder || !out || !outEndTick) return false;
  size_t pos = 0;
  uint64_t tick = 0;
  uint8_t runningStatus = 0;

  auto pushEvent = [&](const RawEvent& event) { out->push_back(event); };

  while (pos < size) {
    uint32_t delta = 0;
    if (!readVarLen(data, size, &pos, &delta)) {
      setError(error, "Invalid MIDI delta-time encoding.");
      return false;
    }
    tick += static_cast<uint64_t>(delta);

    if (pos >= size) break;
    uint8_t status = data[pos++];
    bool usedRunningStatus = false;
    uint8_t firstData = 0;

    if (status < 0x80u) {
      if (runningStatus < 0x80u || runningStatus >= 0xF0u) {
        setError(error, "MIDI file uses invalid running status.");
        return false;
      }
      firstData = status;
      status = runningStatus;
      usedRunningStatus = true;
    } else if (status < 0xF0u) {
      runningStatus = status;
    } else {
      runningStatus = 0;
    }

    if (status == 0xFFu) {
      if (pos >= size) {
        setError(error, "Malformed MIDI meta event.");
        return false;
      }
      uint8_t metaType = data[pos++];
      uint32_t metaLen = 0;
      if (!readVarLen(data, size, &pos, &metaLen) ||
          size - pos < static_cast<size_t>(metaLen)) {
        setError(error, "Malformed MIDI meta event length.");
        return false;
      }
      const uint8_t* metaData = data + pos;
      if (metaType == 0x51u && metaLen == 3u) {
        RawEvent tempo{};
        tempo.tick = tick;
        tempo.order = (*nextOrder)++;
        tempo.kind = RawEventKind::Tempo;
        tempo.tempoUsPerQuarter =
            (static_cast<uint32_t>(metaData[0]) << 16u) |
            (static_cast<uint32_t>(metaData[1]) << 8u) |
            static_cast<uint32_t>(metaData[2]);
        pushEvent(tempo);
      }
      pos += static_cast<size_t>(metaLen);
      if (metaType == 0x2Fu) {
        *outEndTick = tick;
        return true;
      }
      continue;
    }

    if (status == 0xF0u || status == 0xF7u) {
      uint32_t sysexLen = 0;
      if (!readVarLen(data, size, &pos, &sysexLen) ||
          size - pos < static_cast<size_t>(sysexLen)) {
        setError(error, "Malformed MIDI SysEx event.");
        return false;
      }
      pos += static_cast<size_t>(sysexLen);
      continue;
    }

    uint8_t op = static_cast<uint8_t>(status & 0xF0u);
    uint8_t channel = static_cast<uint8_t>(status & 0x0Fu);
    auto readDataByte = [&](uint8_t* outByte, bool useFirstData) -> bool {
      if (useFirstData) {
        *outByte = firstData;
        return true;
      }
      if (pos >= size) return false;
      *outByte = data[pos++];
      return true;
    };

    if (op == 0xC0u || op == 0xD0u) {
      uint8_t d1 = 0;
      if (!readDataByte(&d1, usedRunningStatus)) {
        setError(error, "Malformed MIDI channel event.");
        return false;
      }
      if (op == 0xC0u) {
        RawEvent event{};
        event.tick = tick;
        event.order = (*nextOrder)++;
        event.kind = RawEventKind::ProgramChange;
        event.channel = channel;
        event.data1 = d1;
        pushEvent(event);
      }
      continue;
    }

    if (op >= 0x80u && op <= 0xE0u) {
      uint8_t d1 = 0;
      uint8_t d2 = 0;
      if (!readDataByte(&d1, usedRunningStatus) || pos >= size) {
        setError(error, "Malformed MIDI channel event.");
        return false;
      }
      d2 = data[pos++];

      RawEvent event{};
      event.tick = tick;
      event.order = (*nextOrder)++;
      event.channel = channel;
      event.data1 = d1;
      event.data2 = d2;

      if (op == 0x80u) {
        event.kind = RawEventKind::NoteOff;
        pushEvent(event);
      } else if (op == 0x90u) {
        event.kind = (d2 == 0) ? RawEventKind::NoteOff : RawEventKind::NoteOn;
        pushEvent(event);
      } else if (op == 0xB0u) {
        event.kind = RawEventKind::ControlChange;
        pushEvent(event);
      } else if (op == 0xE0u) {
        event.kind = RawEventKind::PitchBend;
        pushEvent(event);
      }
      continue;
    }
  }

  *outEndTick = tick;
  return true;
}

bool parseMidiFile(const std::vector<uint8_t>& bytes, uint16_t* outDivision,
                   std::vector<RawEvent>* outEvents, uint64_t* outMaxTick,
                   std::string* error) {
  if (!outDivision || !outEvents || !outMaxTick) return false;
  outEvents->clear();
  *outDivision = 0;
  *outMaxTick = 0;

  if (bytes.size() < 14u) {
    setError(error, "MIDI file is too small.");
    return false;
  }
  if (!(bytes[0] == 'M' && bytes[1] == 'T' && bytes[2] == 'h' &&
        bytes[3] == 'd')) {
    setError(error, "Missing MIDI header chunk.");
    return false;
  }

  uint32_t headerSize = readU32BE(bytes.data() + 4u);
  if (headerSize < 6u || bytes.size() < 8u + headerSize) {
    setError(error, "Invalid MIDI header size.");
    return false;
  }

  const uint8_t* header = bytes.data() + 8u;
  uint16_t format = readU16BE(header + 0u);
  uint16_t declaredTrackCount = readU16BE(header + 2u);
  uint16_t division = readU16BE(header + 4u);

  if (division == 0u || (division & 0x8000u) != 0u) {
    setError(error, "Unsupported MIDI timing format.");
    return false;
  }
  if (format > 1u) {
    setError(error, "Unsupported MIDI format type.");
    return false;
  }

  size_t pos = 8u + static_cast<size_t>(headerSize);
  uint64_t order = 0;
  uint32_t foundTracks = 0;
  uint64_t maxTick = 0;

  while (pos + 8u <= bytes.size()) {
    uint32_t chunkId = readU32BE(bytes.data() + pos);
    uint32_t chunkSize = readU32BE(bytes.data() + pos + 4u);
    pos += 8u;
    if (bytes.size() - pos < static_cast<size_t>(chunkSize)) {
      setError(error, "MIDI chunk length exceeds file size.");
      return false;
    }

    if (chunkId == 0x4D54726Bu) {  // MTrk
      uint64_t trackEndTick = 0;
      if (!parseTrackEvents(bytes.data() + pos, static_cast<size_t>(chunkSize),
                            &order, outEvents, &trackEndTick, error)) {
        return false;
      }
      maxTick = std::max(maxTick, trackEndTick);
      ++foundTracks;
    }
    pos += static_cast<size_t>(chunkSize);
  }

  if (foundTracks == 0) {
    setError(error, "No MIDI track chunks found.");
    return false;
  }
  if (declaredTrackCount > 0u && foundTracks < declaredTrackCount) {
    setError(error, "MIDI file ended before all tracks were found.");
    return false;
  }

  *outDivision = division;
  *outMaxTick = maxTick;
  return true;
}

uint64_t microsToFrames(long double micros, uint32_t sampleRate) {
  if (micros <= 0.0L || sampleRate == 0u) return 0;
  long double frames = micros * static_cast<long double>(sampleRate) /
                       static_cast<long double>(1000000.0L);
  if (frames >=
      static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
    return std::numeric_limits<uint64_t>::max();
  }
  return static_cast<uint64_t>(std::llround(frames));
}

bool buildSchedule(const std::vector<RawEvent>& rawEvents, uint16_t division,
                   uint32_t sampleRate, uint64_t maxTick,
                   std::vector<ScheduledEvent>* outEvents,
                   uint64_t* outTotalFrames) {
  if (!outEvents || !outTotalFrames || division == 0u || sampleRate == 0u) {
    return false;
  }
  outEvents->clear();
  *outTotalFrames = 0;

  std::vector<RawEvent> sorted = rawEvents;
  std::sort(sorted.begin(), sorted.end(),
            [](const RawEvent& a, const RawEvent& b) {
              if (a.tick != b.tick) return a.tick < b.tick;
              return a.order < b.order;
            });

  uint32_t tempoUsPerQuarter = kDefaultTempoUsPerQuarter;
  uint64_t currentTick = 0;
  long double elapsedUs = 0.0L;
  bool hasNoteEvents = false;
  uint64_t lastNoteFrame = 0;

  for (const RawEvent& raw : sorted) {
    if (raw.tick > currentTick) {
      uint64_t deltaTick = raw.tick - currentTick;
      elapsedUs += static_cast<long double>(deltaTick) *
                   static_cast<long double>(tempoUsPerQuarter) /
                   static_cast<long double>(division);
      currentTick = raw.tick;
    }

    uint64_t frame = microsToFrames(elapsedUs, sampleRate);
    if (raw.kind == RawEventKind::Tempo) {
      if (raw.tempoUsPerQuarter > 0) {
        tempoUsPerQuarter = std::clamp(raw.tempoUsPerQuarter,
                                       kMinTempoUsPerQuarter,
                                       kMaxTempoUsPerQuarter);
      }
      continue;
    }

    ScheduledEvent scheduled{};
    scheduled.frame = frame;
    scheduled.order = raw.order;
    scheduled.channel = raw.channel;
    scheduled.data1 = raw.data1;
    scheduled.data2 = raw.data2;

    switch (raw.kind) {
      case RawEventKind::NoteOn:
        scheduled.kind = ScheduledEventKind::NoteOn;
        hasNoteEvents = true;
        lastNoteFrame = std::max(lastNoteFrame, frame);
        break;
      case RawEventKind::NoteOff:
        scheduled.kind = ScheduledEventKind::NoteOff;
        hasNoteEvents = true;
        lastNoteFrame = std::max(lastNoteFrame, frame);
        break;
      case RawEventKind::ProgramChange:
        scheduled.kind = ScheduledEventKind::ProgramChange;
        break;
      case RawEventKind::ControlChange:
        scheduled.kind = ScheduledEventKind::ControlChange;
        break;
      case RawEventKind::PitchBend:
        scheduled.kind = ScheduledEventKind::PitchBend;
        break;
      case RawEventKind::Tempo:
        break;
    }
    outEvents->push_back(scheduled);
  }

  if (maxTick > currentTick) {
    uint64_t deltaTick = maxTick - currentTick;
    elapsedUs += static_cast<long double>(deltaTick) *
                 static_cast<long double>(tempoUsPerQuarter) /
                 static_cast<long double>(division);
  }

  uint64_t totalFrames = microsToFrames(elapsedUs, sampleRate);
  if (hasNoteEvents) {
    uint64_t tailFrames =
        static_cast<uint64_t>(std::llround(kTailSeconds *
                                           static_cast<double>(sampleRate)));
    totalFrames = std::max(totalFrames, lastNoteFrame + tailFrames);
  }
  *outTotalFrames = totalFrames;
  return true;
}
}  // namespace

struct MidiAudioDecoder::Impl {
  bool init(const std::filesystem::path& path, uint32_t requestChannels,
            uint32_t requestSampleRate, std::string* error) {
    uninit();

    if (requestChannels != 1u && requestChannels != 2u) {
      setError(error, "Unsupported channel count for MIDI.");
      return false;
    }
    if (requestSampleRate == 0u) {
      setError(error, "Invalid sample rate for MIDI.");
      return false;
    }

    std::vector<uint8_t> bytes;
    if (!readFileBytes(path, &bytes)) {
      setError(error, "Failed to read MIDI file.");
      return false;
    }

    uint16_t division = 0;
    std::vector<RawEvent> rawEvents;
    uint64_t maxTick = 0;
    if (!parseMidiFile(bytes, &division, &rawEvents, &maxTick, error)) {
      return false;
    }

    std::vector<ScheduledEvent> schedule;
    uint64_t total = 0;
    if (!buildSchedule(rawEvents, division, requestSampleRate, maxTick,
                       &schedule, &total)) {
      setError(error, "Failed to build MIDI event schedule.");
      return false;
    }

    channels = requestChannels;
    sampleRate = requestSampleRate;
    totalFrames = total;
    events = std::move(schedule);
    resetPlaybackState();
    active = true;
    return true;
  }

  void uninit() {
    active = false;
    channels = 0;
    sampleRate = 0;
    totalFrames = 0;
    framePos = 0;
    nextEvent = 0;
    events.clear();
    voices.fill(Voice{});
    channelsState.fill(ChannelState{});
  }

  bool readFrames(float* out, uint32_t frameCount, uint64_t* framesRead) {
    if (framesRead) *framesRead = 0;
    if (!active || !out || frameCount == 0) return false;

    if (framePos >= totalFrames) {
      return true;
    }

    uint64_t remaining = totalFrames - framePos;
    uint64_t toRender = std::min<uint64_t>(remaining, frameCount);

    for (uint64_t i = 0; i < toRender; ++i) {
      while (nextEvent < events.size() &&
             events[nextEvent].frame <= framePos) {
        applyEvent(events[nextEvent]);
        ++nextEvent;
      }

      float left = 0.0f;
      float right = 0.0f;
      renderMixedFrame(&left, &right);
      if (channels == 1u) {
        out[i] = 0.5f * (left + right);
      } else {
        out[i * 2u] = left;
        out[i * 2u + 1u] = right;
      }
      ++framePos;
    }

    if (toRender < frameCount) {
      size_t offsetSamples = static_cast<size_t>(toRender) * channels;
      size_t zeroSamples =
          static_cast<size_t>(frameCount - toRender) * channels;
      std::fill(out + offsetSamples, out + offsetSamples + zeroSamples, 0.0f);
    }

    if (framesRead) {
      *framesRead = toRender;
    }
    return true;
  }

  bool seekToFrame(uint64_t targetFrame) {
    if (!active) return false;
    targetFrame = std::min(targetFrame, totalFrames);
    resetPlaybackState();
    framePos = targetFrame;

    // Fast seek by applying events before the target frame.
    while (nextEvent < events.size() && events[nextEvent].frame < framePos) {
      applyEvent(events[nextEvent]);
      ++nextEvent;
    }
    return true;
  }

  bool getTotalFrames(uint64_t* outFrames) const {
    if (!active || !outFrames) return false;
    *outFrames = totalFrames;
    return true;
  }

  void resetPlaybackState() {
    framePos = 0;
    nextEvent = 0;
    for (size_t i = 0; i < channelsState.size(); ++i) {
      channelsState[i] = ChannelState{};
    }
    voices.fill(Voice{});
  }

  void applyEvent(const ScheduledEvent& event) {
    if (event.channel >= channelsState.size()) return;
    switch (event.kind) {
      case ScheduledEventKind::NoteOn:
        noteOn(event.channel, event.data1, event.data2);
        break;
      case ScheduledEventKind::NoteOff:
        noteOff(event.channel, event.data1);
        break;
      case ScheduledEventKind::ProgramChange:
        channelsState[event.channel].program = event.data1;
        break;
      case ScheduledEventKind::ControlChange:
        applyControlChange(event.channel, event.data1, event.data2);
        break;
      case ScheduledEventKind::PitchBend:
        applyPitchBend(event.channel, event.data1, event.data2);
        break;
    }
  }

  size_t findVoiceSlot() const {
    for (size_t i = 0; i < voices.size(); ++i) {
      if (!voices[i].active) return i;
    }

    size_t bestIndex = 0;
    float bestScore = std::numeric_limits<float>::max();
    for (size_t i = 0; i < voices.size(); ++i) {
      const Voice& v = voices[i];
      float score = v.level;
      if (v.released) {
        score *= 0.5f;
      }
      if (score < bestScore) {
        bestScore = score;
        bestIndex = i;
      }
    }
    return bestIndex;
  }

  void releaseVoice(Voice* voice) {
    if (!voice || !voice->active) return;
    voice->released = true;
    voice->sustained = false;
    voice->keyHeld = false;
  }

  void updateVoiceFrequency(Voice* voice) {
    if (!voice || !voice->active) return;
    const ChannelState& ch = channelsState[voice->channel];
    double midi = static_cast<double>(voice->note) +
                  static_cast<double>(ch.bendSemitones());
    double hz = noteToFrequency(midi);
    double nyquistLimit =
        static_cast<double>(sampleRate > 0u ? sampleRate : 48000u) * 0.45;
    hz = std::clamp(hz, 12.0, std::max(40.0, nyquistLimit));
    voice->frequency = static_cast<float>(hz);
  }

  void noteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (velocity == 0u) {
      noteOff(channel, note);
      return;
    }

    for (Voice& v : voices) {
      if (v.active && v.channel == channel && v.note == note && v.keyHeld) {
        releaseVoice(&v);
      }
    }

    size_t slot = findVoiceSlot();
    Voice& voice = voices[slot];
    voice = Voice{};
    voice.active = true;
    voice.channel = channel;
    voice.note = note;
    voice.velocity = clamp01(static_cast<float>(velocity) / 127.0f);
    voice.keyHeld = true;
    voice.released = false;
    voice.sustained = false;
    voice.level = 0.0f;
    voice.phase = 0.0;
    voice.waveform = (channel == 9u)
                         ? Waveform::Noise
                         : waveformForProgram(channelsState[channel].program);
    voice.noiseState = 0xA3C59AC3u ^ (static_cast<uint32_t>(note) << 8u) ^
                       static_cast<uint32_t>(channel);

    float sr = static_cast<float>(sampleRate > 0u ? sampleRate : 48000u);
    float attackSec = (channel == 9u) ? 0.001f : kAttackSeconds;
    float decaySec = (channel == 9u) ? 0.060f : kDecaySeconds;
    float releaseSec = (channel == 9u) ? 0.080f : kReleaseSeconds;
    voice.sustainLevel = (channel == 9u) ? 0.30f : kSustainLevel;
    voice.attackRate = 1.0f / std::max(1.0f, attackSec * sr);
    voice.decayRate =
        (1.0f - voice.sustainLevel) / std::max(1.0f, decaySec * sr);
    voice.releaseRate = 1.0f / std::max(1.0f, releaseSec * sr);
    updateVoiceFrequency(&voice);
  }

  void noteOff(uint8_t channel, uint8_t note) {
    if (channel >= channelsState.size()) return;
    for (Voice& v : voices) {
      if (!v.active || v.channel != channel || v.note != note || !v.keyHeld) {
        continue;
      }
      v.keyHeld = false;
      if (channelsState[channel].sustain) {
        v.sustained = true;
      } else {
        releaseVoice(&v);
      }
    }
  }

  void applyPitchBend(uint8_t channel, uint8_t lsb, uint8_t msb) {
    if (channel >= channelsState.size()) return;
    uint16_t value =
        static_cast<uint16_t>((static_cast<uint16_t>(msb) << 7u) | lsb);
    channelsState[channel].pitchBend = value;
    for (Voice& v : voices) {
      if (!v.active || v.channel != channel || v.waveform == Waveform::Noise) {
        continue;
      }
      updateVoiceFrequency(&v);
    }
  }

  void releaseAllOnChannel(uint8_t channel) {
    for (Voice& v : voices) {
      if (!v.active || v.channel != channel) continue;
      releaseVoice(&v);
    }
  }

  void applyControlChange(uint8_t channel, uint8_t controller, uint8_t value) {
    if (channel >= channelsState.size()) return;
    ChannelState& st = channelsState[channel];
    switch (controller) {
      case 7:
        st.volume = value;
        break;
      case 10:
        st.pan = value;
        break;
      case 11:
        st.expression = value;
        break;
      case 64: {
        bool newSustain = value >= 64u;
        if (st.sustain && !newSustain) {
          for (Voice& v : voices) {
            if (!v.active || v.channel != channel || !v.sustained) continue;
            releaseVoice(&v);
          }
        }
        st.sustain = newSustain;
        break;
      }
      case 120:  // All sound off
      case 123:  // All notes off
        releaseAllOnChannel(channel);
        break;
      case 121:  // Reset all controllers
        st.volume = 100u;
        st.expression = 127u;
        st.pan = 64u;
        st.pitchBend = 8192u;
        st.sustain = false;
        applyPitchBend(channel, 0u, 64u);
        break;
      default:
        break;
    }
  }

  bool updateEnvelope(Voice* voice) {
    if (!voice || !voice->active) return false;

    if (voice->released) {
      voice->level = std::max(0.0f, voice->level - voice->releaseRate);
      if (voice->level <= 0.00001f) {
        voice->active = false;
        voice->level = 0.0f;
        return false;
      }
    } else {
      if (voice->level < 1.0f) {
        voice->level = std::min(1.0f, voice->level + voice->attackRate);
      }
      if (voice->level > voice->sustainLevel) {
        voice->level =
            std::max(voice->sustainLevel, voice->level - voice->decayRate);
      }
    }
    return true;
  }

  float oscillatorSample(Voice* voice) {
    if (!voice) return 0.0f;
    float phase = static_cast<float>(voice->phase);
    switch (voice->waveform) {
      case Waveform::Sine:
        return std::sin(phase * kTwoPi);
      case Waveform::Triangle:
        return 1.0f - 4.0f * std::fabs(phase - 0.5f);
      case Waveform::Saw:
        return 2.0f * phase - 1.0f;
      case Waveform::Square:
        return phase < 0.5f ? 1.0f : -1.0f;
      case Waveform::Noise:
      default:
        voice->noiseState = voice->noiseState * 1664525u + 1013904223u;
        return static_cast<float>((voice->noiseState >> 8u) & 0xFFFFu) /
                   32767.5f -
               1.0f;
    }
  }

  void advancePhase(Voice* voice) {
    if (!voice) return;
    double increment = static_cast<double>(voice->frequency) /
                       static_cast<double>(sampleRate > 0u ? sampleRate
                                                          : 48000u);
    voice->phase += increment;
    voice->phase -= std::floor(voice->phase);
  }

  void renderMixedFrame(float* outLeft, float* outRight) {
    float left = 0.0f;
    float right = 0.0f;
    for (Voice& voice : voices) {
      if (!voice.active) continue;
      if (!updateEnvelope(&voice)) continue;

      float sample = oscillatorSample(&voice);
      advancePhase(&voice);

      const ChannelState& ch = channelsState[voice.channel];
      float gain = ch.gain() * voice.velocity * voice.level;
      float pan = ch.panNorm();
      float leftGain = std::cos(pan * kHalfPi);
      float rightGain = std::sin(pan * kHalfPi);

      left += sample * gain * leftGain;
      right += sample * gain * rightGain;
    }

    if (outLeft) *outLeft = softClip(left * kMasterGain);
    if (outRight) *outRight = softClip(right * kMasterGain);
  }

  bool active = false;
  uint32_t channels = 0;
  uint32_t sampleRate = 0;
  uint64_t totalFrames = 0;
  uint64_t framePos = 0;
  size_t nextEvent = 0;
  std::vector<ScheduledEvent> events;
  std::array<ChannelState, 16> channelsState{};
  std::array<Voice, kMaxVoices> voices{};
};

MidiAudioDecoder::MidiAudioDecoder() : impl_(new Impl()) {}

MidiAudioDecoder::~MidiAudioDecoder() = default;

MidiAudioDecoder::MidiAudioDecoder(MidiAudioDecoder&&) noexcept = default;

MidiAudioDecoder& MidiAudioDecoder::operator=(MidiAudioDecoder&&) noexcept =
    default;

bool MidiAudioDecoder::init(const std::filesystem::path& path,
                            uint32_t channels,
                            uint32_t sampleRate,
                            std::string* error) {
  if (!impl_) impl_.reset(new Impl());
  return impl_->init(path, channels, sampleRate, error);
}

void MidiAudioDecoder::uninit() {
  if (!impl_) return;
  impl_->uninit();
}

bool MidiAudioDecoder::readFrames(float* out, uint32_t frameCount,
                                  uint64_t* framesRead) {
  if (!impl_) return false;
  return impl_->readFrames(out, frameCount, framesRead);
}

bool MidiAudioDecoder::seekToFrame(uint64_t frame) {
  if (!impl_) return false;
  return impl_->seekToFrame(frame);
}

bool MidiAudioDecoder::getTotalFrames(uint64_t* outFrames) const {
  if (!impl_) return false;
  return impl_->getTotalFrames(outFrames);
}

bool MidiAudioDecoder::active() const { return impl_ && impl_->active; }
