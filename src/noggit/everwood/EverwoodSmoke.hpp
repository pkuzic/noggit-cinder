// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// Everwood smoke-test hook. Noggit has no command line for opening a project, so an automated run of
// the real application is driven by a small text script named by the environment variable
// NOGGIT_EVERWOOD_SMOKE. With the variable unset nothing in this file does anything and main() takes
// the normal path. See EVERWOOD-MODE.md for the script commands.
#pragma once

namespace Noggit::Application
{
  class NoggitApplication;
}

namespace Noggit::Everwood
{
  // True when NOGGIT_EVERWOOD_SMOKE names a script.
  bool smokeEnabled();

  // Must run before anything constructs a QSettings: sends every default QSettings to an ini file
  // beside the script, so a test run never reads or writes the owner's Noggit settings.
  void smokePrepareSettings();

  // Replaces the project selection window: runs the script, then ends the process with its result.
  void smokeStart(Noggit::Application::NoggitApplication* application);

  class SmokeRunner;
}
