#pragma once

#include <Adafruit_GFX.h>
#include "V3AnimationFrames.h"

enum class DepartureGuideState : uint8_t {
  resting,
  packing,
  walking,
  running,
  driving,
  changingTrain,
};

inline DepartureGuideState departureGuideState(uint32_t secondsRemaining) {
  if (secondsRemaining < 5 * 60) return DepartureGuideState::changingTrain;
  if (secondsRemaining < 11 * 60) return DepartureGuideState::driving;
  if (secondsRemaining < 14 * 60) return DepartureGuideState::running;
  if (secondsRemaining < 22 * 60) return DepartureGuideState::walking;
  if (secondsRemaining < 24 * 60) return DepartureGuideState::packing;
  return DepartureGuideState::resting;
}

inline const char *departureGuideLabel(DepartureGuideState state) {
  switch (state) {
    case DepartureGuideState::resting: return "VAR ATPUSTIES";
    case DepartureGuideState::packing: return "LIEK SOMU";
    case DepartureGuideState::walking: return "EJ UZ STACIJU";
    case DepartureGuideState::running: return "JASKRIEN";
    case DepartureGuideState::driving: return "BRAUC AR AUTO";
    case DepartureGuideState::changingTrain: return "VILCIENS AIZBRAUC";
  }
  return "";
}

// PNG source frames are stored in flash as compact 1-bit bitmaps. The
// say-goodbye sequence is used while packing and as the short goodbye to a
// departing train; it is the natural bridge between the supplied sequences.
inline void drawDepartureGuideAnimation(Adafruit_GFX &display,
                                        DepartureGuideState state, int16_t x,
                                        int16_t y, uint32_t nowMs) {
  const uint8_t *frames = v3_sitting_frames[0];
  uint8_t frameCount = V3_SITTING_FRAME_COUNT;
  uint16_t frameDurationMs = 110;

  switch (state) {
    case DepartureGuideState::resting:
      break;
    case DepartureGuideState::packing:
    case DepartureGuideState::changingTrain:
      frames = v3_goodbye_frames[0];
      frameCount = V3_GOODBYE_FRAME_COUNT;
      frameDurationMs = 95;
      break;
    case DepartureGuideState::walking:
      frames = v3_walking_frames[0];
      frameCount = V3_WALKING_FRAME_COUNT;
      frameDurationMs = 85;
      break;
    case DepartureGuideState::running:
      frames = v3_running_frames[0];
      frameCount = V3_RUNNING_FRAME_COUNT;
      frameDurationMs = 65;
      break;
    case DepartureGuideState::driving:
      frames = v3_car_frames[0];
      frameCount = V3_CAR_FRAME_COUNT;
      frameDurationMs = 70;
      break;
  }

  const uint8_t frame = (nowMs / frameDurationMs) % frameCount;
  display.drawBitmap(x, y, frames + frame * V3_ANIMATION_FRAME_BYTES,
                     V3_ANIMATION_WIDTH, V3_ANIMATION_HEIGHT, SSD1306_WHITE);
}
