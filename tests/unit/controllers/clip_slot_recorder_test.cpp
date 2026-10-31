// SPDX-FileCopyrightText: © 2026 Hannes Janetzek <hannes.janetzek@gmail.com>
// SPDX-License-Identifier: LicenseRef-ZrythmLicense

#include "controllers/clip_slot_recorder.h"

#include <gtest/gtest.h>

namespace zrythm::controllers
{

class ClipSlotRecorderQuantizationTest : public ::testing::Test
{
protected:
  int64_t next (double tick, structure::tracks::ClipQuantizeOption quantization)
  {
    return ClipSlotRecorder::next_quantization_tick (
             tempo_map_, units::ticks (tick), quantization)
      .asQuantity ()
      .in (units::ticks);
  }

  // 4/4: one bar is 3840 ticks, one beat 960
  dsp::TempoMap tempo_map_{ units::sample_rate (44100.0) };
};

TEST_F (ClipSlotRecorderQuantizationTest, ImmediateIsTheNextTick)
{
  EXPECT_EQ (
    next (100.4, structure::tracks::ClipQuantizeOption::Immediate), 101);
  EXPECT_EQ (
    next (100.0, structure::tracks::ClipQuantizeOption::Immediate), 100);
}

TEST_F (ClipSlotRecorderQuantizationTest, NextBarStartsAtTheFollowingBar)
{
  using Q = structure::tracks::ClipQuantizeOption;
  EXPECT_EQ (next (1.0, Q::NextBar), 3840);
  EXPECT_EQ (next (3839.5, Q::NextBar), 3840);
  // A position on a bar start is its own quantization point
  EXPECT_EQ (next (3840.0, Q::NextBar), 3840);
  EXPECT_EQ (next (0.0, Q::NextBar), 0);
}

TEST_F (ClipSlotRecorderQuantizationTest, NextBeatAndNoteValues)
{
  using Q = structure::tracks::ClipQuantizeOption;
  EXPECT_EQ (next (1000.0, Q::NextBeat), 1920);
  EXPECT_EQ (next (1000.0, Q::NextQuarterNote), 1920);
  EXPECT_EQ (next (1000.0, Q::NextEighthNote), 1440);
  EXPECT_EQ (next (1000.0, Q::NextSixteenthNote), 1200);
  EXPECT_EQ (next (1000.0, Q::NextThirtySecondNote), 1080);
  EXPECT_EQ (next (960.0, Q::NextBeat), 960);
}

TEST_F (ClipSlotRecorderQuantizationTest, GridIsCappedAtTheNextBar)
{
  using Q = structure::tracks::ClipQuantizeOption;

  // In 7/8 a bar is 3360 ticks: quarter-note steps from the bar start do
  // not divide it, so the last step ends at the next bar
  tempo_map_.add_time_signature_event (
    units::ticks (int64_t{ 0 }), { .numerator = 7, .denominator = 8 });
  EXPECT_EQ (next (3000.0, Q::NextQuarterNote), 3360);
  EXPECT_EQ (next (2000.0, Q::NextQuarterNote), 2880);
}

} // namespace zrythm::controllers
