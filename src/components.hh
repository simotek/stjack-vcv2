
#pragma once

struct DavidLTPort : public SVGPort {
   DavidLTPort() {
      setSVG(APP->window->loadSvg(asset::plugin(::plugin, "res/cntr_DK.svg")));
   }
};
