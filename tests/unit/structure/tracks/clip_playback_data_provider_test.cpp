// SPDX-FileCopyrightText: © 2025 Alexandros Theodotou <alex@zrythm.org>
// SPDX-License-Identifier: LicenseRef-ZrythmLicense

#include "dsp/midi_event.h"
#include "dsp/tempo_map.h"
#include "dsp/tempo_map_qml_adapter.h"
#include "structure/arrangement/arranger_object_all.h"
#include "structure/arrangement/midi_clip.h"
#include "structure/arrangement/midi_note.h"
#include "structure/tracks/clip_playback_data_provider.h"
#include "utils/app_settings.h"
#include "utils/midi.h"
#include "utils/object_registry.h"
#include "utils/registry_utils.h"
#include "utils/types.h"

#include "helpers/in_memory_settings_backend.h"

#include <gtest/gtest.h>

namespace zrythm::structure::tracks
{

class ClipPlaybackDataProviderTest : public ::testing::Test
{
protected:
  void SetUp () override
  {
    // Create a tempo map for testing
    tempo_map_ = std::make_unique<dsp::TempoMap> (units::sample_rate (44100));
    tempo_map_wrapper_ = std::make_unique<dsp::TempoMapWrapper> (*tempo_map_);

    // Create the provider
    provider_ = std::make_unique<ClipPlaybackDataProvider> (*tempo_map_);

    // Create an object registry
    registry_ = std::make_unique<utils::ObjectRegistry> ();
    app_settings_ = std::make_unique<utils::AppSettings> (
      std::make_unique<test_helpers::InMemorySettingsBackend> ());
  }

  void TearDown () override
  {
    clip_refs.clear (); // Clear references first
    provider_.reset ();
    tempo_map_.reset ();
    tempo_map_wrapper_.reset ();
    registry_.reset ();
    app_settings_.reset ();
  }

  // Helper function to create a MIDI clip with notes at specific sample
  // positions
  arrangement::MidiClip * create_midi_clip_with_sampled_notes (
    double                                        start_pos_ticks,
    double                                        end_pos_ticks,
    const std::vector<std::tuple<int, int, int>> &notes) // pitch, velocity,
                                                         // position in samples
  {
    // Create a MIDI clip
    auto clip_ref = utils::create_object<arrangement::MidiClip> (
      *registry_, *tempo_map_wrapper_, *registry_);
    auto clip = clip_ref.get_object_as<arrangement::MidiClip> ();

    // Set the clip's position
    clip->position ()->setTicks (start_pos_ticks);
    clip->length ()->setTicks (end_pos_ticks - start_pos_ticks);

    // Create MIDI notes
    for (const auto &[pitch, velocity, sample_pos] : notes)
      {
        auto note_ref = utils::create_object<arrangement::MidiNote> (
          *registry_, *tempo_map_wrapper_);
        auto midi_note = note_ref.get_object_as<arrangement::MidiNote> ();

        // Set the note's properties
        midi_note->setPitch (pitch);
        midi_note->setVelocity (velocity);
        // Convert sample position to ticks
        midi_note->position ()->setTicks (
          tempo_map_->samples_to_tick (units::samples (sample_pos)).asDouble ());
        midi_note->length ()->setTicks (50.0); // Note duration

        // Add the note to the clip
        clip->ArrangerObjectOwner<structure::arrangement::MidiNote>::add_object (
          note_ref);
      }

    // Keep a reference to the clip
    clip_refs.push_back (std::move (clip_ref));

    return clip;
  }

  // Simplified helper for creating a clip with a single note at position 0
  arrangement::MidiClip *
  create_simple_region (midi_byte_t pitch = 60, midi_byte_t velocity = 64)
  {
    return create_midi_clip_with_sampled_notes (
      0.0, 400.0,
      {
        { pitch, velocity, 0 }
    });
  }

  // Simplified helper for creating a clip with multiple notes at position 0
  arrangement::MidiClip * create_multi_note_region (
    const std::vector<std::pair<int, int>> &notes) // pitch, velocity pairs
  {
    std::vector<std::tuple<int, int, int>> sampled_notes;
    for (const auto &[pitch, velocity] : notes)
      {
        sampled_notes.emplace_back (pitch, velocity, 0);
      }
    return create_midi_clip_with_sampled_notes (0.0, 400.0, sampled_notes);
  }

  // Helper function to find events with specific pitch
  std::vector<units::sample_u32_t>
  find_event_timestamps (const dsp::MidiEventBuffer &events, midi_byte_t pitch)
  {
    std::vector<units::sample_u32_t> timestamps;
    for (const auto &event : events)
      {
        if (
          utils::midi::midi_is_note_on (event.data ()) // Note on
          && utils::midi::midi_get_note_number (event.data ()) == pitch) // Matching
                                                                         // pitch
          {
            timestamps.push_back (event.time ());
          }
      }
    return timestamps;
  }

  // Helper function to check for timestamp overflow
  bool has_timestamp_overflow (const dsp::MidiEventBuffer &events)
  {
    return std::ranges::any_of (events, [] (const auto &ev) {
      // Check if timestamp is greater than a reasonable maximum
      // (e.g., 1 second of audio at 44100Hz = 44100 samples)
      return ev.time () > units::samples (44100);
    });
  }

  // Helper to create dsp::graph::ProcessBlockInfo
  static dsp::graph::ProcessBlockInfo create_time_info (
    uint64_t transport_position,
    uint64_t buffer_offset,
    uint32_t nframes)
  {
    return {
      .transport_position_ = units::samples (transport_position),
      .buffer_offset_ = units::samples (buffer_offset),
      .nframes_ = units::samples (nframes)
    };
  }

  // Helper function to create a simple audio clip
  arrangement::AudioClip * create_simple_audio_clip (float gain = 1.0f)
  {
    // Create a simple audio buffer with test data
    auto sample_buffer = std::make_unique<utils::audio::AudioBuffer> (2, 1024);

    // Fill with test data: left channel = 0.5, right channel = -0.5
    for (int i = 0; i < sample_buffer->getNumSamples (); i++)
      {
        sample_buffer->setSample (0, i, 0.5f * gain);
        sample_buffer->setSample (1, i, -0.5f * gain);
      }

    // Create a mock audio source
    auto source_ref = utils::create_object<dsp::FileAudioSource> (
      *registry_, *sample_buffer, utils::audio::BitDepth::BIT_DEPTH_32,
      units::sample_rate (44100), units::bpm (120.0), u8"DummySource");
    auto audio_source_object_ref =
      utils::create_object<arrangement::AudioSourceObject> (
        *registry_, *tempo_map_wrapper_, *registry_, source_ref);

    // Create the audio clip
    auto clip_ref = utils::create_object<arrangement::AudioClip> (
      *registry_, *tempo_map_wrapper_, *registry_);
    auto clip = clip_ref.get_object_as<arrangement::AudioClip> ();

    clip->set_source (audio_source_object_ref);
    clip->setGain (gain);

    // Set the clip's position
    clip->position ()->setTicks (0.0);
    clip->length ()->setTicks (400.0);

    // Keep a reference to the clip
    clip_refs.push_back (std::move (clip_ref));

    return clip;
  }

  // Helper function to create a short audio clip
  arrangement::AudioClip *
  create_short_audio_clip (int num_samples, float gain = 1.0f)
  {
    // Create a short audio buffer with test data
    auto sample_buffer =
      std::make_unique<utils::audio::AudioBuffer> (2, num_samples);

    // Fill with test data: left channel = 0.5, right channel = -0.5
    for (int i = 0; i < sample_buffer->getNumSamples (); i++)
      {
        sample_buffer->setSample (0, i, 0.5f * gain);
        sample_buffer->setSample (1, i, -0.5f * gain);
      }

    // Create a mock audio source
    auto source_ref = utils::create_object<dsp::FileAudioSource> (
      *registry_, *sample_buffer, utils::audio::BitDepth::BIT_DEPTH_32,
      units::sample_rate (44100), units::bpm (120.0), u8"ShortSource");
    auto audio_source_object_ref =
      utils::create_object<arrangement::AudioSourceObject> (
        *registry_, *tempo_map_wrapper_, *registry_, source_ref);

    // Create the audio clip
    auto clip_ref = utils::create_object<arrangement::AudioClip> (
      *registry_, *tempo_map_wrapper_, *registry_);
    auto clip = clip_ref.get_object_as<arrangement::AudioClip> ();

    clip->set_source (audio_source_object_ref);
    clip->setGain (gain);

    // Set the clip's position
    clip->position ()->setTicks (0.0);
    clip->length ()->setTicks (1.0); // Very short clip

    // Keep a reference to the clip
    clip_refs.push_back (std::move (clip_ref));

    return clip;
  }

  // Helper function to create a sine wave audio clip
  arrangement::AudioClip *
  create_sine_wave_audio_clip (int num_samples, float gain = 1.0f)
  {
    // Create a sine wave audio buffer
    auto sample_buffer =
      std::make_unique<utils::audio::AudioBuffer> (2, num_samples);
    const double sample_rate = 44100.0;
    const double frequency_hz = 441.0; // 441 Hz divides evenly into 44100 Hz
    const double phase_increment = 2.0 * M_PI * frequency_hz / sample_rate;
    const double phase_offset = M_PI / 4.0; // Start at 45 degrees to avoid zero

    for (int i = 0; i < num_samples; ++i)
      {
        const double phase = phase_offset + i * phase_increment;
        const float sample_value = static_cast<float> (std::sin (phase)) * gain;

        // Left channel: sine wave
        sample_buffer->setSample (0, i, sample_value);
        // Right channel: cosine wave (90 degrees phase shift)
        sample_buffer->setSample (
          1, i, static_cast<float> (std::cos (phase)) * gain);
      }

    // Create a mock audio source
    auto source_ref = utils::create_object<dsp::FileAudioSource> (
      *registry_, *sample_buffer, utils::audio::BitDepth::BIT_DEPTH_32,
      units::sample_rate (44100), units::bpm (120.0), u8"SineSource");
    auto audio_source_object_ref =
      utils::create_object<arrangement::AudioSourceObject> (
        *registry_, *tempo_map_wrapper_, *registry_, source_ref);

    // Create the audio clip
    auto clip_ref = utils::create_object<arrangement::AudioClip> (
      *registry_, *tempo_map_wrapper_, *registry_);
    auto clip = clip_ref.get_object_as<arrangement::AudioClip> ();

    clip->set_source (audio_source_object_ref);
    clip->setGain (gain);

    // Set the clip's position
    clip->position ()->setTicks (0.0);
    clip->length ()->setTicks (
      tempo_map_->samples_to_tick (units::samples (num_samples)).asDouble ());

    // Keep a reference to the clip
    clip_refs.push_back (std::move (clip_ref));

    return clip;
  }

