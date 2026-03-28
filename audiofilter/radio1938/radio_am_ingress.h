#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_RADIO_AM_INGRESS_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_RADIO_AM_INGRESS_H

struct RadioAmIngressConfig {
  float receivedCarrierRmsVolts = 0.12f;
  float modulationIndex = 0.85f;
};

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_RADIO_AM_INGRESS_H
