// panel.h — PhotoPainter panel class with a corrected busy timeout.
//
// WHY THIS EXISTS
//
// GxEPD2's GxEPD2_730c_GDEP073E01 class hardcodes a 20-second busy timeout
// (passed to the GxEPD2_EPD constructor). Waveshare specs this panel at ~25
// seconds per refresh, so the wait gives up about 5 seconds early, every time.
//
// That would be harmless on its own, but GxEPD2_7C::nextPage() does:
//
//     epd2.refresh(false);
//     epd2.powerOff();
//
// with nothing in between. So when the wait returns early, powerOff() cuts the
// panel's driving voltages part way through the refresh and the image never
// develops. Symptom: the refresh appears to "work" (no error), the reported
// _refresh time is well under 20s, and the screen either stays unchanged or
// ends up blank.
//
// _busy_timeout is protected in GxEPD2_EPD, so a subclass can raise it. That's
// all this does — everything else is inherited unchanged.

#pragma once

#include <GxEPD2_7C.h>

class PhotoPainterPanel : public GxEPD2_730c_GDEP073E01 {
  public:
    PhotoPainterPanel(int16_t cs, int16_t dc, int16_t rst, int16_t busy)
      : GxEPD2_730c_GDEP073E01(cs, dc, rst, busy) {
      // 45s, comfortably past the ~25s this panel actually needs. The wait
      // exits as soon as BUSY goes idle, so a generous value costs nothing
      // in the normal case — it only matters when something is wrong.
      _busy_timeout = 45000000UL;
    }
};
