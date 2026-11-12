#include "radio_visualization_scene.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace tui_radio_visualization {

struct Rgb { float r, g, b; };

class Canvas {
 public:
  explicit Canvas(std::vector<uint8_t>& pixels) : pixels_(pixels) {
    pixels_.resize(kWidth * kHeight * 4);
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        const float vignette = std::max(0.0f, 1.0f -
            0.0000025f * ((x - 360) * (x - 360) + (y - 220) * (y - 220)));
        put(x, y, {7 + 6 * vignette, 8 + 5 * vignette, 11 + 5 * vignette});
      }
    }
  }
  void put(int x, int y, Rgb c, float alpha = 1.0f) {
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return;
    const size_t i = (static_cast<size_t>(y) * kWidth + x) * 4;
    for (int j = 0; j < 3; ++j) {
      const float v = j == 0 ? c.r : j == 1 ? c.g : c.b;
      pixels_[i + j] = static_cast<uint8_t>(std::clamp(
          pixels_[i + j] * (1 - alpha) + v * alpha, 0.0f, 255.0f));
    }
    pixels_[i + 3] = 255;
  }
  void rect(int x0, int y0, int x1, int y1, Rgb c) {
    for (int y = std::max(0, y0); y < std::min(kHeight, y1); ++y)
      for (int x = std::max(0, x0); x < std::min(kWidth, x1); ++x)
        put(x, y, c);
  }
  void disc(int cx, int cy, int radius, Rgb c) {
    for (int y = -radius; y <= radius; ++y)
      for (int x = -radius; x <= radius; ++x)
        if (x*x + y*y <= radius*radius) put(cx+x, cy+y, c);
  }
  void line(int x0, int y0, int x1, int y1, int thickness, Rgb color) {
    const int steps = std::max(std::abs(x1-x0), std::abs(y1-y0));
    for (int i = 0; i <= steps; ++i) {
      const float t = steps ? float(i)/steps : 0;
      disc(int(x0 + t*(x1-x0)), int(y0 + t*(y1-y0)), thickness, color);
    }
  }
  void glow(int cx, int cy, int radius, float intensity) {
    intensity = std::clamp(intensity, 0.0f, 1.0f);
    for (int y = -radius; y <= radius; ++y) {
      for (int x = -radius; x <= radius; ++x) {
        const float distance = std::sqrt(float(x*x + y*y)) / radius;
        if (distance < 1) {
          const float a = (1-distance)*(1-distance)*intensity*0.65f;
          put(cx+x, cy+y, {245, 110, 28}, a);
        }
      }
    }
  }
  void screw(int x, int y) {
    disc(x, y, 5, {28, 31, 31});
    disc(x-1, y-1, 3, {146, 139, 115});
    line(x-2, y, x+2, y-2, 0, {50, 52, 51});
  }
  void can(int x, int y, int w, int h, float level) {
    rect(x+5,y+6,x+w+5,y+h+6,{20,22,23});
    rect(x,y,x+w,y+h,{86,91,87});
    rect(x+4,y+4,x+w-4,y+h-4,{47,53,53});
    rect(x+7,y+7,x+w-7,y+h-7,{65,72,69});
    line(x+8,y+8,x+w-9,y+8,1,{132,132,116});
    glow(x+w/2,y+h/2, w, 0.55f*level);
    screw(x+8,y+8); screw(x+w-9,y+h-9);
  }
  void tube(int x, int y, int radius, float level, bool powered) {
    const float activity = std::clamp(level, 0.0f, 1.0f);
    glow(x,y,radius*2,powered ? 0.04f + 0.96f*activity : 0.0f);
    disc(x+3,y+5,radius+5,{17,19,20});
    disc(x,y,radius+3,{102,99,79});
    disc(x,y,radius,{31,38,38});
    disc(x-2,y-3,radius-4,{73,77,67});
    disc(x,y,radius-8,{25,31,32});
    const float s = powered ? 0.04f + 0.96f*activity : 0.0f;
    disc(x,y,radius-13,{75+150*s,35+100*s,15+38*s});
    line(x-4,y-9,x-4,y+9,2,{42+213*s,25+170*s,12+88*s});
    line(x+4,y-9,x+4,y+9,2,{42+213*s,25+143*s,12+53*s});
    line(x-radius/2,y-radius/2,x-radius/2+7,y-radius/2-3,1,{165,173,152});
    screw(x-radius+4,y); screw(x+radius-4,y);
  }
 private:
  std::vector<uint8_t>& pixels_;
};

