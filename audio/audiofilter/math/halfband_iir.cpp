#include "halfband_iir.h"

#include <cmath>
#include <stdexcept>

#include "hiir/PolyphaseIir2Designer.h"

void HalfbandIirCoefficients::configure(double transition) {
  if (!std::isfinite(transition) || transition < .009 || transition >= .5)
    throw std::invalid_argument("Halfband transition must be in [.009, .5).");
  count = hiir::PolyphaseIir2Designer::compute_nbr_coefs_from_proto(100, transition);
  if (count > kCapacity)
    throw std::invalid_argument("Halfband transition exceeds coefficient capacity.");
  hiir::PolyphaseIir2Designer::compute_coefs_spec_order_tbw(values.data(), count,
                                                            transition);
}
