// Copyright 2013 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
// 
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// Voice.

#include "yarns/voice.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "stmlib/midi/midi.h"
#include "stmlib/utils/dsp.h"
#include "stmlib/utils/random.h"

#include "yarns/resources.h"

namespace yarns {
  
using namespace stmlib;
using namespace stmlib_midi;

const int32_t kOctave = 12 << 7;
const int32_t kMaxNote = 120 << 7;

void Voice::Init(bool reset_calibration) {
  note_ = -1;
  note_source_ = note_target_ = note_portamento_ = 60 << 7;
  gate_ = false;
  
  mod_velocity_ = 0;
  ResetAllControllers();
  
  modulation_rate_ = 0;
  pitch_bend_range_ = 2;
  vibrato_range_ = 0;
  
  lfo_phase_ = portamento_phase_ = 0;
  portamento_phase_increment_ = 1U << 31;
  portamento_exponential_shape_ = false;
  
  trigger_duration_ = 2;
  
  if (reset_calibration) {
    for (uint8_t i = 0; i < kNumOctaves; ++i) {
      calibrated_dac_code_[i] = 54586 - 5133 * i;
    }
  }
  dirty_ = false;
  oscillator_.Init(
    calibrated_dac_code_[3] - calibrated_dac_code_[8],
    calibrated_dac_code_[3]);
}

void Voice::Calibrate(uint16_t* calibrated_dac_code) {
  std::copy(
      &calibrated_dac_code[0],
      &calibrated_dac_code[kNumOctaves],
      &calibrated_dac_code_[0]);
}

inline uint16_t Voice::NoteToDacCode(int32_t note) const {
  if (note <= 0) {
    note = 0;
  }
  if (note >= kMaxNote) {
    note = kMaxNote - 1;
  }
  uint8_t octave = 0;
  while (note >= kOctave) {
    note -= kOctave;
    ++octave;
  }
  
  // Note is now between 0 and kOctave
  // Octave indicates the octave. Look up in the DAC code table.
  int32_t a = calibrated_dac_code_[octave];
  int32_t b = calibrated_dac_code_[octave + 1];
  return a + ((b - a) * note / kOctave);
}

void Voice::ResetAllControllers() {
  mod_pitch_bend_ = 8192;
  mod_wheel_ = 0;
  std::fill(&mod_aux_[0], &mod_aux_[7], 0);
}

void Voice::Refresh() {
  // Compute base pitch with portamento.
  portamento_phase_ += portamento_phase_increment_;
  if (portamento_phase_ < portamento_phase_increment_) {
    portamento_phase_ = 0;
    portamento_phase_increment_ = 0;
    note_source_ = note_target_;
  }
  uint16_t portamento_level = portamento_exponential_shape_
      ? Interpolate824(lut_env_expo, portamento_phase_)
      : portamento_phase_ >> 16;
  int32_t note = note_source_ + \
      ((note_target_ - note_source_) * portamento_level >> 16);

  note_portamento_ = note;
  
  // Add pitch-bend.
  note += static_cast<int32_t>(mod_pitch_bend_ - 8192) * pitch_bend_range_ >> 6;
  
  // Add transposition/fine tuning.
  note += tuning_;
  
  // Add vibrato.
  if (modulation_rate_ < 100) {
    lfo_phase_ += lut_lfo_increments[modulation_rate_];
  } else {
    lfo_phase_ += lfo_pll_phase_increment_;
  }
  int32_t lfo = lfo_phase_ < 1UL << 31
      ?  -32768 + (lfo_phase_ >> 15)
      : 0x17fff - (lfo_phase_ >> 15);
  note += lfo * mod_wheel_ * vibrato_range_ >> 15;
  mod_aux_[0] = mod_velocity_ << 9;
  mod_aux_[1] = mod_wheel_ << 9;
  mod_aux_[5] = static_cast<uint16_t>(mod_pitch_bend_) << 2;
  mod_aux_[6] = (lfo * mod_wheel_ >> 7) + 32768;
  mod_aux_[7] = lfo + 32768;
  
  if (retrigger_delay_) {
    --retrigger_delay_;
  }
  
  if (trigger_pulse_) {
    --trigger_pulse_;
  }
  
  if (trigger_phase_increment_) {
    trigger_phase_ += trigger_phase_increment_;
    if (trigger_phase_ < trigger_phase_increment_) {
      trigger_phase_ = 0;
      trigger_phase_increment_ = 0;
    }
  }
  if (note != note_ || dirty_) {
    note_dac_code_ = NoteToDacCode(note);
    note_ = note;
    dirty_ = false;
  }
}

void Voice::NoteOn(
    int16_t note,
    uint8_t velocity,
    uint8_t portamento,
    bool trigger) {
  note_source_ = note_portamento_;  
  note_target_ = note;
  if (!portamento) {
    note_source_ = note_target_;
  }
  portamento_phase_ = 0;
  if (portamento <= 50) {
    portamento_phase_increment_ = lut_portamento_increments[portamento << 1];
    portamento_exponential_shape_ = true;
  } else {
    uint32_t base_increment = lut_portamento_increments[(portamento - 51) << 1];
    uint32_t delta = abs(note_target_ - note_source_) + 1;
    portamento_phase_increment_ = (1536 * (base_increment >> 11) / delta) << 11;
    CONSTRAIN(portamento_phase_increment_, 1, 2147483647);
    portamento_exponential_shape_ = false;
  }

  mod_velocity_ = velocity;

  if (gate_ && trigger) {
    retrigger_delay_ = 2;
  }
  if (trigger) {
    trigger_pulse_ = trigger_duration_ * 8;
    trigger_phase_ = 0;
    trigger_phase_increment_ = lut_portamento_increments[trigger_duration_];
  }
  gate_ = true;
}

void Voice::NoteOff() {
  gate_ = false;
}

void Voice::ControlChange(uint8_t controller, uint8_t value) {
  switch (controller) {
    case kCCModulationWheelMsb:
      mod_wheel_ = value;
      break;
    
    case kCCBreathController:
      mod_aux_[3] = value << 9;
      break;
      
    case kCCFootPedalMsb:
      mod_aux_[4] = value << 9;
      break;
  }
}

uint16_t Voice::trigger_dac_code() const {
  if (trigger_phase_ <= trigger_phase_increment_) {
    return calibrated_dac_code_[3]; // 0V.
  } else {
    int32_t velocity_coefficient = trigger_scale_ ? mod_velocity_ << 8 : 32768;
    int32_t value = 0;
    switch(trigger_shape_) {
      case TRIGGER_SHAPE_SQUARE:
        value = 32767;
        break;
      case TRIGGER_SHAPE_LINEAR:
        value = 32767 - (trigger_phase_ >> 17);
        break;
      default:
        {
          const int16_t* table = waveform_table[
              trigger_shape_ - TRIGGER_SHAPE_EXPONENTIAL];
          value = Interpolate824(table, trigger_phase_);
        }
        break;
    }
    value = value * velocity_coefficient >> 15;
    int32_t max = calibrated_dac_code_[8];
    int32_t min = calibrated_dac_code_[3];
    return min + ((max - min) * value >> 15);
  }
}

}  // namespace yarns