  std::unique_ptr<ClipPlaybackDataProvider>             provider_;
  std::unique_ptr<dsp::TempoMap>                        tempo_map_;
  std::unique_ptr<dsp::TempoMapWrapper>                 tempo_map_wrapper_;
  std::unique_ptr<utils::ObjectRegistry>                registry_;
  std::unique_ptr<utils::AppSettings>                   app_settings_;
  std::vector<arrangement::ArrangerObjectUuidReference> clip_refs; // Keep
                                                                   // references
};

TEST_F (ClipPlaybackDataProviderTest, InitialState)
{
  // Provider should start with no events
  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
  dsp::graph::ProcessBlockInfo time_info = create_time_info (0, 0, 256);

  provider_->process_midi_events (time_info, output_buffer);
  EXPECT_EQ (output_buffer.size (), 0);
}

TEST_F (ClipPlaybackDataProviderTest, GenerateEventsWithMidiClip)
{
  // Create a MIDI clip
  auto clip = create_simple_region ();

  // Generate events for the clip
  provider_->generate_midi_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);

  // Test processing events that should include the note
  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
  dsp::graph::ProcessBlockInfo time_info = create_time_info (0, 0, 256);

  provider_->process_midi_events (time_info, output_buffer);

  // Should have at least one event (the note on)
  EXPECT_GE (output_buffer.size (), 1);

  // Check that the event timestamp is within the expected range
  if (!output_buffer.empty ())
    {
      const auto &event = output_buffer.front ();
      EXPECT_GE (event.time (), units::samples (0));
      EXPECT_LT (event.time (), time_info.nframes_);

      // Verify it's a note on event with the correct pitch
      EXPECT_TRUE (utils::midi::midi_is_note_on (event.data ())); // Note on
      EXPECT_EQ (utils::midi::midi_get_note_number (event.data ()), 60); // Pitch
    }
}

TEST_F (ClipPlaybackDataProviderTest, ClearEvents)
{
  // Create a MIDI clip
  auto clip = create_simple_region ();

  // Generate events for the clip
  provider_->generate_midi_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);

  // Test processing events - should have events
  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
  dsp::graph::ProcessBlockInfo time_info = create_time_info (0, 0, 256);

  provider_->process_midi_events (time_info, output_buffer);
  EXPECT_GT (output_buffer.size (), 0);

  // Stop playback
  provider_->queue_stop_playback (
    structure::tracks::ClipQuantizeOption::Immediate);

  // Test processing events again - should have no events other than all notes off
  output_buffer.clear ();
  provider_->process_midi_events (time_info, output_buffer);
  EXPECT_EQ (output_buffer.size (), 16);
}

TEST_F (ClipPlaybackDataProviderTest, QuantizeOptions)
{
  // Create a MIDI clip
  auto clip = create_simple_region ();

  // Test different quantize options
  std::vector<ClipQuantizeOption> quantize_options = {
    structure::tracks::ClipQuantizeOption::Immediate,
    structure::tracks::ClipQuantizeOption::NextBar,
    structure::tracks::ClipQuantizeOption::NextBeat,
    structure::tracks::ClipQuantizeOption::NextQuarterNote,
    structure::tracks::ClipQuantizeOption::NextEighthNote,
    structure::tracks::ClipQuantizeOption::NextSixteenthNote,
    structure::tracks::ClipQuantizeOption::NextThirtySecondNote
  };

  for (const auto &quantize_option : quantize_options)
    {
      // Generate events with the quantize option
      provider_->generate_midi_events (*clip, quantize_option);

      // Test processing events
      auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
      dsp::graph::ProcessBlockInfo time_info = create_time_info (0, 0, 256);

      provider_->process_midi_events (time_info, output_buffer);

      // Should have events (quantize option doesn't affect the presence of
      // events, just when they start playing)
      if (quantize_option == structure::tracks::ClipQuantizeOption::Immediate)
        {
          EXPECT_GT (output_buffer.size (), 0);
        }
    }
}

TEST_F (ClipPlaybackDataProviderTest, NoteValueQuantizationStartsOnNextGridPoint)
{
  const std::array<std::pair<structure::tracks::ClipQuantizeOption, double>, 4>
    options_and_grid_ticks = {
      {
       { structure::tracks::ClipQuantizeOption::NextQuarterNote, 960.0 },
       { structure::tracks::ClipQuantizeOption::NextEighthNote, 480.0 },
       { structure::tracks::ClipQuantizeOption::NextSixteenthNote, 240.0 },
       { structure::tracks::ClipQuantizeOption::NextThirtySecondNote, 120.0 },
       }
  };

  for (const auto &[quantize_option, grid_ticks] : options_and_grid_ticks)
    {
      ClipPlaybackDataProvider provider (*tempo_map_);
      provider.generate_midi_events (*create_simple_region (), quantize_option);

      // Third grid point in the second bar
      const auto grid_point = static_cast<uint64_t> (
        tempo_map_
          ->tick_to_samples_rounded (
            dsp::TimelineTick{ units::ticks (3840.0 + (3.0 * grid_ticks)) })
          .in (units::sample));

      // The block just before the grid point stays silent
      auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
      provider.process_midi_events (
        create_time_info (grid_point - 132, 0, 32), output_buffer);
      EXPECT_TRUE (find_event_timestamps (output_buffer, 60).empty ())
        << "grid " << grid_ticks;

      // The block containing it starts the clip on the grid point
      output_buffer.clear ();
      provider.process_midi_events (
        create_time_info (grid_point - 100, 0, 256), output_buffer);
      const auto pitch_60_events = find_event_timestamps (output_buffer, 60);
      ASSERT_EQ (pitch_60_events.size (), 1) << "grid " << grid_ticks;
      EXPECT_EQ (pitch_60_events[0], units::samples (100))
        << "grid " << grid_ticks;
    }
}

TEST_F (
  ClipPlaybackDataProviderTest,
  NoteValueQuantizationStartsAtBlockStartOnGridPoint)
{
  provider_->generate_midi_events (
    *create_simple_region (),
    structure::tracks::ClipQuantizeOption::NextSixteenthNote);

  // Block starting exactly on a sixteenth (tick 240)
  const auto grid_point = static_cast<uint64_t> (
    tempo_map_
      ->tick_to_samples_rounded (dsp::TimelineTick{ units::ticks (240.0) })
      .in (units::sample));
  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
  provider_->process_midi_events (
    create_time_info (grid_point, 0, 256), output_buffer);

  const auto pitch_60_events = find_event_timestamps (output_buffer, 60);
  ASSERT_EQ (pitch_60_events.size (), 1);
  EXPECT_EQ (pitch_60_events[0], units::samples (0));
}

namespace
{
struct TimedNote
{
  int         pitch;
  int64_t     sample;
  bool        on;
  friend bool operator== (const TimedNote &, const TimedNote &) = default;
  friend std::ostream &operator<< (std::ostream &os, const TimedNote &note)
  {
    return os << (note.on ? "on " : "off ") << note.pitch << "@" << note.sample;
  }
};
}

class ClipPlaybackDataProviderLoopTest : public ClipPlaybackDataProviderTest
{
protected:
  // MIDI clip with notes given as (pitch, position ticks, length ticks)
  arrangement::MidiClip * create_midi_clip_with_tick_notes (
    double                                              length_ticks,
    const std::vector<std::tuple<int, double, double>> &notes)
  {
    auto * clip = create_midi_clip_with_sampled_notes (0.0, length_ticks, {});
    for (const auto &[pitch, position, note_length] : notes)
      {
        auto note_ref = utils::create_object<arrangement::MidiNote> (
          *registry_, *tempo_map_wrapper_);
        auto * note = note_ref.get_object_as<arrangement::MidiNote> ();
        note->setPitch (pitch);
        note->setVelocity (90);
        note->position ()->setTicks (position);
        note->length ()->setTicks (note_length);
        clip->ArrangerObjectOwner<arrangement::MidiNote>::add_object (note_ref);
      }
    return clip;
  }

  // Plays the clip launched immediately at sample 0 for @p total_samples and
  // returns note-ons and note-offs with absolute sample positions
  std::vector<TimedNote>
  play (const arrangement::MidiClip &clip, int64_t total_samples)
  {
    provider_->generate_midi_events (
      clip, structure::tracks::ClipQuantizeOption::Immediate);
    std::vector<TimedNote> notes;
    constexpr int64_t      block = 256;
    for (int64_t start = 0; start < total_samples; start += block)
      {
        auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
        provider_->process_midi_events (
          create_time_info (static_cast<uint64_t> (start), 0, block),
          output_buffer);
        for (const auto &event : output_buffer)
          {
            const auto data = event.data ();
            const bool is_on = utils::midi::midi_is_note_on (data);
            if (is_on || utils::midi::midi_is_note_off (data))
              {
                notes.push_back (
                  TimedNote{
                    .pitch = utils::midi::midi_get_note_number (data),
                    .sample = start + event.time ().in<int64_t> (units::samples),
                    .on = is_on });
              }
          }
      }
    std::erase_if (notes, [&] (const TimedNote &note) {
      return note.sample >= total_samples;
    });
    return notes;
  }

  // Compares note lists, allowing event times to differ by one sample of
  // rounding
  static void expect_notes_near (
    const std::vector<TimedNote> &actual,
    const std::vector<TimedNote> &expected)
  {
    ASSERT_EQ (actual.size (), expected.size ())
      << ::testing::PrintToString (actual);
    for (const auto i : std::views::iota (0uz, actual.size ()))
      {
        EXPECT_EQ (actual[i].pitch, expected[i].pitch) << "event " << i;
        EXPECT_EQ (actual[i].on, expected[i].on) << "event " << i;
        EXPECT_LE (std::abs (actual[i].sample - expected[i].sample), 1)
          << "event " << i << ": " << actual[i] << " vs " << expected[i];
      }
  }

  int64_t samples_at (double ticks) const
  {
    return tempo_map_
      ->tick_to_samples_rounded (dsp::TimelineTick{ units::ticks (ticks) })
      .in<int64_t> (units::samples);
  }

  static std::vector<TimedNote> note_ons (const std::vector<TimedNote> &notes)
  {
    return notes | std::views::filter (&TimedNote::on)
           | std::ranges::to<std::vector> ();
  }
};

TEST_F (ClipPlaybackDataProviderLoopTest, PlaysFromClipStartThenRepeatsLoop)
{
  // Clip markers: clip start 480, loop 960..1920 (clip length 3840 is not
  // used by the launcher)
  auto * clip = create_midi_clip_with_tick_notes (
    3840.0,
    {
      { 60, 0.0,    100.0 }, // before the clip start: never plays
      { 62, 480.0,  100.0 }, // intro only
      { 64, 960.0,  100.0 }, // loop start
      { 65, 1200.0, 100.0 }, // inside the loop
      { 67, 2400.0, 100.0 }, // after the loop end: never plays
  });
  clip->set_loop_range (
    dsp::ContentTick{ units::ticks (480.0) },
    dsp::ContentTick{ units::ticks (960.0) },
    dsp::ContentTick{ units::ticks (1920.0) });

  // Playback ticks: intro 0..1440 (clip 480..1920), then the loop repeats
  // every 960 ticks
  const auto notes = note_ons (play (*clip, samples_at (4000.0)));
  const std::vector<TimedNote> expected = {
    { 62, samples_at (0.0),    true },
    { 64, samples_at (480.0),  true },
    { 65, samples_at (720.0),  true },
    { 64, samples_at (1440.0), true },
    { 65, samples_at (1680.0), true },
    { 64, samples_at (2400.0), true },
    { 65, samples_at (2640.0), true },
    { 64, samples_at (3360.0), true },
    { 65, samples_at (3600.0), true },
  };
  expect_notes_near (notes, expected);
}

