#ifndef RADIOIFY_COUPLED_IF_TRANSFORMER_H
#define RADIOIFY_COUPLED_IF_TRANSFORMER_H

#include <array>

// Narrowband, current-driven pair of magnetically coupled parallel LC tanks.
// State is primary voltage and secondary voltage rotated by -90 degrees.
// Both quadratures follow the same real two-state system. The caller supplies
// the envelope clock; configure performs all matrix work outside the sample loop.
// The input envelope is the driving plate current times the drive resistance:
// 1 ohm for a current input, or 1/gm for the grid voltage of a tube with
// transconductance gm.
class CoupledIfTransformer {
 public:
  void configure(double sampleRate, double centreHz, double primaryHenries,
                 double secondaryHenries, double primaryLossOhms,
                 double secondaryLossOhms, double secondaryLoadOhms, double coupling,
                 double driveResistanceOhms);
  std::array<float, 2> process(float i, float q);
  void reset();
  // Steady primary/secondary voltage ratio at the centre frequency.
  double primaryToSecondaryRatio() const { return primaryPerSecondary_; }

 private:
  double m00_ = 0, m01_ = 0, m10_ = 0, m11_ = 0;
  double b0_ = 0, b1_ = 0;
  double primaryPerSecondary_ = 1;
  std::array<double, 2> primary_{}, secondary_{}, previousInput_{};
};

#endif
