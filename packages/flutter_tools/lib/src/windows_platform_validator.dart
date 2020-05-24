// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:flutter_tools/src/base/io.dart';
import 'package:meta/meta.dart';
import 'package:process/process.dart';

import 'doctor.dart';

const String kWindowsGitUrl = 'https://git-scm.com/download/win';

/// A validator that checks if the prerequisties for running Flutter exist.
class WindowsPlatformValidator extends DoctorValidator {
  WindowsPlatformValidator({
    @required ProcessManager processManager,
  }) : _processManager = processManager,
       super('Platform Dependencies (Windows)');

  final ProcessManager _processManager;

  @override
  Future<ValidationResult> validate() async {
    bool gitInstalled = false;
    try {
      gitInstalled = _processManager.canRun('git');
    } on ArgumentError {
      // Do nothing.
    }
    if (!gitInstalled) {
      return const ValidationResult(
        ValidationType.missing,
        <ValidationMessage>[
          ValidationMessage(
            'git for Windows is missing. Flutter will not be '
            'able to upgrade or determine its version.'),
          ValidationMessage(
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
            'git for Windows is present but the version could not be determined.'),
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