TEST_F (
  ClipPlaybackDataProviderLoopTest,
  ClipStartAfterLoopStartPlaysFullLoopAfterIntro)
{
  // Clip start 1440 lies inside the loop 960..1920
  auto * clip = create_midi_clip_with_tick_notes (
    1920.0,
    {
      { 64, 960.0,  100.0 },
      { 65, 1500.0, 100.0 },
  });
  clip->set_loop_range (
    dsp::ContentTick{ units::ticks (1440.0) },
    dsp::ContentTick{ units::ticks (960.0) },
    dsp::ContentTick{ units::ticks (1920.0) });

  // Intro 0..480 (clip 1440..1920), then loops of 960 from the loop start
  const auto notes = note_ons (play (*clip, samples_at (2400.0)));
  const std::vector<TimedNote> expected = {
    { 65, samples_at (60.0),   true },
    { 64, samples_at (480.0),  true },
    { 65, samples_at (1020.0), true },
    { 64, samples_at (1440.0), true },
    { 65, samples_at (1980.0), true },
  };
  expect_notes_near (notes, expected);
}

TEST_F (ClipPlaybackDataProviderLoopTest, EventTimesFollowTempoChanges)
{
  // 120 BPM up to tick 960, 60 BPM afterwards
  tempo_map_->add_tempo_event (
    units::ticks (960), units::bpm (60.0), dsp::TempoMap::CurveType::Constant);

  auto * clip = create_midi_clip_with_tick_notes (
    3840.0,
    {
      { 60, 0.0,    100.0 },
      { 62, 960.0,  100.0 },
      { 64, 1920.0, 100.0 },
  });

  const auto notes = note_ons (play (*clip, samples_at (2000.0)));
  const std::vector<TimedNote> expected = {
    { 60, samples_at (0.0),    true },
    { 62, samples_at (960.0),  true },
    { 64, samples_at (1920.0), true },
  };
  expect_notes_near (notes, expected);
  // 960 ticks at 60 BPM take twice as long as at 120 BPM
  EXPECT_EQ (samples_at (1920.0) - samples_at (960.0), 2 * samples_at (960.0));
}

TEST_F (ClipPlaybackDataProviderLoopTest, AlignedLaunchStartsInsideTheClip)
{
  auto * clip = create_midi_clip_with_tick_notes (
    960.0,
    {
      { 60, 0.0,   100.0 },
      { 62, 480.0, 100.0 },
  });

  // Launched at tick 240 but aligned to tick 0: the note at tick 0 has
  // passed, the one at tick 480 plays where it falls on the timeline
  provider_->generate_midi_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate, units::samples (0));
  const auto             block_start = samples_at (240.0);
  std::vector<TimedNote> notes;
  for (int64_t start = block_start; start < samples_at (960.0); start += 256)
    {
      auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
      provider_->process_midi_events (
        create_time_info (static_cast<uint64_t> (start), 0, 256), output_buffer);
      for (const auto &event : output_buffer)
        {
          if (utils::midi::midi_is_note_on (event.data ()))
            {
              notes.push_back (
                TimedNote{
                  .pitch = utils::midi::midi_get_note_number (event.data ()),
                  .sample = start + event.time ().in<int64_t> (units::samples),
                  .on = true });
            }
        }
    }
  // Notes up to the loop end at tick 960
  std::erase_if (notes, [&] (const TimedNote &note) {
    return note.sample >= samples_at (960.0);
  });
  expect_notes_near (
    notes,
    {
      { 62, samples_at (480.0), true }
  });
  EXPECT_TRUE (provider_->playing ());
}

TEST_F (ClipPlaybackDataProviderTest, AudioAlignedLaunchStartsInsideTheClip)
{
  auto * clip = create_simple_audio_clip ();

  // Reference: launched at sample 0, rendering samples 100..356
  ClipPlaybackDataProvider reference_provider (*tempo_map_);
  reference_provider.generate_audio_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);
  std::vector<float> scratch_left (100, 0.0f);
  std::vector<float> scratch_right (100, 0.0f);
  reference_provider.process_audio_events (
    create_time_info (0, 0, 100), scratch_left, scratch_right);
  std::vector<float> reference_left (256, 0.0f);
  std::vector<float> reference_right (256, 0.0f);
  reference_provider.process_audio_events (
    create_time_info (100, 0, 256), reference_left, reference_right);

  // Launched at sample 100, aligned to sample 0
  provider_->generate_audio_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate, units::samples (0));
  std::vector<float> output_left (256, 0.0f);
  std::vector<float> output_right (256, 0.0f);
  provider_->process_audio_events (
    create_time_info (100, 0, 256), output_left, output_right);

  for (const auto i : std::views::iota (0uz, output_left.size ()))
    {
      EXPECT_FLOAT_EQ (output_left[i], reference_left[i]) << "at sample " << i;
      EXPECT_FLOAT_EQ (output_right[i], reference_right[i]) << "at sample " << i;
    }
}

TEST_F (ClipPlaybackDataProviderLoopTest, NoteEndingAtLoopEndIsReleasedEveryLoop)
{
  // A note lasting until the loop end
  auto * clip = create_midi_clip_with_tick_notes (
    960.0,
    {
      { 60, 480.0, 480.0 }
  });

  // Play four loops, plus a little to include the last note-off
  const auto notes = play (*clip, samples_at (960.0 * 4) + 512);
  const auto ons = std::ranges::count_if (notes, &TimedNote::on);
  const auto offs = std::ranges::count_if (notes, [] (const TimedNote &n) {
    return !n.on;
  });
  EXPECT_EQ (ons, 4);
  EXPECT_EQ (offs, 4);
  // Each note-off comes before the next note-on
  for (const auto i : std::views::iota (0uz, notes.size ()))
    EXPECT_EQ (notes[i].on, i % 2 == 0) << "event " << i;
}

TEST_F (ClipPlaybackDataProviderTest, MultipleNotesInRegion)
{
  // Create a MIDI clip with multiple notes
  std::vector<std::pair<int, int>> notes = {
    { 60, 64  },
    { 64, 80  },
    { 67, 100 }
  };
  auto clip = create_multi_note_region (notes);

  // Generate events for the clip
  provider_->generate_midi_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);

  // Test processing events that should include all notes
  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
  dsp::graph::ProcessBlockInfo time_info = create_time_info (0, 0, 256);

  provider_->process_midi_events (time_info, output_buffer);

  // Should have at least 3 events (the note ons)
  EXPECT_GE (output_buffer.size (), 3);

  // Check that we have the expected pitches
  std::vector<int> found_pitches;
  for (const auto &event : output_buffer)
    {
      if (utils::midi::midi_is_note_on (event.data ())) // Note on
        {
          found_pitches.push_back (
            utils::midi::midi_get_note_number (event.data ()));
        }
    }

  EXPECT_TRUE (std::ranges::contains (found_pitches, 60));
  EXPECT_TRUE (std::ranges::contains (found_pitches, 64));
  EXPECT_TRUE (std::ranges::contains (found_pitches, 67));
}

TEST_F (ClipPlaybackDataProviderTest, PreciseLoopingBehavior)
{
  // Create a very short MIDI clip that will loop (approximately 23 samples)
  std::vector<std::tuple<int, int, int>> notes = {
    { 60, 64, 0 }  // Note at position 0
  };
  auto clip = create_midi_clip_with_sampled_notes (0.0, 1.0, notes);

  // Generate events for the clip
  provider_->generate_midi_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);

  // Process multiple buffers to trigger looping
  std::vector<std::vector<units::sample_u32_t>> loop_events;
  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();

  // Process first buffer
  dsp::graph::ProcessBlockInfo time_info1 = create_time_info (0, 0, 1024);
  provider_->process_midi_events (time_info1, output_buffer);
  auto first_loop_events = find_event_timestamps (output_buffer, 60);
  loop_events.push_back (first_loop_events);

  // Process second buffer that should include the loop
  dsp::graph::ProcessBlockInfo time_info2 = create_time_info (1024, 0, 2048);
  output_buffer.clear ();
  provider_->process_midi_events (time_info2, output_buffer);
  auto second_loop_events = find_event_timestamps (output_buffer, 60);
  loop_events.push_back (second_loop_events);

  // Process third buffer that should include another loop
  dsp::graph::ProcessBlockInfo time_info3 = create_time_info (3072, 0, 2048);
  output_buffer.clear ();
  provider_->process_midi_events (time_info3, output_buffer);
  auto third_loop_events = find_event_timestamps (output_buffer, 60);
  loop_events.push_back (third_loop_events);

  // Verify that we have events in each buffer
  EXPECT_GT (first_loop_events.size (), 0);
  EXPECT_GT (second_loop_events.size (), 0);
  EXPECT_GT (third_loop_events.size (), 0);

  // Verify that the events in subsequent loops are at the correct positions
  // The second loop should have events at positions relative to the loop point
  // (accounting for buffer offset)
  if (!second_loop_events.empty ())
    {
      // The first event in the second loop should be approximately at the loop
      // point (accounting for buffer offset)
      EXPECT_LT (second_loop_events[0], time_info2.nframes_);
    }

  if (!third_loop_events.empty ())
    {
      // The first event in the third loop should also be at the correct position
      EXPECT_LT (third_loop_events[0], time_info3.nframes_);
    }

  // Check for timestamp overflow
  EXPECT_FALSE (has_timestamp_overflow (output_buffer));
}

TEST_F (ClipPlaybackDataProviderTest, PreciseEventTiming)
{
  // Create a MIDI clip with notes at specific sample positions
  std::vector<std::tuple<int, int, int>> notes = {
    { 60, 64,  0    }, // Note at position 0
    { 62, 80,  1150 }, // Note at position ~1150 samples
    { 64, 100, 2295 }  // Note at position ~2295 samples
  };
  auto clip = create_midi_clip_with_sampled_notes (0.0, 400.0, notes);

  // Generate events for the clip
  provider_->generate_midi_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);

  // Process events
  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
  dsp::graph::ProcessBlockInfo time_info = create_time_info (0, 0, 4096);

  provider_->process_midi_events (time_info, output_buffer);

  // Find events for each pitch
  auto pitch_60_events = find_event_timestamps (output_buffer, 60);
  auto pitch_62_events = find_event_timestamps (output_buffer, 62);
  auto pitch_64_events = find_event_timestamps (output_buffer, 64);

  // Verify that we have events for each pitch
  EXPECT_GT (pitch_60_events.size (), 0);
  EXPECT_GT (pitch_62_events.size (), 0);
  EXPECT_GT (pitch_64_events.size (), 0);

  // Verify that the events are at the correct positions
  // Note at position 0 should be at timestamp 0
  EXPECT_EQ (pitch_60_events[0], units::samples (0));

  // Note at position 1150 samples should be at the correct position
  EXPECT_EQ (pitch_62_events[0], units::samples (1150));

  // Note at position 2295 samples should be at the correct position
  EXPECT_EQ (pitch_64_events[0], units::samples (2295));

  // Verify that all timestamps are within the buffer range
  for (const auto &event : output_buffer)
    {
      EXPECT_GE (event.time (), units::samples (0));
      EXPECT_LT (event.time (), time_info.nframes_);
    }

  // Check for timestamp overflow
  EXPECT_FALSE (has_timestamp_overflow (output_buffer));
}

