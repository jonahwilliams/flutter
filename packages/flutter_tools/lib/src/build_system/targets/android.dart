// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';

import '../../base/file_system.dart';
import '../../base/os.dart';
import '../../build_info.dart';
import '../build_system.dart';
import '../exceptions.dart';
import 'dart.dart';

/// Build an AAR to package flutter artifacts for Android.
///
/// See https://developer.android.com/studio/projects/android-library.html#aar-contents
class AndroidAarTarget extends Target {
  @override
  String get name => 'android_aar';

  @override
  FutureOr<void> build(Environment environment) {
    if (environment.defines[kTargetPlatform] == null) {
      throw MissingDefineException(kTargetPlatform, 'android_aar');
    }
    final TargetPlatform targetPlatform = getTargetPlatformForName(environment.defines[kTargetPlatform]);
    final Directory scratchSpace = fs.systemTempDirectory
      .createTempSync('_flutter_tools')
      ..createSync();
    // Create mandatory files
    fs.file(fs.path.join(scratchSpace.path, 'AndroidManifest.xml'))
      .writeAsStringSync('');
    fs.file(fs.path.join(scratchSpace.path, 'classes.jar'))
      .writeAsStringSync('');
    fs.file(fs.path.join(scratchSpace.path, 'R.txt'))
      .writeAsStringSync('');
    fs.file(fs.path.join(scratchSpace.path, 'R.public.txt'))
      .writeAsStringSync('');
    fs.directory(fs.path.join(scratchSpace.path, 'res'))
      .createSync();
    // Bundle existing SO.
    AndroidArch androidArch;
    switch (targetPlatform) {
      case TargetPlatform.android_arm:
        androidArch = AndroidArch.armeabi_v7a;
        break;
      case TargetPlatform.android_arm64:
        androidArch = AndroidArch.arm64_v8a;
        break;
      case TargetPlatform.android_x64:
        androidArch = AndroidArch.x86_64;
        break;
      default:
        throw Exception('unsupported target platform: $targetPlatform');
    }
    final String path = fs.path.join(scratchSpace.path, 'jni', getNameForAndroidArch(androidArch), 'app.so');
    environment.buildDir.childFile('app.so').copySync(path);
    os.zip(scratchSpace, environment.outputDir.childFile('flutter.aar'));
  }

  @override
  List<Target> get dependencies => const <Target>[
    AotElfRelease(),
  ];

  @override
  List<Source> get inputs => const <Source>[
    Source.pattern('{BUILD_DIR}/app.so'),
    Source.pattern('{FLUTTER_ROOT}/packages/flutter_tools/lib/src/build_system/targets/android.dart'),
  ];

  @override
  List<Source> get outputs => const <Source>[
    Source.pattern('{OUTPUT_DIR}/flutter.aar')
  ];
}
