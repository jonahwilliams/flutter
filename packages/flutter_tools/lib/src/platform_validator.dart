// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:meta/meta.dart';
import 'package:process/process.dart';

import 'base/io.dart';
import 'base/platform.dart';
import 'doctor.dart';

const String kWindowsGitUrl = 'https://git-scm.com/download/win';

/// A validator that checks if the prerequisties for running Flutter exist.
class PlatformValidator extends DoctorValidator {
  PlatformValidator({
    @required ProcessManager processManager,
    @required Platform platform,
  }) : _processManager = processManager,
       _platform = platform,
       super('Platform Dependencies (${platform.operatingSystem})');

  final ProcessManager _processManager;
  final Platform _platform;

  @override
  Future<ValidationResult> validate() async {
    bool gitInstalled = false;
    try {
      gitInstalled = _processManager.canRun('git');
    } on ArgumentError {
      // Do nothing.
    }
    if (!gitInstalled) {
      return ValidationResult(
        ValidationType.missing,
        <ValidationMessage>[
          ValidationMessage(
            'git for ${_platform.operatingSystem} is missing. Flutter will not be '
            'able to upgrade or determine its version.'),
          if (_platform.isWindows)
            const ValidationMessage(
              'install git at $kWindowsGitUrl .'
            )
        ]
      );
    }

    String gitVersion;
    ProcessResult versionResult;
    try {
      versionResult = await _processManager.run(<String>['git', '--version']);
      if (versionResult.exitCode == 0) {
        gitVersion = versionResult.stdout as String;
      }
    } on ProcessException {
      // Do nothing.
    }

    if (gitVersion == null) {
      return ValidationResult(
        ValidationType.partial,
        <ValidationMessage>[
          const ValidationMessage(
            'git is present but the version could not be determined.'),
          ValidationMessage(
            '"git --version" returned exit code ${versionResult?.exitCode ?? 1}.'
          )
        ]
      );
    }

    return ValidationResult(
        ValidationType.installed,
        <ValidationMessage>[
          ValidationMessage(gitVersion)
        ]
      );
  }
}