TEST_F (ClipPlaybackDataProviderTest, BarBoundaryQuantization)
{
  // Create a MIDI clip
  auto clip = create_simple_region ();

  // Generate events with NextBar quantization
  provider_->generate_midi_events (
    *clip, structure::tracks::ClipQuantizeOption::NextBar);

  // Test processing events that should NOT include the note initially
  // because we're quantizing to the next bar
  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
  dsp::graph::ProcessBlockInfo time_info = create_time_info (64, 0, 256);

  provider_->process_midi_events (time_info, output_buffer);

  // Should have no events initially because we're waiting for the next bar
  EXPECT_EQ (output_buffer.size (), 0);

  // Process events at the next bar boundary (1 bar = 4 beats = 3840 ticks)
  const auto bar_start_samples = tempo_map_->tick_to_samples_rounded (
    dsp::TimelineTick{ units::ticks (3840.0) });
  dsp::graph::ProcessBlockInfo bar_time_info = create_time_info (
    static_cast<uint64_t> (bar_start_samples.in (units::sample)), 0, 256);

  provider_->process_midi_events (bar_time_info, output_buffer);

  // Should now have events because we're at the next bar
  EXPECT_GT (output_buffer.size (), 0);
}

TEST_F (ClipPlaybackDataProviderTest, BeatBoundaryQuantization)
{
  // Create a MIDI clip
  auto clip = create_simple_region ();

  // Generate events with NextBeat quantization
  provider_->generate_midi_events (
    *clip, structure::tracks::ClipQuantizeOption::NextBeat);

  // Test processing events that should NOT include the note initially
  // because we're quantizing to the next beat
  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
  dsp::graph::ProcessBlockInfo time_info = create_time_info (64, 0, 256);

  provider_->process_midi_events (time_info, output_buffer);

  // Should have no events initially because we're waiting for the next beat
  EXPECT_EQ (output_buffer.size (), 0);

  // Process events at the next beat boundary (1 beat = 960 ticks)
  const auto beat_start_samples = tempo_map_->tick_to_samples_rounded (
    dsp::TimelineTick{ units::ticks (960.0) });
  dsp::graph::ProcessBlockInfo beat_time_info = create_time_info (
    static_cast<uint64_t> (beat_start_samples.in (units::sample)), 0, 256);

  provider_->process_midi_events (beat_time_info, output_buffer);

  // Should now have events because we're at the next beat
  EXPECT_GT (output_buffer.size (), 0);
}

class ClipPlaybackDataProviderBufferSizeTest
    : public ClipPlaybackDataProviderTest,
      public ::testing::WithParamInterface<uint32_t>
{
};

TEST_P (
  ClipPlaybackDataProviderBufferSizeTest,
  ProcessEventsWithDifferentBufferSizes)
{
  const auto buffer_size = GetParam ();

  // Create a MIDI clip with notes spanning multiple buffer sizes
  std::vector<std::pair<int, int>> notes = {
    { 60, 64  }, // Note at position 0
    { 62, 80  }, // Note at position 0
    { 64, 100 }  // Note at position 0
  };
  auto clip = create_multi_note_region (notes);

  // Generate events for the clip
  provider_->generate_midi_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);

  // Test processing events with the specific buffer size
  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
  dsp::graph::ProcessBlockInfo time_info = create_time_info (0, 0, buffer_size);

  provider_->process_midi_events (time_info, output_buffer);

  // Should have events regardless of buffer size
  EXPECT_GT (output_buffer.size (), 0);

  // Verify that event timestamps are within the buffer range
  for (const auto &event : output_buffer)
    {
      EXPECT_GE (event.time (), units::samples (0));
      EXPECT_LT (event.time (), units::samples (buffer_size));
    }

  // Check that we have the expected pitches
  std::vector<int> found_pitches;
  for (const auto &event : output_buffer)
    {
      if (utils::midi::midi_is_note_on (event.data ())) // Note on
        {
          found_pitches.push_back (
            utils::midi::midi_get_note_number (event.data ()));
        }
    }

  EXPECT_TRUE (std::ranges::contains (found_pitches, 60));
  EXPECT_TRUE (std::ranges::contains (found_pitches, 62));
  EXPECT_TRUE (std::ranges::contains (found_pitches, 64));
}

INSTANTIATE_TEST_SUITE_P (
  ClipPlaybackDataProviderTest,
  ClipPlaybackDataProviderBufferSizeTest,
  ::testing::Values (64, 128, 256, 512, 1024, 2048));

TEST_F (ClipPlaybackDataProviderTest, ProcessEventsWithOffset)
{
  // Create a MIDI clip with a note at position 0
  auto clip = create_simple_region ();

  // Generate events for the clip
  provider_->generate_midi_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);

  // Test processing events with a global offset
  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
  dsp::graph::ProcessBlockInfo time_info = create_time_info (10000, 100, 256);

  provider_->process_midi_events (time_info, output_buffer);

  // Should have events since we're using immediate quantization
  EXPECT_GT (output_buffer.size (), 0);

  // Find the note on event
  auto pitch_60_events = find_event_timestamps (output_buffer, 60);
  EXPECT_GT (pitch_60_events.size (), 0);

  // The event should be offset by buffer_offset_, which is 100 samples
  EXPECT_EQ (pitch_60_events[0], units::samples (100));

  // Check for timestamp overflow
  EXPECT_FALSE (has_timestamp_overflow (output_buffer));
}

TEST_F (ClipPlaybackDataProviderTest, QuantizationTimingAccuracy)
{
  // Create a MIDI clip
  auto clip = create_simple_region ();

  // Generate events with NextBeat quantization
  provider_->generate_midi_events (
    *clip, structure::tracks::ClipQuantizeOption::NextBeat);

  // Start processing from a position that's not on a beat boundary
  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
  dsp::graph::ProcessBlockInfo time_info = create_time_info (64, 0, 256);

  provider_->process_midi_events (time_info, output_buffer);

  // Should have no events initially because we're quantizing to the next beat
  EXPECT_EQ (output_buffer.size (), 0);

  // Process events at the next beat boundary (1 beat = 960 ticks)
  const auto beat_start_samples = tempo_map_->tick_to_samples_rounded (
    dsp::TimelineTick{ units::ticks (960.0) });
  dsp::graph::ProcessBlockInfo beat_time_info = create_time_info (
    static_cast<uint64_t> (beat_start_samples.in (units::sample)), 0, 256);

  output_buffer.clear ();
  provider_->process_midi_events (beat_time_info, output_buffer);

  // Should now have events because we're at the next beat
  EXPECT_GT (output_buffer.size (), 0);

  // Find the note on event
  auto pitch_60_events = find_event_timestamps (output_buffer, 60);
  EXPECT_GT (pitch_60_events.size (), 0);

  // The event should be at timestamp 0 since we're starting exactly at the beat
  // boundary
  EXPECT_EQ (pitch_60_events[0], units::samples (0));

  // Check for timestamp overflow
  EXPECT_FALSE (has_timestamp_overflow (output_buffer));
}

TEST_F (ClipPlaybackDataProviderTest, QuantizedStartWithinBlockIsSampleAccurate)
{
  auto clip = create_simple_region ();
  provider_->generate_midi_events (
    *clip, structure::tracks::ClipQuantizeOption::NextBeat);

  // Block that contains the beat boundary 100 samples after its start
  const auto beat_start_samples = tempo_map_->tick_to_samples_rounded (
    dsp::TimelineTick{ units::ticks (960.0) });
  const auto block_start =
    static_cast<uint64_t> (beat_start_samples.in (units::sample)) - 100;

  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
  provider_->process_midi_events (
    create_time_info (block_start, 0, 256), output_buffer);

  const auto pitch_60_events = find_event_timestamps (output_buffer, 60);
  ASSERT_EQ (pitch_60_events.size (), 1);
  EXPECT_EQ (pitch_60_events[0], units::samples (100));
  EXPECT_TRUE (provider_->playing ());
}

TEST_F (ClipPlaybackDataProviderTest, QuantizedStartWithinBlockAddsBufferOffset)
{
  auto clip = create_simple_region ();
  provider_->generate_midi_events (
    *clip, structure::tracks::ClipQuantizeOption::NextBar);

  // Sub-block starting 32 samples into the output buffer, with the bar
  // boundary 50 samples after the sub-block start
  const auto bar_start_samples = tempo_map_->tick_to_samples_rounded (
    dsp::TimelineTick{ units::ticks (3840.0) });
  const auto block_start =
    static_cast<uint64_t> (bar_start_samples.in (units::sample)) - 50;

  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
  provider_->process_midi_events (
    create_time_info (block_start, 32, 128), output_buffer);

  const auto pitch_60_events = find_event_timestamps (output_buffer, 60);
  ASSERT_EQ (pitch_60_events.size (), 1);
  EXPECT_EQ (pitch_60_events[0], units::samples (82));
}

TEST_F (ClipPlaybackDataProviderTest, QuantizedStartAtBlockEndStartsInNextBlock)
{
  auto clip = create_simple_region ();
  provider_->generate_midi_events (
    *clip, structure::tracks::ClipQuantizeOption::NextBeat);

  // Block that ends exactly on the beat boundary
  const auto beat_start_samples = tempo_map_->tick_to_samples_rounded (
    dsp::TimelineTick{ units::ticks (960.0) });
  const auto beat_start =
    static_cast<uint64_t> (beat_start_samples.in (units::sample));

  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
  provider_->process_midi_events (
    create_time_info (beat_start - 256, 0, 256), output_buffer);
  EXPECT_TRUE (find_event_timestamps (output_buffer, 60).empty ());
  EXPECT_FALSE (provider_->playing ());

  output_buffer.clear ();
  provider_->process_midi_events (
    create_time_info (beat_start, 0, 256), output_buffer);
  const auto pitch_60_events = find_event_timestamps (output_buffer, 60);
  ASSERT_EQ (pitch_60_events.size (), 1);
  EXPECT_EQ (pitch_60_events[0], units::samples (0));
}

TEST_F (
  ClipPlaybackDataProviderTest,
  AudioQuantizedStartWithinBlockIsSampleAccurate)
{
  auto clip = create_simple_audio_clip ();
  provider_->generate_audio_events (
    *clip, structure::tracks::ClipQuantizeOption::NextBar);

  // Block that contains the bar boundary 100 samples after its start
  const auto bar_start_samples = tempo_map_->tick_to_samples_rounded (
    dsp::TimelineTick{ units::ticks (3840.0) });
  const auto block_start =
    static_cast<uint64_t> (bar_start_samples.in (units::sample)) - 100;

  std::vector<float> output_left (256, 0.0f);
  std::vector<float> output_right (256, 0.0f);
  provider_->process_audio_events (
    create_time_info (block_start, 0, 256), output_left, output_right);

  // Reference: the same clip launched immediately at a block start
  ClipPlaybackDataProvider reference_provider (*tempo_map_);
  reference_provider.generate_audio_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);
  std::vector<float> reference_left (256, 0.0f);
  std::vector<float> reference_right (256, 0.0f);
  reference_provider.process_audio_events (
    create_time_info (0, 0, 256), reference_left, reference_right);

  // Silence before the bar boundary, then the clip from its first sample
  for (const auto i : std::views::iota (0uz, 100uz))
    {
      EXPECT_FLOAT_EQ (output_left[i], 0.0f) << "at sample " << i;
      EXPECT_FLOAT_EQ (output_right[i], 0.0f) << "at sample " << i;
    }
  for (const auto i : std::views::iota (100uz, output_left.size ()))
    {
      EXPECT_FLOAT_EQ (output_left[i], reference_left[i - 100])
        << "at sample " << i;
      EXPECT_FLOAT_EQ (output_right[i], reference_right[i - 100])
        << "at sample " << i;
    }
}