float level(const Levels* levels, RadioCircuitProbe stage) {
  return levels && levels->has(stage) ? levels->values[static_cast<std::size_t>(stage)] : 0.0f;
}

void drawRadio(std::vector<uint8_t>& pixels, const Levels* levels) {
  Canvas c(pixels);
  const float antenna = level(levels, RadioCircuitProbe::AntennaEnvelopeVolts);
  const float converter = level(levels, RadioCircuitProbe::ConverterEnvelopeAmps);
  const float intermediate = level(levels, RadioCircuitProbe::IfEnvelopeVolts);
  const float detector = level(levels, RadioCircuitProbe::DetectorVolts);
  const float audio = level(levels, RadioCircuitProbe::AudioPlateSwingVolts);
  const float output = level(levels, RadioCircuitProbe::SpeakerTerminalVolts);
  const float speaker = level(levels, RadioCircuitProbe::SpeakerCoilAmps);
  const float supply = level(levels, RadioCircuitProbe::SupplyVolts);

  // A deliberately simplified top view of the 38-12-inspired five-tube set.
  c.rect(22,22,698,420,{31,22,17});
  c.rect(29,29,691,413,{97,66,40});
  c.rect(39,39,681,403,{50,48,39});
  c.rect(48,48,672,394,{65,65,55});
  c.line(52,53,668,53,2,{128,119,91});
  for (auto p : {std::pair{57,58}, {660,58}, {57,384}, {660,384}})
    c.screw(p.first,p.second);

  // Wiring lies under the components. Brightness follows each connected stage.
  auto wire = [&](int x0,int y0,int x1,int y1,float v) {
    c.line(x0,y0,x1,y1,4,{24,26,23});
    c.line(x0,y0,x1,y1,2,{53+147*v,38+109*v,24+46*v});
  };
  wire(80,155,155,155,antenna);
  wire(155,155,190,155,converter);
  wire(205,155,264,106,converter);
  wire(264,106,295,155,intermediate);
  wire(295,155,350,106,intermediate);
  wire(350,106,403,155,detector);
  wire(403,155,403,240,audio);
  wire(403,240,456,274,audio);
  wire(456,274,524,274,output);
  wire(524,274,570,220,speaker);
  wire(165,320,300,320,supply);
  wire(300,320,455,315,supply);

  // Antenna coil, variable tuning capacitor, two IF cans and five valves.
  c.glow(85,153,50,antenna*0.6f);
  c.rect(68,110,104,198,{40,35,28});
  for (int y=116; y<195; y+=9)
    c.line(69,y,102,y,2,{91+83*antenna,60+48*antenna,36+13*antenna});
  c.can(119,77,57,45,antenna);
  for (int x=125;x<172;x+=7) c.line(x,84,x,115,1,{164,164,142});
  c.can(239,72,52,52,intermediate);
  c.can(325,72,52,52,intermediate);
  c.tube(195,167,29,converter,levels && levels->has(RadioCircuitProbe::ConverterEnvelopeAmps));
  c.tube(291,167,29,intermediate,levels && levels->has(RadioCircuitProbe::IfEnvelopeVolts));
  c.tube(400,167,29,std::max(detector,audio),levels &&
         (levels->has(RadioCircuitProbe::DetectorVolts) ||
          levels->has(RadioCircuitProbe::AudioPlateSwingVolts)));
  c.tube(456,280,34,output,levels && levels->has(RadioCircuitProbe::SpeakerTerminalVolts));
  c.tube(299,314,29,supply*0.35f,levels && levels->has(RadioCircuitProbe::SupplyVolts));
  c.can(115,280,93,74,supply*0.4f);
  c.can(511,253,48,50,output);
  for (int x : {228, 250, 272}) {
    c.disc(x,245,9,{34,38,35});
    c.disc(x-1,244,6,{155,136,95});
  }

  // Concentric circles are the cone, surround, basket and field-coil centre.
  c.glow(570,167,110,speaker*0.55f);
  c.disc(573,171,89,{22,24,23});
  c.disc(570,167,87,{117,110,85});
  c.disc(570,167,80,{35,38,35});
  c.disc(570,167,72,{93,78,56});
  c.disc(570,167,63,{72,62,47});
  c.disc(570,167,49,{103,85,59});
  c.disc(570,167,26,{43,40,33});
  c.disc(570,167,19,{75+100*speaker,59+70*speaker,36+32*speaker});
  for (int i=0;i<8;++i) {
    const float a = float(i)*0.785398163f;
    c.screw(570+int(std::cos(a)*83),167+int(std::sin(a)*83));
  }
}


}  // namespace tui_radio_visualization
