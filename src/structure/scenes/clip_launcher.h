// SPDX-FileCopyrightText: © 2025-2026 Alexandros Theodotou <alex@zrythm.org>
// SPDX-License-Identifier: LicenseRef-ZrythmLicense

#pragma once

#include "structure/scenes/scene.h"

#include <QtQmlIntegration/qqmlintegration.h>

#include <nlohmann/json_fwd.hpp>

namespace zrythm::structure::scenes
{
class ClipLauncher : public QObject
{
  Q_OBJECT
  Q_PROPERTY (zrythm::structure::scenes::SceneList * scenes READ scenes CONSTANT)
  Q_PROPERTY (
    zrythm::structure::tracks::ClipQuantizeOption launchQuantization READ
      launchQuantization WRITE setLaunchQuantization NOTIFY
        launchQuantizationChanged)
  QML_ELEMENT
  QML_UNCREATABLE ("")

public:
  ClipLauncher (
    utils::IObjectRegistry        &registry,
    const tracks::TrackCollection &track_collection,
    QObject *                      parent = nullptr);

  SceneList * scenes () const { return scene_list_.get (); }

  /**
   * @brief Quantization used when clips or scenes are launched or stopped
   * without choosing one explicitly.
   */
  tracks::ClipQuantizeOption launchQuantization () const
  {
    return launch_quantization_;
  }
  void          setLaunchQuantization (tracks::ClipQuantizeOption quantization);
  Q_SIGNAL void launchQuantizationChanged ();

  // Scene management
  Q_INVOKABLE Scene * addScene ();

  /**
   * @brief Creates an empty scene (with a slot for each track) that is not
   * part of the scene list yet.
   */
  utils::QObjectUniquePtr<Scene> create_scene () const;

private:
  static constexpr auto kScenesKey = "scenes"sv;
  static constexpr auto kLaunchQuantizationKey = "launchQuantization"sv;
  friend void to_json (nlohmann::json &j, const ClipLauncher &launcher);
  friend void from_json (const nlohmann::json &j, ClipLauncher &launcher);

private:
  utils::QObjectUniquePtr<SceneList> scene_list_;
  tracks::ClipQuantizeOption         launch_quantization_{
    tracks::ClipQuantizeOption::NextBar
  };

  utils::IObjectRegistry        &registry_;
  const tracks::TrackCollection &track_collection_;
};

} // namespace zrythm::structure::scenes