namespace
{
std::vector<units::sample_u32_t>
all_notes_off_timestamps (const dsp::MidiEventBuffer &events)
{
  std::vector<units::sample_u32_t> timestamps;
  for (const auto &event : events)
    {
      if (utils::midi::midi_is_all_notes_off (event.data ()))
        timestamps.push_back (event.time ());
    }
  return timestamps;
}
}

TEST_F (
  ClipPlaybackDataProviderTest,
  QueuedClipReplacesPlayingClipAtQuantizationPoint)
{
  // Clip A plays immediately
  provider_->generate_midi_events (
    *create_simple_region (60),
    structure::tracks::ClipQuantizeOption::Immediate);
  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
  provider_->process_midi_events (create_time_info (0, 0, 256), output_buffer);
  ASSERT_EQ (find_event_timestamps (output_buffer, 60).size (), 1);

  // Clip B is queued for the next beat; A keeps playing until then
  provider_->generate_midi_events (
    *create_simple_region (62), structure::tracks::ClipQuantizeOption::NextBeat);
  output_buffer.clear ();
  provider_->process_midi_events (create_time_info (256, 0, 256), output_buffer);
  EXPECT_TRUE (find_event_timestamps (output_buffer, 62).empty ());
  EXPECT_TRUE (all_notes_off_timestamps (output_buffer).empty ());
  EXPECT_TRUE (provider_->playing ());
  EXPECT_TRUE (provider_->launch_pending ());

  // Block containing the beat boundary 100 samples after its start
  const auto beat_start = static_cast<uint64_t> (
    tempo_map_
      ->tick_to_samples_rounded (dsp::TimelineTick{ units::ticks (960.0) })
      .in (units::sample));
  output_buffer.clear ();
  provider_->process_midi_events (
    create_time_info (beat_start - 100, 0, 256), output_buffer);

  // A's notes are silenced and B starts, both on the beat
  const auto notes_off = all_notes_off_timestamps (output_buffer);
  ASSERT_EQ (notes_off.size (), 16);
  EXPECT_TRUE (std::ranges::all_of (notes_off, [] (const auto ts) {
    return ts == units::samples (100);
  }));
  const auto pitch_62_events = find_event_timestamps (output_buffer, 62);
  ASSERT_EQ (pitch_62_events.size (), 1);
  EXPECT_EQ (pitch_62_events[0], units::samples (100));
  EXPECT_TRUE (find_event_timestamps (output_buffer, 60).empty ());
  EXPECT_TRUE (provider_->playing ());
  EXPECT_FALSE (provider_->launch_pending ());
  // B has played 156 samples of its 400-tick contents
  const auto played_ticks =
    (tempo_map_->samples_to_tick (units::samples (beat_start + 156))
     - tempo_map_->samples_to_tick (units::samples (beat_start)))
      .asQuantity ()
      .in (units::ticks);
  EXPECT_NEAR (
    provider_->current_playback_fraction (), played_ticks / 400.0, 1e-9);
}

TEST_F (ClipPlaybackDataProviderTest, NewerQueuedClipReplacesEarlierQueuedClip)
{
  provider_->generate_midi_events (
    *create_simple_region (60),
    structure::tracks::ClipQuantizeOption::Immediate);
  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
  provider_->process_midi_events (create_time_info (0, 0, 256), output_buffer);

  provider_->generate_midi_events (
    *create_simple_region (62), structure::tracks::ClipQuantizeOption::NextBar);
  provider_->generate_midi_events (
    *create_simple_region (64), structure::tracks::ClipQuantizeOption::NextBar);

  const auto bar_start = static_cast<uint64_t> (
    tempo_map_
      ->tick_to_samples_rounded (dsp::TimelineTick{ units::ticks (3840.0) })
      .in (units::sample));
  output_buffer.clear ();
  provider_->process_midi_events (
    create_time_info (bar_start - 10, 0, 256), output_buffer);

  EXPECT_TRUE (find_event_timestamps (output_buffer, 62).empty ());
  const auto pitch_64_events = find_event_timestamps (output_buffer, 64);
  ASSERT_EQ (pitch_64_events.size (), 1);
  EXPECT_EQ (pitch_64_events[0], units::samples (10));
}

TEST_F (ClipPlaybackDataProviderTest, CancelPendingLaunchKeepsPlayingClip)
{
  provider_->generate_midi_events (
    *create_simple_region (60),
    structure::tracks::ClipQuantizeOption::Immediate);
  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
  provider_->process_midi_events (create_time_info (0, 0, 256), output_buffer);

  provider_->generate_midi_events (
    *create_simple_region (62), structure::tracks::ClipQuantizeOption::NextBeat);
  provider_->cancel_pending_launch ();
  EXPECT_FALSE (provider_->launch_pending ());

  const auto beat_start = static_cast<uint64_t> (
    tempo_map_
      ->tick_to_samples_rounded (dsp::TimelineTick{ units::ticks (960.0) })
      .in (units::sample));
  output_buffer.clear ();
  provider_->process_midi_events (
    create_time_info (beat_start - 100, 0, 256), output_buffer);

  EXPECT_TRUE (find_event_timestamps (output_buffer, 62).empty ());
  EXPECT_TRUE (all_notes_off_timestamps (output_buffer).empty ());
  EXPECT_TRUE (provider_->playing ());
}

TEST_F (ClipPlaybackDataProviderTest, QuantizedStopWaitsForQuantizationPoint)
{
  provider_->generate_midi_events (
    *create_simple_region (60),
    structure::tracks::ClipQuantizeOption::Immediate);
  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
  provider_->process_midi_events (create_time_info (0, 0, 256), output_buffer);

  provider_->queue_stop_playback (
    structure::tracks::ClipQuantizeOption::NextBeat);
  EXPECT_TRUE (provider_->launch_pending ());

  // The clip keeps playing until the beat boundary
  output_buffer.clear ();
  provider_->process_midi_events (create_time_info (256, 0, 256), output_buffer);
  EXPECT_TRUE (all_notes_off_timestamps (output_buffer).empty ());
  EXPECT_TRUE (provider_->playing ());

  // Block containing the beat boundary 100 samples after its start
  const auto beat_start = static_cast<uint64_t> (
    tempo_map_
      ->tick_to_samples_rounded (dsp::TimelineTick{ units::ticks (960.0) })
      .in (units::sample));
  output_buffer.clear ();
  provider_->process_midi_events (
    create_time_info (beat_start - 100, 0, 256), output_buffer);

  const auto notes_off = all_notes_off_timestamps (output_buffer);
  ASSERT_EQ (notes_off.size (), 16);
  EXPECT_TRUE (std::ranges::all_of (notes_off, [] (const auto ts) {
    return ts == units::samples (100);
  }));
  EXPECT_FALSE (provider_->playing ());
  EXPECT_FALSE (provider_->launch_pending ());

  // Nothing plays afterwards
  output_buffer.clear ();
  provider_->process_midi_events (
    create_time_info (beat_start + 156, 0, 256), output_buffer);
  EXPECT_EQ (output_buffer.size (), 0);
}

TEST_F (ClipPlaybackDataProviderTest, LaunchReplacesQueuedStop)
{
  provider_->generate_midi_events (
    *create_simple_region (60),
    structure::tracks::ClipQuantizeOption::Immediate);
  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
  provider_->process_midi_events (create_time_info (0, 0, 256), output_buffer);

  provider_->queue_stop_playback (
    structure::tracks::ClipQuantizeOption::NextBar);
  provider_->generate_midi_events (
    *create_simple_region (62), structure::tracks::ClipQuantizeOption::NextBar);

  const auto bar_start = static_cast<uint64_t> (
    tempo_map_
      ->tick_to_samples_rounded (dsp::TimelineTick{ units::ticks (3840.0) })
      .in (units::sample));
  output_buffer.clear ();
  provider_->process_midi_events (
    create_time_info (bar_start - 10, 0, 256), output_buffer);

  const auto pitch_62_events = find_event_timestamps (output_buffer, 62);
  ASSERT_EQ (pitch_62_events.size (), 1);
  EXPECT_EQ (pitch_62_events[0], units::samples (10));
  EXPECT_TRUE (provider_->playing ());
}

TEST_F (
  ClipPlaybackDataProviderTest,
  AudioQuantizedStopSilencesFromQuantizationPoint)
{
  // Launch in the block before the beat so the clip's audio is still
  // playing when the beat comes
  const auto beat_start = static_cast<uint64_t> (
    tempo_map_
      ->tick_to_samples_rounded (dsp::TimelineTick{ units::ticks (960.0) })
      .in (units::sample));
  provider_->generate_audio_events (
    *create_simple_audio_clip (),
    structure::tracks::ClipQuantizeOption::Immediate);
  std::vector<float> output_left (256, 0.0f);
  std::vector<float> output_right (256, 0.0f);
  provider_->process_audio_events (
    create_time_info (beat_start - 300, 0, 256), output_left, output_right);

  provider_->queue_stop_playback (
    structure::tracks::ClipQuantizeOption::NextBeat);

  // Next block: the beat boundary is 44 samples after its start
  std::ranges::fill (output_left, 0.0f);
  std::ranges::fill (output_right, 0.0f);
  provider_->process_audio_events (
    create_time_info (beat_start - 44, 0, 256), output_left, output_right);

  for (const auto i : std::views::iota (0uz, 44uz))
    {
      EXPECT_FLOAT_EQ (output_left[i], 0.5f) << "at sample " << i;
      EXPECT_FLOAT_EQ (output_right[i], -0.5f) << "at sample " << i;
    }
  for (const auto i : std::views::iota (44uz, output_left.size ()))
    {
      EXPECT_FLOAT_EQ (output_left[i], 0.0f) << "at sample " << i;
      EXPECT_FLOAT_EQ (output_right[i], 0.0f) << "at sample " << i;
    }
  EXPECT_FALSE (provider_->playing ());
}

