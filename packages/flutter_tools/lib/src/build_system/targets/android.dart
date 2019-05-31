// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:xml/xml.dart' as xml;

import '../../base/common.dart';
import '../../base/file_system.dart';
import '../../base/io.dart';
import '../../base/process_manager.dart';
import '../../build_info.dart';
import '../../globals.dart';
import '../build_system.dart';

const String aapt = '/Users/jonahwilliams/Library/Android/sdk/build-tools/28.0.3/aapt';
const String androidJar = '/Users/jonahwilliams/Library/Android/sdk/platforms/android-28/android.jar';
const String dx = '/Users/jonahwilliams/Library/Android/sdk/build-tools/28.0.3/dx';
const String zipAligner = '/Users/jonahwilliams/Library/Android/sdk/build-tools/28.0.3/zipalign';
const String apkSigner = '/Users/jonahwilliams/Library/Android/sdk/build-tools/28.0.3/apksigner';


String parseMainActivity(String androidManifestPath) {
  if (_mainActivity != null) {
    return _mainActivity;
  }
  final xml.XmlDocument document = xml.parse(fs.file(androidManifestPath).readAsStringSync());
  final xml.XmlElement element = document.findAllElements('application').single;
  final Iterable<xml.XmlElement> activities = element.findAllElements('activity').toList();
  if (activities.length == 1) {
    final xml.XmlAttribute nameAttr = activities.single.attributes.firstWhere((xml.XmlAttribute attribute) {
      return attribute.name.local == 'name';
    });
    return _mainActivity = nameAttr.value;
  }
  throw UnsupportedError('Only single activity applications are supported');
}
String _mainActivity;

/// Lists all resource files under `{PROJECT_DIR}/android/res`.
List<FileSystemEntity> listResources(Environment environment) {
  final Directory resourceDirectory = fs.directory(fs.path.join(
    environment.projectDir.path,
    'android',
    'res',
  ));
  return resourceDirectory
      .listSync(recursive: true)
      .whereType<File>()
      .toList();
}

/// List the location of the output R.java file.
List<FileSystemEntity> listResourceOutputs(Environment environment) {
  final String mainActivity = parseMainActivity(fs.path.join(
    environment.projectDir.path,
    'android',
    'AndroidManifest.xml'
  ));
  final String outputPath = fs.path.joinAll(mainActivity.split('.')..removeLast());
  return <File>[fs.file(fs.path.join(environment.projectDir.path, 'android', 'gen', outputPath, 'R.java'))];
}

/// list all Java source files.
List<FileSystemEntity> listJavaSources(Environment environment) {
  final Directory resourceDirectory = fs.directory(fs.path.join(
    environment.projectDir.path,
    'android',
    'src',
  ));
  return resourceDirectory
      .listSync(recursive: true)
      .whereType<File>()
      .where((File file) => fs.path.extension(file.path) == 'java')
      .toList();
}

/// List the correct Fltuter.jar for the current build mode.
List<FileSystemEntity> listEngineSources(Environment environment) {
  final String targetPlatform = environment.targetPlatform == TargetPlatform.android_arm64
      ? 'arm64' : 'arm';
  String directoryName;
  switch (environment.buildMode) {
    case BuildMode.debug:
      directoryName = 'android-$targetPlatform';
      break;
    case BuildMode.release:
      directoryName = 'android-$targetPlatform-release';
      break;
    case BuildMode.profile:
      directoryName = 'android-$targetPlatform-profile';
      break;
    default:
      throwToolExit('${environment.buildMode} is not currently supported');
  }
  return <File>[
    fs.file(environment.cacheDir
        .childDirectory(directoryName)
        .uri
        .resolve('flutter.jar')
        .toFilePath()),
  ];
}

/// Generate the resource file for an Android application.
Future<void> generateResource(List<FileSystemEntity> inputs, Environment environment) async {
  final ProcessResult result = await processManager.run(<String>[
    aapt,
    'package',
    '-f',
    '-M',
    fs.path.join(environment.projectDir.path, 'android', 'AndroidManifest.xml'),
    '-I',
    androidJar,
    '-J',
    fs.path.join(environment.projectDir.path, 'android', 'gen'),
    '-m',
    '-S',
    fs.path.join(environment.projectDir.path, 'android', 'res'),
  ]);
  if (result.exitCode != 0) {
    printError(result.stderr);
    throwToolExit('android_generate_resource failed with exit code ${result.exitCode}');
  }
}

/// List the outputs of javac compilation.
List<FileSystemEntity> listJavaOutputs(Environment environment) {
  final String mainActivity = parseMainActivity(fs.path.join(
    environment.projectDir.path,
    'android',
    'AndroidManifest.xml'
  ));
  final String path = fs.path.joinAll(mainActivity.split('.')
    ..removeLast());
  final String activityName = mainActivity.split('.').last;
  final String prefix = fs.path.join(
    environment.projectDir.path,
    'android',
    'obj',
    path,
  );
  return <File>[
    fs.file(fs.path.join(prefix, 'R.class')),
    fs.file(fs.path.join(prefix, '$activityName.class')),
    fs.file(fs.path.join(prefix, 'R\$attr.class')),
    fs.file(fs.path.join(prefix, 'R\$layout.class')),
  ];
}

