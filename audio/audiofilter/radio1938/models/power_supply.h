#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_POWER_SUPPLY_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_POWER_SUPPLY_H
#include "../../../radio.h"
void configurePowerSupply(Radio1938& radio);
void resetPowerSupply(Radio1938& radio);
void advancePowerSupply(Radio1938& radio, double outputPlateAmps, double driverPlateAmps);
#endif