TEST_F (ClipPlaybackDataProviderTest, StopDiscardsQueuedClip)
{
  provider_->generate_midi_events (
    *create_simple_region (60),
    structure::tracks::ClipQuantizeOption::Immediate);
  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
  provider_->process_midi_events (create_time_info (0, 0, 256), output_buffer);

  provider_->generate_midi_events (
    *create_simple_region (62), structure::tracks::ClipQuantizeOption::NextBeat);
  provider_->queue_stop_playback (
    structure::tracks::ClipQuantizeOption::Immediate);
  EXPECT_FALSE (provider_->launch_pending ());

  // The playing clip stops on the next block
  output_buffer.clear ();
  provider_->process_midi_events (create_time_info (256, 0, 256), output_buffer);
  EXPECT_EQ (all_notes_off_timestamps (output_buffer).size (), 16);
  EXPECT_FALSE (provider_->playing ());

  // The queued clip never starts
  const auto beat_start = static_cast<uint64_t> (
    tempo_map_
      ->tick_to_samples_rounded (dsp::TimelineTick{ units::ticks (960.0) })
      .in (units::sample));
  output_buffer.clear ();
  provider_->process_midi_events (
    create_time_info (beat_start - 100, 0, 256), output_buffer);
  EXPECT_TRUE (find_event_timestamps (output_buffer, 62).empty ());
  EXPECT_FALSE (provider_->playing ());
}

TEST_F (
  ClipPlaybackDataProviderTest,
  AudioQueuedClipReplacesPlayingClipAtQuantizationPoint)
{
  auto * clip_a = create_simple_audio_clip (1.0f);
  auto * clip_b = create_simple_audio_clip (2.0f);

  provider_->generate_audio_events (
    *clip_a, structure::tracks::ClipQuantizeOption::Immediate);
  std::vector<float> output_left (256, 0.0f);
  std::vector<float> output_right (256, 0.0f);
  provider_->process_audio_events (
    create_time_info (0, 0, 256), output_left, output_right);

  provider_->generate_audio_events (
    *clip_b, structure::tracks::ClipQuantizeOption::NextBar);

  // Reference: clip B launched immediately at a block start
  ClipPlaybackDataProvider reference_provider (*tempo_map_);
  reference_provider.generate_audio_events (
    *clip_b, structure::tracks::ClipQuantizeOption::Immediate);
  std::vector<float> reference_left (256, 0.0f);
  std::vector<float> reference_right (256, 0.0f);
  reference_provider.process_audio_events (
    create_time_info (0, 0, 256), reference_left, reference_right);

  // Block containing the bar boundary 100 samples after its start
  const auto bar_start = static_cast<uint64_t> (
    tempo_map_
      ->tick_to_samples_rounded (dsp::TimelineTick{ units::ticks (3840.0) })
      .in (units::sample));
  std::ranges::fill (output_left, 0.0f);
  std::ranges::fill (output_right, 0.0f);
  provider_->process_audio_events (
    create_time_info (bar_start - 100, 0, 256), output_left, output_right);

  // From the bar boundary on, the output is clip B from its first sample
  for (const auto i : std::views::iota (100uz, output_left.size ()))
    {
      EXPECT_FLOAT_EQ (output_left[i], reference_left[i - 100])
        << "at sample " << i;
      EXPECT_FLOAT_EQ (output_right[i], reference_right[i - 100])
        << "at sample " << i;
    }
  EXPECT_FALSE (provider_->launch_pending ());
}

TEST_F (ClipPlaybackDataProviderTest, StateManagementAcrossBuffers)
{
  // Create a MIDI clip with notes at different positions
  std::vector<std::tuple<int, int, int>> notes = {
    { 60, 64, 0    }, // Note at position 0
    { 62, 80, 1150 }  // Note at position ~1150 samples
  };
  auto clip = create_midi_clip_with_sampled_notes (0.0, 400.0, notes);

  // Generate events for the clip
  provider_->generate_midi_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);

  // Process first buffer
  auto output_buffer1 = dsp::MidiEventBuffer::make_reserved ();
  dsp::graph::ProcessBlockInfo time_info1 = create_time_info (0, 0, 128);

  provider_->process_midi_events (time_info1, output_buffer1);

  // Should have the first note but not the second
  auto pitch_60_events1 = find_event_timestamps (output_buffer1, 60);
  auto pitch_62_events1 = find_event_timestamps (output_buffer1, 62);
  EXPECT_GT (pitch_60_events1.size (), 0);
  EXPECT_EQ (pitch_62_events1.size (), 0);

  // Process second buffer continuing from where we left off
  auto output_buffer2 = dsp::MidiEventBuffer::make_reserved ();
  dsp::graph::ProcessBlockInfo time_info2 = create_time_info (128, 0, 2048);

  provider_->process_midi_events (time_info2, output_buffer2);

  // Should have the second note
  auto pitch_62_events2 = find_event_timestamps (output_buffer2, 62);
  EXPECT_GT (pitch_62_events2.size (), 0);

  // The second note should be at the correct position relative to the buffer
  EXPECT_EQ (pitch_62_events2[0], units::samples (1150 - 128));

  // Check for timestamp overflow
  EXPECT_FALSE (has_timestamp_overflow (output_buffer2));
}

TEST_F (ClipPlaybackDataProviderTest, BasicFunctionality)
{
  // Test that the provider can be constructed
  EXPECT_NE (provider_, nullptr);

  // Test that process_midi_events can be called without crashing
  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
  dsp::graph::ProcessBlockInfo time_info = create_time_info (0, 0, 256);

  provider_->process_midi_events (time_info, output_buffer);
  EXPECT_EQ (output_buffer.size (), 0);

  // Test that generate_midi_events can be called without crashing
  auto clip = create_simple_region ();
  provider_->generate_midi_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);

  // Process again to ensure no crash
  provider_->process_midi_events (time_info, output_buffer);
  EXPECT_GT (output_buffer.size (), 0);
}

TEST_F (ClipPlaybackDataProviderTest, GenerateEventsWithMutedNote)
{
  // Create a MIDI clip
  auto clip = create_simple_region ();

  // Mute the note in our clip
  auto note_view = clip->ArrangerObjectOwner<
    structure::arrangement::MidiNote>::get_children_view ();
  if (!note_view.empty ())
    {
      note_view[0]->mute ()->setMuted (true);
    }

  // Generate events for the clip
  provider_->generate_midi_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);

  // Test processing events
  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
  dsp::graph::ProcessBlockInfo time_info = create_time_info (0, 0, 256);

  provider_->process_midi_events (time_info, output_buffer);

  // Should have no events since the only note is muted
  EXPECT_EQ (output_buffer.size (), 0);
}

TEST_F (ClipPlaybackDataProviderTest, LoopingEdgeCases)
{
  // Create a very short MIDI clip (23 samples) to test edge cases
  std::vector<std::tuple<int, int, int>> notes = {
    { 60, 64, 0 }  // Note at position 0
  };
  auto clip = create_midi_clip_with_sampled_notes (0.0, 1.0, notes);

  // Generate events for the clip
  provider_->generate_midi_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);

  // Process multiple buffers to verify looping
  std::vector<std::vector<units::sample_u32_t>> loop_events;
  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();

  // Process several buffers
  for (int i = 0; i < 5; ++i)
    {
      dsp::graph::ProcessBlockInfo time_info =
        create_time_info (i * 256, 0, 256);

      output_buffer.clear ();
      provider_->process_midi_events (time_info, output_buffer);
      auto events = find_event_timestamps (output_buffer, 60);
      loop_events.push_back (events);
    }

  // Should have events in each buffer due to looping
  for (const auto &events : loop_events)
    {
      EXPECT_GT (events.size (), 0);
    }

  // Verify that events are at the correct positions in each buffer
  // The first buffer should have the event at position 0
  EXPECT_EQ (loop_events[0][0], units::samples (0));

  // Subsequent buffers should have events at positions that depend on where
  // the loop point falls within the buffer
  for (size_t i = 1; i < loop_events.size (); ++i)
    {
      // Events should be within the buffer range
      EXPECT_LT (loop_events[i][0], units::samples (256));
    }

  // Check for timestamp overflow
  EXPECT_FALSE (has_timestamp_overflow (output_buffer));
}

TEST_F (ClipPlaybackDataProviderTest, EventsOnBufferBoundaries)
{
  // Create a MIDI clip with a note exactly at a buffer boundary
  std::vector<std::tuple<int, int, int>> notes = {
    { 60, 64, 128 }  // Note exactly at buffer boundary
  };
  auto clip = create_midi_clip_with_sampled_notes (0.0, 400.0, notes);

  // Generate events for the clip
  provider_->generate_midi_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);

  // Process first buffer
  auto output_buffer1 = dsp::MidiEventBuffer::make_reserved ();
  dsp::graph::ProcessBlockInfo time_info1 = create_time_info (0, 0, 128);

  provider_->process_midi_events (time_info1, output_buffer1);

  // Should have no events in the first buffer since the note is at the boundary
  auto pitch_60_events1 = find_event_timestamps (output_buffer1, 60);
  EXPECT_EQ (pitch_60_events1.size (), 0);

  // Process second buffer starting at the boundary
  auto output_buffer2 = dsp::MidiEventBuffer::make_reserved ();
  dsp::graph::ProcessBlockInfo time_info2 = create_time_info (128, 0, 128);

  provider_->process_midi_events (time_info2, output_buffer2);

  // Should have the event in the second buffer at position 0
  auto pitch_60_events2 = find_event_timestamps (output_buffer2, 60);
  EXPECT_GT (pitch_60_events2.size (), 0);
  EXPECT_EQ (pitch_60_events2[0], units::samples (0));

  // Check for timestamp overflow
  EXPECT_FALSE (has_timestamp_overflow (output_buffer2));
}

TEST_F (ClipPlaybackDataProviderTest, BackwardPlayheadMovement)
{
  // Create a MIDI clip with notes at different positions
  std::vector<std::tuple<int, int, int>> notes = {
    { 60, 64, 0    }, // Note at position 0
    { 62, 80, 1150 }  // Note at position ~1150 samples
  };
  auto clip = create_midi_clip_with_sampled_notes (0.0, 400.0, notes);

  // Generate events for the clip
  provider_->generate_midi_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);

  // Process first buffer starting at frame 0
  auto output_buffer1 = dsp::MidiEventBuffer::make_reserved ();
  dsp::graph::ProcessBlockInfo time_info1 = create_time_info (0, 0, 256);

  provider_->process_midi_events (time_info1, output_buffer1);

  // Should have the first note
  auto pitch_60_events1 = find_event_timestamps (output_buffer1, 60);
  EXPECT_GT (pitch_60_events1.size (), 0);

  // Process second buffer continuing forward
  auto output_buffer2 = dsp::MidiEventBuffer::make_reserved ();
  dsp::graph::ProcessBlockInfo time_info2 = create_time_info (256, 0, 256);

  provider_->process_midi_events (time_info2, output_buffer2);

  // Should NOT have the second note yet because it's at position 1150
  // and this buffer only covers positions 256-511
  auto pitch_62_events2 = find_event_timestamps (output_buffer2, 62);
  EXPECT_EQ (pitch_62_events2.size (), 0);

  // Now simulate transport looping back to frame 0
  // This is the critical test - when playhead moves backwards
  auto output_buffer3 = dsp::MidiEventBuffer::make_reserved ();
  dsp::graph::ProcessBlockInfo time_info3 = create_time_info (0, 0, 256);

  provider_->process_midi_events (time_info3, output_buffer3);

  // Should have the first note again since we looped back
  auto pitch_60_events3 = find_event_timestamps (output_buffer3, 60);
  EXPECT_GT (pitch_60_events3.size (), 0);

  // The event should be at timestamp 0 since we're starting at frame 0
  EXPECT_EQ (pitch_60_events3[0], units::samples (0));

  // Check for timestamp overflow
  EXPECT_FALSE (has_timestamp_overflow (output_buffer3));
}