/// Compile the java code using javac.
Future<void> compileJava(List<FileSystemEntity> inputs, Environment environment) async {
  final String mainActivity = parseMainActivity(fs.path.join(
    environment.projectDir.path,
    'android',
    'AndroidManifest.xml'
  ));
  final String prefix = fs.path.joinAll(mainActivity.split('.')..removeLast());
  final String activityName = mainActivity.split('.').last;
  final ProcessResult result = await processManager.run(<String>[
    'javac',
    '-d',
    fs.path.join(environment.projectDir.path, 'android', 'obj'),
    '-classpath',
    fs.path.join(environment.projectDir.path, 'android', 'src'),
    '-bootclasspath',
    androidJar,
    fs.path.join(environment.projectDir.path, 'android', 'obj', prefix, '$activityName.java'),
    fs.path.join(environment.projectDir.path, 'android', 'gen', prefix, 'R.java'),
  ]);
  if (result.exitCode != 0) {
    printError(result.stderr);
    throwToolExit('android_compile_java failed with exit code ${result.exitCode}');
  }
}

/// Translate java bytecode in dalvik executable format.
Future<void> translateDex(List<FileSystemEntity> inputs, Environment environment) async {
  final ProcessResult result = await processManager.run(<String>[
    dx,
    '--dex',
    '--output=classes.dex',
    fs.path.join(environment.projectDir.path, 'android', 'obj/'),
  ]);
  if (result.exitCode != 0) {
    printError(result.stderr);
    throwToolExit('android_translate_dalvik failed with exit code ${result.exitCode}');
  }
}

/// Perform the initial packaging for an APK.
Future<void> performPackageApk(List<FileSystemEntity> inputs, Environment environment) async {
  final String apkName = getNameForBuildMode(environment.buildMode);
  final String apkPath = fs.path.join(environment.projectDir.path, 'android', 'bin', '$apkName.unaligned.apk');
  // build unaligned APK.
  final ProcessResult packageResult = await processManager.run(<String>[
    aapt,
    'package',
    '-f',
    '-m',
    '-F',
    apkPath,
    '-M',
    fs.path.join(environment.projectDir.path, 'android', 'AndroidManifest.xml'),
    '-S',
    fs.path.join(environment.projectDir.path, 'android', 'res'),
    '-I',
    androidJar,
  ]);
  if (packageResult.exitCode != 0) {
    printError(packageResult.stderr);
    throwToolExit('android_package_apk failed with exit code ${packageResult.exitCode}');
  }
  // add dex file.
  final ProcessResult addResult = await processManager.run(<String>[
    aapt,
    'add',
    apkPath,
    fs.path.join(environment.projectDir.path, 'android', 'classes.dex'),
  ]);
  if (addResult.exitCode != 0) {
    printError(addResult.stderr);
    throwToolExit('android_package_apk failed with exit code ${addResult.exitCode}');
  }
}

/// Sign (and align) an apk.
Future<void> performSignApk(List<FileSystemEntity> inputs, Environment environment) async {
  final String apkName = getNameForBuildMode(environment.buildMode);
  final String unalignedApk = fs.path.join(environment.projectDir.path, 'android', 'bin', '$apkName.unaligned.apk');
  final String outputPath = fs.path.join(environment.buildDir.path, 'android', '$apkName.apk');
  final ProcessResult alignResult = await processManager.run(<String>[
    zipAligner,
    '-f',
    '4',
    unalignedApk,
    outputPath,
  ]);
  if (alignResult.exitCode != 0) {
    printError(alignResult.stderr);
    throwToolExit('android_sign_apk failed with exit code ${alignResult.exitCode}');
  }
  final ProcessResult signResult = await processManager.run(<String>[
    apkSigner,
    'sign',
    '--ks',
    'mykey.keystore',
    outputPath,
  ]);
  if (signResult.exitCode != 0) {
    printError(signResult.stderr);
    throwToolExit('android_sign_apk failed with exit code ${alignResult.exitCode}');
  }
}

/// Generate the Resource file R.java.
const Target androidGenerateResource = Target(
  name: 'android_generate_resource',
  inputs: <Source>[
    Source.function(listResources),
    Source.pattern('{PROJECT_DIR}/android/AndroidManifest.xml'),
  ],
  outputs: <Source>[
    Source.function(listResourceOutputs),
  ],
  dependencies: <Target>[],
  invocation: generateResource,
);

/// Compile the java code using javac.
const Target androidCompileJava = Target(
  name: 'android_compile_java',
  dependencies: <Target>[
    androidGenerateResource,
  ],
  inputs: <Source>[
    Source.function(listJavaSources),
    Source.function(listEngineSources),
  ],
  outputs: <Source>[
    Source.function(listJavaOutputs),
  ],
  invocation: compileJava,
);

/// Translate java bytecode into dex.
const Target translateDalvik = Target(
  name: 'android_translate_dalvik',
  dependencies: <Target>[
    androidCompileJava,
  ],
  inputs: <Source>[],
  outputs: <Source>[
    Source.pattern('{PROJECT_DIR}/android/classes.dex')
  ],
  invocation: translateDex,
);

/// Package the initial unsigned and unalighed apk.
const Target packageApk = Target(
  name: 'android_package_apk',
  inputs: <Source>[],
  outputs: <Source>[
    Source.pattern('{PROJECT_DIR}/android/bin/{mode}.unaligned.apk')
  ],
  dependencies: <Target>[
    translateDalvik,
  ],
  invocation: performPackageApk,
);

/// Sign the APK.
const Target signApk = Target(
  name: 'android_sign_apk',
  dependencies: <Target>[
    packageApk
  ],
  inputs: <Source>[],
  outputs: <Source>[
    Source.pattern('{OUTPUT_DIR}/android/{mode}.apk'),
  ],
  invocation: performSignApk,
);

const List<Target> allTargets = <Target>[
  androidGenerateResource,
  androidCompileJava,
  translateDalvik,
  packageApk,
  signApk,
];
