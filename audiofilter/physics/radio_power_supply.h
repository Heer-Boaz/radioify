#ifndef RADIOIFY_AUDIOFILTER_PHYSICS_RADIO_POWER_SUPPLY_H
#define RADIOIFY_AUDIOFILTER_PHYSICS_RADIO_POWER_SUPPLY_H

#include "../../radio.h"

float computePowerLoadT(const Radio1938::PowerNodeState& power);
float computePowerBranchSupplyScale(const Radio1938& radio, float branchDepth);
void advanceRectifierRipplePhase(Radio1938& radio);

#endif
