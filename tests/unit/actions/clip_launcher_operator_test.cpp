// SPDX-FileCopyrightText: © 2026 Hannes Janetzek <hannes.janetzek@gmail.com>
// SPDX-License-Identifier: LicenseRef-ZrythmLicense

#include "actions/clip_launcher_operator.h"
#include "structure/arrangement/midi_clip.h"
#include "structure/tracks/track_all.h"
#include "structure/tracks/track_collection.h"
#include "utils/object_registry.h"
#include "utils/registry_utils.h"

#include "helpers/scoped_qcoreapplication.h"

#include "unit/actions/mock_undo_stack.h"
#include <gtest/gtest.h>

namespace zrythm::actions
{

class ClipLauncherOperatorTest : public ::testing::Test
{
protected:
  void SetUp () override
  {
    track_collection_ =
      std::make_unique<structure::tracks::TrackCollection> (registry_);
    structure::tracks::FinalTrackDependencies deps{
      tempo_map_wrapper_, registry_, [] { return false; }, {}
    };
    track_collection_->add_track (
      utils::create_object<structure::tracks::MidiTrack> (
        registry_, std::move (deps)));

    clip_launcher_ = std::make_unique<structure::scenes::ClipLauncher> (
      registry_, *track_collection_);
    undo_stack_ = create_mock_undo_stack ();

    operator_ = std::make_unique<ClipLauncherOperator> ();
    operator_->setClipLauncher (clip_launcher_.get ());
    operator_->setUndoStack (undo_stack_.get ());
  }

  void TearDown () override
  {
    operator_.reset ();
    undo_stack_.reset ();
    clip_launcher_.reset ();
    track_collection_.reset ();
  }

  structure::scenes::SceneList &scene_list ()
  {
    return *clip_launcher_->scenes ();
  }

  structure::scenes::Scene * scene_at (int index)
  {
    return scene_list ().scenes ().at (static_cast<size_t> (index)).get ();
  }

  // Adds three scenes named "A", "B" and "C"
  void add_named_scenes ()
  {
    for (const auto &name : { "A", "B", "C" })
      operator_->addScene ()->setName (QString::fromLatin1 (name));
  }

  QStringList scene_names ()
  {
    QStringList names;
    for (const auto &scene : scene_list ().scenes ())
      names.append (scene->name ());
    return names;
  }

  test_helpers::ScopedQCoreApplication qapp_;
  dsp::TempoMap         tempo_map_{ units::sample_rate (44100.0) };
  dsp::TempoMapWrapper  tempo_map_wrapper_{ tempo_map_ };
  utils::ObjectRegistry registry_;
  std::unique_ptr<structure::tracks::TrackCollection> track_collection_;
  std::unique_ptr<structure::scenes::ClipLauncher>    clip_launcher_;
  std::unique_ptr<undo::UndoStack>                    undo_stack_;
  std::unique_ptr<ClipLauncherOperator>               operator_;
};

TEST_F (ClipLauncherOperatorTest, AddSceneUndoRedo)
{
  auto * scene = operator_->addScene ();
  ASSERT_NE (scene, nullptr);
  EXPECT_EQ (scene_list ().rowCount (), 1);
  EXPECT_EQ (scene_at (0), scene);
  EXPECT_EQ (scene->clipSlots ()->rowCount (), 1);

  undo_stack_->undo ();
  EXPECT_EQ (scene_list ().rowCount (), 0);

  // Redo restores the same scene
  undo_stack_->redo ();
  EXPECT_EQ (scene_list ().rowCount (), 1);
  EXPECT_EQ (scene_at (0), scene);
}

TEST_F (ClipLauncherOperatorTest, InsertSceneAtIndex)
{
  add_named_scenes ();
  operator_->insertScene (1)->setName (QStringLiteral ("X"));
  EXPECT_EQ (scene_names (), (QStringList{ "A", "X", "B", "C" }));

  EXPECT_EQ (operator_->insertScene (10), nullptr);
  EXPECT_EQ (scene_list ().rowCount (), 4);
}

TEST_F (ClipLauncherOperatorTest, RemoveSceneKeepsItsClipsForUndo)
{
  add_named_scenes ();
  auto * scene_b = scene_at (1);
  auto * track = track_collection_->get_track_ref_at_index (0).get ();
  auto * slot = scene_b->clipSlots ()->clipSlotForTrack (track);
  auto   clip_ref = utils::create_object<structure::arrangement::MidiClip> (
    registry_, tempo_map_wrapper_, registry_);
  auto * clip = clip_ref.get_object_as<structure::arrangement::MidiClip> ();
  slot->setClip (clip);

  operator_->removeScene (scene_b);
  EXPECT_EQ (scene_names (), (QStringList{ "A", "C" }));

  undo_stack_->undo ();
  EXPECT_EQ (scene_names (), (QStringList{ "A", "B", "C" }));
  EXPECT_EQ (scene_at (1), scene_b);
  EXPECT_EQ (scene_b->clipSlots ()->clipSlotForTrack (track)->clip (), clip);

  undo_stack_->redo ();
  EXPECT_EQ (scene_names (), (QStringList{ "A", "C" }));
}

TEST_F (ClipLauncherOperatorTest, MoveSceneBothDirectionsUndoRedo)
{
  add_named_scenes ();

  operator_->moveScene (scene_at (0), 2);
  EXPECT_EQ (scene_names (), (QStringList{ "B", "C", "A" }));
  operator_->moveScene (scene_at (2), 1);
  EXPECT_EQ (scene_names (), (QStringList{ "B", "A", "C" }));

  undo_stack_->undo ();
  EXPECT_EQ (scene_names (), (QStringList{ "B", "C", "A" }));
  undo_stack_->undo ();
  EXPECT_EQ (scene_names (), (QStringList{ "A", "B", "C" }));
  undo_stack_->redo ();
  EXPECT_EQ (scene_names (), (QStringList{ "B", "C", "A" }));
}

TEST_F (ClipLauncherOperatorTest, RenameAndColorSceneUndoRedo)
{
  auto * scene = operator_->addScene ();

  operator_->renameScene (scene, QStringLiteral ("Verse"));
  EXPECT_EQ (scene->name (), QStringLiteral ("Verse"));
  operator_->setSceneColor (scene, QColor (Qt::red));
  EXPECT_EQ (scene->color (), QColor (Qt::red));

  undo_stack_->undo ();
  EXPECT_FALSE (scene->color ().isValid ());
  undo_stack_->undo ();
  EXPECT_EQ (scene->name (), QString ());
  undo_stack_->redo ();
  EXPECT_EQ (scene->name (), QStringLiteral ("Verse"));
}

TEST_F (ClipLauncherOperatorTest, OperationsWithoutTargetAreRefused)
{
  // Scene that is not part of the clip launcher
  auto      foreign_scene = clip_launcher_->create_scene ();
  const int initial_count = undo_stack_->count ();
  operator_->removeScene (foreign_scene.get ());
  operator_->moveScene (foreign_scene.get (), 0);
  operator_->renameScene (nullptr, QStringLiteral ("X"));
  EXPECT_EQ (undo_stack_->count (), initial_count);

  operator_->setUndoStack (nullptr);
  EXPECT_EQ (operator_->addScene (), nullptr);
  EXPECT_EQ (scene_list ().rowCount (), 0);
}

} // namespace zrythm::actions