TEST_F (ClipPlaybackDataProviderTest, BackwardPlayheadMovementWithLoop)
{
  // Create a very short MIDI clip that will loop multiple times
  std::vector<std::tuple<int, int, int>> notes = {
    { 60, 64, 0 }  // Note at position 0
  };
  auto clip = create_midi_clip_with_sampled_notes (0.0, 1.0, notes);

  // Generate events for the clip
  provider_->generate_midi_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);

  // Process several buffers to establish a playback position
  for (int i = 0; i < 3; ++i)
    {
      auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
      dsp::graph::ProcessBlockInfo time_info =
        create_time_info (i * 256, 0, 256);
      provider_->process_midi_events (time_info, output_buffer);

      // Should have events in each buffer
      auto events = find_event_timestamps (output_buffer, 60);
      EXPECT_GT (events.size (), 0);
    }

  // Now simulate transport looping back to frame 0
  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
  dsp::graph::ProcessBlockInfo time_info = create_time_info (0, 0, 256);

  provider_->process_midi_events (time_info, output_buffer);

  // Should have events even after the backward jump
  auto events = find_event_timestamps (output_buffer, 60);
  EXPECT_GT (events.size (), 0);

  // The event should be at timestamp 0 since we're starting at frame 0
  EXPECT_EQ (events[0], units::samples (0));

  // Check for timestamp overflow
  EXPECT_FALSE (has_timestamp_overflow (output_buffer));
}

// ========== Audio Tests ==========

TEST_F (ClipPlaybackDataProviderTest, AudioInitialState)
{
  // Provider should start with no audio
  std::vector<float>           output_left (256, 0.0f);
  std::vector<float>           output_right (256, 0.0f);
  dsp::graph::ProcessBlockInfo time_info = create_time_info (0, 0, 256);

  provider_->process_audio_events (time_info, output_left, output_right);

  // Output should be all zeros
  for (size_t i = 0; i < output_left.size (); ++i)
    {
      EXPECT_FLOAT_EQ (output_left[i], 0.0f);
      EXPECT_FLOAT_EQ (output_right[i], 0.0f);
    }
}

TEST_F (ClipPlaybackDataProviderTest, GenerateAudioEventsWithAudioClip)
{
  // Create a simple audio clip
  auto clip = create_simple_audio_clip ();

  // Generate events for the clip
  provider_->generate_audio_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);

  // Test processing audio that should include the clip
  std::vector<float>           output_left (256, 0.0f);
  std::vector<float>           output_right (256, 0.0f);
  dsp::graph::ProcessBlockInfo time_info = create_time_info (0, 0, 256);

  provider_->process_audio_events (time_info, output_left, output_right);

  // Should have some audio output (not all zeros)
  bool has_audio = false;
  for (size_t i = 0; i < output_left.size (); ++i)
    {
      if (
        std::abs (output_left[i]) > 0.001f
        || std::abs (output_right[i]) > 0.001f)
        {
          has_audio = true;
          break;
        }
    }
  EXPECT_TRUE (has_audio);
}

TEST_F (ClipPlaybackDataProviderTest, AudioClearEvents)
{
  // Create a simple audio clip
  auto clip = create_simple_audio_clip ();

  // Generate events for the clip
  provider_->generate_audio_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);

  // Test processing audio - should have audio
  std::vector<float>           output_left (256, 0.0f);
  std::vector<float>           output_right (256, 0.0f);
  dsp::graph::ProcessBlockInfo time_info = create_time_info (0, 0, 256);

  provider_->process_audio_events (time_info, output_left, output_right);

  bool has_audio_before = false;
  for (size_t i = 0; i < output_left.size (); ++i)
    {
      if (
        std::abs (output_left[i]) > 0.001f
        || std::abs (output_right[i]) > 0.001f)
        {
          has_audio_before = true;
          break;
        }
    }
  EXPECT_TRUE (has_audio_before);

  // Stop playback
  provider_->queue_stop_playback (
    structure::tracks::ClipQuantizeOption::Immediate);

  // Test processing audio again - should have no audio
  std::ranges::fill (output_left, 0.0f);
  std::ranges::fill (output_right, 0.0f);
  provider_->process_audio_events (time_info, output_left, output_right);

  for (size_t i = 0; i < output_left.size (); ++i)
    {
      EXPECT_FLOAT_EQ (output_left[i], 0.0f);
      EXPECT_FLOAT_EQ (output_right[i], 0.0f);
    }
}

TEST_F (ClipPlaybackDataProviderTest, AudioQuantizeOptions)
{
  // Create a simple audio clip
  auto clip = create_simple_audio_clip ();

  // Test different quantize options
  std::vector<ClipQuantizeOption> quantize_options = {
    structure::tracks::ClipQuantizeOption::Immediate,
    structure::tracks::ClipQuantizeOption::NextBar,
    structure::tracks::ClipQuantizeOption::NextBeat,
    structure::tracks::ClipQuantizeOption::NextQuarterNote,
    structure::tracks::ClipQuantizeOption::NextEighthNote,
    structure::tracks::ClipQuantizeOption::NextSixteenthNote,
    structure::tracks::ClipQuantizeOption::NextThirtySecondNote
  };

  for (const auto &quantize_option : quantize_options)
    {
      // Generate events with the quantize option
      provider_->generate_audio_events (*clip, quantize_option);

      // Test processing audio
      std::vector<float>           output_left (256, 0.0f);
      std::vector<float>           output_right (256, 0.0f);
      dsp::graph::ProcessBlockInfo time_info = create_time_info (0, 0, 256);

      provider_->process_audio_events (time_info, output_left, output_right);

      // Should have audio for immediate quantization
      if (quantize_option == structure::tracks::ClipQuantizeOption::Immediate)
        {
          bool has_audio = false;
          for (size_t i = 0; i < output_left.size (); ++i)
            {
              if (
                std::abs (output_left[i]) > 0.001f
                || std::abs (output_right[i]) > 0.001f)
                {
                  has_audio = true;
                  break;
                }
            }
          EXPECT_TRUE (has_audio);
        }
    }
}

TEST_F (ClipPlaybackDataProviderTest, AudioPreciseLoopingBehavior)
{
  // Create a very short audio clip that will loop
  auto clip = create_short_audio_clip (23); // 23 samples

  // Generate events for the clip
  provider_->generate_audio_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);

  // Process multiple buffers to trigger looping
  std::vector<std::vector<float>> loop_audio_left;
  std::vector<std::vector<float>> loop_audio_right;
  std::vector<float>              output_left (2048, 0.0f);
  std::vector<float>              output_right (2048, 0.0f);

  // Process first buffer
  dsp::graph::ProcessBlockInfo time_info1 = create_time_info (0, 0, 1024);
  provider_->process_audio_events (time_info1, output_left, output_right);
  loop_audio_left.push_back (output_left);
  loop_audio_right.push_back (output_right);

  // Process second buffer that should include the loop
  output_left.assign (256, 0.0f);
  output_right.assign (256, 0.0f);
  dsp::graph::ProcessBlockInfo time_info2 = create_time_info (1024, 0, 2048);
  provider_->process_audio_events (time_info2, output_left, output_right);
  loop_audio_left.push_back (output_left);
  loop_audio_right.push_back (output_right);

  // Process third buffer that should include another loop
  output_left.assign (256, 0.0f);
  output_right.assign (256, 0.0f);
  dsp::graph::ProcessBlockInfo time_info3 = create_time_info (3072, 0, 2048);
  provider_->process_audio_events (time_info3, output_left, output_right);
  loop_audio_left.push_back (output_left);
  loop_audio_right.push_back (output_right);

  // Verify that we have audio in each buffer
  for (const auto &audio_buffer : loop_audio_left)
    {
      bool has_audio = false;
      for (size_t i = 0; i < audio_buffer.size (); ++i)
        {
          if (std::abs (audio_buffer[i]) > 0.001f)
            {
              has_audio = true;
              break;
            }
        }
      EXPECT_TRUE (has_audio);
    }
}

TEST_F (ClipPlaybackDataProviderTest, AudioPreciseEventTiming)
{
  // Create an audio clip with specific content
  auto clip = create_sine_wave_audio_clip (4410); // 0.1 seconds

  // Generate events for the clip
  provider_->generate_audio_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);

  // Process audio
  std::vector<float>           output_left (4096, 0.0f);
  std::vector<float>           output_right (4096, 0.0f);
  dsp::graph::ProcessBlockInfo time_info = create_time_info (0, 0, 4096);

  provider_->process_audio_events (time_info, output_left, output_right);

  // Should have audio output
  bool has_audio = false;
  for (size_t i = 0; i < output_left.size (); ++i)
    {
      if (
        std::abs (output_left[i]) > 0.001f
        || std::abs (output_right[i]) > 0.001f)
        {
          has_audio = true;
          break;
        }
    }
  EXPECT_TRUE (has_audio);

  // First sample will be zero due to built-in fade in (starts from 0.0)
  EXPECT_FLOAT_EQ (output_left[0], 0.0f);
  EXPECT_FLOAT_EQ (output_right[0], 0.0f);

  // But we should have non-zero audio after the built-in fade in
  EXPECT_GT (
    std::abs (output_left[arrangement::AudioClip::BUILTIN_FADE_FRAMES]), 0.001f);
  EXPECT_GT (
    std::abs (output_right[arrangement::AudioClip::BUILTIN_FADE_FRAMES]),
    0.001f);
}

TEST_F (ClipPlaybackDataProviderTest, AudioBarBoundaryQuantization)
{
  // Create a simple audio clip
  auto clip = create_simple_audio_clip ();

  // Generate events with NextBar quantization
  provider_->generate_audio_events (
    *clip, structure::tracks::ClipQuantizeOption::NextBar);

  // Test processing audio that should NOT include the audio initially
  // because we're quantizing to the next bar
  std::vector<float>           output_left (256, 0.0f);
  std::vector<float>           output_right (256, 0.0f);
  dsp::graph::ProcessBlockInfo time_info = create_time_info (64, 0, 256);

  provider_->process_audio_events (time_info, output_left, output_right);

  // Should have no audio initially because we're waiting for the next bar
  for (size_t i = 0; i < output_left.size (); ++i)
    {
      EXPECT_FLOAT_EQ (output_left[i], 0.0f);
      EXPECT_FLOAT_EQ (output_right[i], 0.0f);
    }

  // Process audio at the next bar boundary (1 bar = 4 beats = 3840 ticks)
  const auto bar_start_samples = tempo_map_->tick_to_samples_rounded (
    dsp::TimelineTick{ units::ticks (3840.0) });
  dsp::graph::ProcessBlockInfo bar_time_info = create_time_info (
    static_cast<uint64_t> (bar_start_samples.in (units::sample)), 0, 256);

  std::ranges::fill (output_left, 0.0f);
  std::ranges::fill (output_right, 0.0f);
  provider_->process_audio_events (bar_time_info, output_left, output_right);

  // Should now have audio because we're at the next bar
  bool has_audio = false;
  for (size_t i = 0; i < output_left.size (); ++i)
    {
      if (
        std::abs (output_left[i]) > 0.001f
        || std::abs (output_right[i]) > 0.001f)
        {
          has_audio = true;
          break;
        }
    }
  EXPECT_TRUE (has_audio);
}

