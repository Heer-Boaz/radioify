#include "radio.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void fail(const char* message) {
  std::cerr << "radio_receiver_profile_tests: " << message << '\n';
  std::exit(1);
}

void expectNear(float actual, float expected, float tolerance,
                const char* message) {
  if (!std::isfinite(actual) || std::fabs(actual - expected) > tolerance) {
    std::cerr << "radio_receiver_profile_tests: " << message
              << " expected=" << expected << " actual=" << actual << '\n';
    std::exit(1);
  }
}

void expectProfileParserContract() {
  if (kDefaultRadioReceiverProfile != RadioReceiverProfile::Typical1930s) {
    fail("typical-1930s is not the default receiver profile");
  }

  RadioReceiverProfile parsed = RadioReceiverProfile::Philco37116;
  if (!parseRadioReceiverProfile("typical-1930s", parsed) ||
      parsed != RadioReceiverProfile::Typical1930s ||
      radioReceiverProfileName(parsed) != "typical-1930s") {
    fail("typical-1930s does not round-trip");
  }
  if (!parseRadioReceiverProfile("philco-38-12", parsed) ||
      parsed != RadioReceiverProfile::Typical1930s) {
    fail("Philco 38-12 concrete alias does not select typical-1930s");
  }
  if (!parseRadioReceiverProfile("philco-37-116", parsed) ||
      parsed != RadioReceiverProfile::Philco37116 ||
      radioReceiverProfileName(parsed) != "philco-37-116") {
    fail("philco-37-116 does not round-trip");
  }
  if (parseRadioReceiverProfile("generic-old-radio", parsed)) {
    fail("receiver parser accepted an undocumented profile");
  }
}

void expectTypicalReceiverTopology() {
  Radio1938 radio;
  if (radio.receiverProfile != RadioReceiverProfile::Typical1930s ||
      radio.frontEnd.secondTunedCircuitEnabled ||
      radio.power.topology !=
          Radio1938::PowerNodeState::Topology::CapCoupledSingleEnded) {
    fail("default receiver is not the single-ended mass-market topology");
  }
  expectNear(radio.receiverCircuit.volumeControlResistanceOhms, 500000.0f,
             1.0f, "38-12 volume-control value");
  expectNear(radio.power.outputLoadResistanceOhms, 3.5f, 0.01f,
             "BO-1 voice-coil load");
  expectNear(radio.power.outputTransformerPrimaryResistanceOhms, 450.0f,
             0.1f, "BO-1 transformer primary resistance");
  expectNear(radio.speakerStage.speaker.suspensionHz, 115.0f, 0.1f,
             "five-inch speaker resonance proxy");

  radio.init(1, 48000.0f, 5000.0f, 0.012f);
  if (radio.power.nominalOutputPowerWatts < 1.5f ||
      radio.power.nominalOutputPowerWatts > 2.5f) {
    fail("typical receiver did not initialize near its documented 2 W output");
  }
  if (!std::isfinite(radio.output.digitalReferenceSpeakerVoltsPeak) ||
      radio.output.digitalReferenceSpeakerVoltsPeak <= 0.0f) {
    fail("typical receiver has no finite speaker reference");
  }
}

void expectInitializedProfileSwitchRebuildsPhysicalModel() {
  Radio1938 radio;
  radio.init(1, 48000.0f, 5000.0f, 0.012f);
  radio.applyReceiverProfile(RadioReceiverProfile::Philco37116);
  if (radio.receiverProfile != RadioReceiverProfile::Philco37116 ||
      !radio.frontEnd.secondTunedCircuitEnabled ||
      radio.power.topology !=
          Radio1938::PowerNodeState::Topology::TransformerCoupledPushPull ||
      radio.power.nominalOutputPowerWatts < 12.0f ||
      radio.power.nominalOutputPowerWatts > 18.0f) {
    fail("initialized switch did not rebuild the Philco physical model");
  }

  radio.applyReceiverProfile(RadioReceiverProfile::Typical1930s);
  if (radio.receiverProfile != RadioReceiverProfile::Typical1930s ||
      radio.frontEnd.secondTunedCircuitEnabled ||
      radio.power.topology !=
          Radio1938::PowerNodeState::Topology::CapCoupledSingleEnded ||
      radio.power.nominalOutputPowerWatts < 1.5f ||
      radio.power.nominalOutputPowerWatts > 2.5f) {
    fail("switching back did not restore the typical physical model");
  }
}

}  // namespace

int main() {
  expectProfileParserContract();
  expectTypicalReceiverTopology();
  expectInitializedProfileSwitchRebuildsPhysicalModel();
  return 0;
}