TEST_F (ClipPlaybackDataProviderTest, AudioBeatBoundaryQuantization)
{
  // Create a simple audio clip
  auto clip = create_simple_audio_clip ();

  // Generate events with NextBeat quantization
  provider_->generate_audio_events (
    *clip, structure::tracks::ClipQuantizeOption::NextBeat);

  // Test processing audio that should NOT include the audio initially
  // because we're quantizing to the next beat
  std::vector<float>           output_left (256, 0.0f);
  std::vector<float>           output_right (256, 0.0f);
  dsp::graph::ProcessBlockInfo time_info = create_time_info (64, 0, 256);

  provider_->process_audio_events (time_info, output_left, output_right);

  // Should have no audio initially because we're waiting for the next beat
  for (size_t i = 0; i < output_left.size (); ++i)
    {
      EXPECT_FLOAT_EQ (output_left[i], 0.0f);
      EXPECT_FLOAT_EQ (output_right[i], 0.0f);
    }

  // Process audio at the next beat boundary (1 beat = 960 ticks)
  const auto beat_start_samples = tempo_map_->tick_to_samples_rounded (
    dsp::TimelineTick{ units::ticks (960.0) });
  dsp::graph::ProcessBlockInfo beat_time_info = create_time_info (
    static_cast<uint64_t> (beat_start_samples.in (units::sample)), 0, 256);

  std::ranges::fill (output_left, 0.0f);
  std::ranges::fill (output_right, 0.0f);
  provider_->process_audio_events (beat_time_info, output_left, output_right);

  // Should now have audio because we're at the next beat
  bool has_audio = false;
  for (size_t i = 0; i < output_left.size (); ++i)
    {
      if (
        std::abs (output_left[i]) > 0.001f
        || std::abs (output_right[i]) > 0.001f)
        {
          has_audio = true;
          break;
        }
    }
  EXPECT_TRUE (has_audio);
}

class ClipPlaybackDataProviderAudioBufferSizeTest
    : public ClipPlaybackDataProviderTest,
      public ::testing::WithParamInterface<uint32_t>
{
};

TEST_P (
  ClipPlaybackDataProviderAudioBufferSizeTest,
  ProcessAudioEventsWithDifferentBufferSizes)
{
  const auto buffer_size = GetParam ();

  // Create a simple audio clip
  auto clip = create_simple_audio_clip ();

  // Generate events for the clip
  provider_->generate_audio_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);

  // Test processing audio with the specific buffer size
  std::vector<float>           output_left (buffer_size, 0.0f);
  std::vector<float>           output_right (buffer_size, 0.0f);
  dsp::graph::ProcessBlockInfo time_info = create_time_info (0, 0, buffer_size);

  provider_->process_audio_events (time_info, output_left, output_right);

  // Should have audio regardless of buffer size
  bool has_audio = false;
  for (size_t i = 0; i < output_left.size (); ++i)
    {
      if (
        std::abs (output_left[i]) > 0.001f
        || std::abs (output_right[i]) > 0.001f)
        {
          has_audio = true;
          break;
        }
    }
  EXPECT_TRUE (has_audio);
}

INSTANTIATE_TEST_SUITE_P (
  ClipPlaybackDataProviderTest,
  ClipPlaybackDataProviderAudioBufferSizeTest,
  ::testing::Values (64, 128, 256, 512, 1024, 2048));

TEST_F (ClipPlaybackDataProviderTest, ProcessAudioEventsWithOffset)
{
  // Create a simple audio clip
  auto clip = create_simple_audio_clip ();

  // Generate events for the clip
  provider_->generate_audio_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);

  // Test processing audio with a global offset
  std::vector<float>           output_left (256, 0.0f);
  std::vector<float>           output_right (256, 0.0f);
  dsp::graph::ProcessBlockInfo time_info =
    create_time_info (10000, 100, 256 - 100);

  provider_->process_audio_events (time_info, output_left, output_right);

  // Should have audio since we're using immediate quantization
  bool has_audio = false;
  for (size_t i = 0; i < output_left.size (); ++i)
    {
      if (
        std::abs (output_left[i]) > 0.001f
        || std::abs (output_right[i]) > 0.001f)
        {
          has_audio = true;
          break;
        }
    }
  EXPECT_TRUE (has_audio);

  // The audio should be offset by buffer_offset_, which is 100 samples
  // So the first 100 samples should be zero
  for (size_t i = 0; i < 100; ++i)
    {
      EXPECT_FLOAT_EQ (output_left[i], 0.0f);
      EXPECT_FLOAT_EQ (output_right[i], 0.0f);
    }

  // But there should be audio after the offset
  for (size_t i = 100; i < output_left.size (); ++i)
    {
      if (
        std::abs (output_left[i]) > 0.001f
        || std::abs (output_right[i]) > 0.001f)
        {
          has_audio = true;
          break;
        }
    }
  EXPECT_TRUE (has_audio);
}

TEST_F (ClipPlaybackDataProviderTest, AudioStateManagementAcrossBuffers)
{
  // Create a short audio clip
  auto clip = create_short_audio_clip (128);

  // Generate events for the clip
  provider_->generate_audio_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);

  // Process first buffer
  std::vector<float>           output_left1 (128, 0.0f);
  std::vector<float>           output_right1 (128, 0.0f);
  dsp::graph::ProcessBlockInfo time_info1 = create_time_info (0, 0, 128);

  provider_->process_audio_events (time_info1, output_left1, output_right1);

  // Should have audio in the first buffer
  bool has_audio1 = false;
  for (size_t i = 0; i < output_left1.size (); ++i)
    {
      if (
        std::abs (output_left1[i]) > 0.001f
        || std::abs (output_right1[i]) > 0.001f)
        {
          has_audio1 = true;
          break;
        }
    }
  EXPECT_TRUE (has_audio1);

  // Process second buffer continuing from where we left off
  std::vector<float>           output_left2 (2048, 0.0f);
  std::vector<float>           output_right2 (2048, 0.0f);
  dsp::graph::ProcessBlockInfo time_info2 = create_time_info (128, 0, 2048);

  provider_->process_audio_events (time_info2, output_left2, output_right2);

  // Should have audio in the second buffer due to looping
  bool has_audio2 = false;
  for (size_t i = 0; i < output_left2.size (); ++i)
    {
      if (
        std::abs (output_left2[i]) > 0.001f
        || std::abs (output_right2[i]) > 0.001f)
        {
          has_audio2 = true;
          break;
        }
    }
  EXPECT_TRUE (has_audio2);
}

TEST_F (ClipPlaybackDataProviderTest, AudioBasicFunctionality)
{
  // Test that the provider can be constructed
  EXPECT_NE (provider_, nullptr);

  // Test that process_audio_events can be called without crashing
  std::vector<float>           output_left (256, 0.0f);
  std::vector<float>           output_right (256, 0.0f);
  dsp::graph::ProcessBlockInfo time_info = create_time_info (0, 0, 256);

  provider_->process_audio_events (time_info, output_left, output_right);

  // Should be all zeros initially
  for (size_t i = 0; i < output_left.size (); ++i)
    {
      EXPECT_FLOAT_EQ (output_left[i], 0.0f);
      EXPECT_FLOAT_EQ (output_right[i], 0.0f);
    }

  // Test that generate_audio_events can be called without crashing
  auto clip = create_simple_audio_clip ();
  provider_->generate_audio_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);

  // Process again to ensure no crash
  provider_->process_audio_events (time_info, output_left, output_right);

  // Should now have audio
  bool has_audio = false;
  for (size_t i = 0; i < output_left.size (); ++i)
    {
      if (
        std::abs (output_left[i]) > 0.001f
        || std::abs (output_right[i]) > 0.001f)
        {
          has_audio = true;
          break;
        }
    }
  EXPECT_TRUE (has_audio);
}

// ========== Transport State Tests ==========

TEST_F (ClipPlaybackDataProviderTest, MidiAllNotesOffWhenClipStops)
{
  // Create a MIDI clip
  auto clip = create_simple_region ();

  // Generate events for the clip
  provider_->generate_midi_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);

  auto output_buffer = dsp::MidiEventBuffer::make_reserved ();
  dsp::graph::ProcessBlockInfo time_info = create_time_info (0, 0, 256);

  // First, process events to establish playing state
  provider_->process_midi_events (time_info, output_buffer);
  EXPECT_GT (output_buffer.size (), 0);
  EXPECT_TRUE (provider_->playing ());

  // Clear the queued events for next test
  output_buffer.clear ();

  // Stop playback
  provider_->queue_stop_playback (
    structure::tracks::ClipQuantizeOption::Immediate);

  // Process events again - should send all-notes-off due to transition
  provider_->process_midi_events (time_info, output_buffer);
  EXPECT_FALSE (provider_->playing ());

  // Should have exactly 16 events: all-notes-off on each channel
  EXPECT_EQ (output_buffer.size (), 16);

  // Verify they are all all-notes-off events (CC 123)
  for (const auto &event : output_buffer)
    {
      EXPECT_TRUE (
        utils::midi::midi_is_all_notes_off (event.data ())); // Control change
                                                             // All notes off
    }

  // Clear the queued events for next test
  output_buffer.clear ();

  // Process events a third time - should have no events (transition already
  // handled)
  provider_->process_midi_events (time_info, output_buffer);
  EXPECT_EQ (output_buffer.size (), 0);
}

TEST_F (ClipPlaybackDataProviderTest, AudioDoesNotGetStuckAfterClear)
{
  // Create a simple audio clip
  auto clip = create_simple_audio_clip ();

  // Generate events for the clip
  provider_->generate_audio_events (
    *clip, structure::tracks::ClipQuantizeOption::Immediate);

  std::vector<float>           output_left (256, 0.0f);
  std::vector<float>           output_right (256, 0.0f);
  dsp::graph::ProcessBlockInfo time_info = create_time_info (0, 0, 256);

  // First, process audio to establish playing state
  provider_->process_audio_events (time_info, output_left, output_right);

  // Should have some audio output
  bool has_audio_before = false;
  for (size_t i = 0; i < output_left.size (); ++i)
    {
      if (
        std::abs (output_left[i]) > 0.001f
        || std::abs (output_right[i]) > 0.001f)
        {
          has_audio_before = true;
          break;
        }
    }
  EXPECT_TRUE (has_audio_before);
  EXPECT_TRUE (provider_->playing ());

  // Stop playback
  provider_->queue_stop_playback (
    structure::tracks::ClipQuantizeOption::Immediate);

  // Clear buffers for next test
  std::ranges::fill (output_left, 0.0f);
  std::ranges::fill (output_right, 0.0f);

  // Process audio again - should have no new audio (provider doesn't clear
  // buffers)
  provider_->process_audio_events (time_info, output_left, output_right);
  EXPECT_FALSE (provider_->playing ());

  // Buffers should remain unchanged (provider adds to existing content)
  for (size_t i = 0; i < output_left.size (); ++i)
    {
      EXPECT_FLOAT_EQ (output_left[i], 0.0f);
      EXPECT_FLOAT_EQ (output_right[i], 0.0f);
    }
}

} // namespace zrythm::structure::scenes
