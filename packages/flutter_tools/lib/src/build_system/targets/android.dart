// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:flutter_tools/src/base/io.dart';
import 'package:flutter_tools/src/build_system/targets/dart.dart';

import '../../artifacts.dart';
import '../../base/file_system.dart';
import '../../base/process_manager.dart';
import '../../build_info.dart';
import '../../globals.dart';
import '../build_system.dart';
import 'assets.dart';

// TODO(jonahwilliams)
/// Refactor into artifacts.
const String aapt =
'/Users/jonahwilliams/Library/Android/sdk/build-tools/28.0.3/aapt';
const String androidJar =
    '/Users/jonahwilliams/Library/Android/sdk/platforms/android-28/android.jar';
const String dexer =
    '/Users/jonahwilliams/Library/Android/sdk/build-tools/28.0.3/dx';
const String zipAlign =
    '/Users/jonahwilliams/Library/Android/sdk/build-tools/28.0.3/zipalign';
const String apksigner =
    '/Users/jonahwilliams/Library/Android/sdk/build-tools/28.0.3/apksigner';

/// Known during planning.
const String package = 'foo/bar/baz';

/// All resource files.
List<File> resourceFiles(Environment environment) {
  final Directory resources = environment.buildDir.childDirectory('res');
  if (!resources.existsSync()) {
    return <File>[];
  }
  return resources
      .listSync(recursive: true)
      .whereType<File>()
      .toList();
}

class GenerateEphemeralProject extends Target {
  const GenerateEphemeralProject();

  @override
  Future<void> build(List<File> inputFiles, Environment environment) async {
    environment.buildDir.childFile('AndroidManifest.xml')
      ..writeAsStringSync(r'''
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    android:versionCode="1"
    android:versionName="example"
    package="foo.bar.baz" >

    <uses-sdk android:minSdkVersion="16" android:targetSdkVersion="28"/>

    <uses-permission android:name="android.permission.INTERNET" />

    <application android:label="hello_world">
        <activity android:name="foo.bar.baz.MainActivity" >
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
    </application>
</manifest>
''');
    final String activityPath = fs.path.join(
      environment.buildDir.path,
      'src',
      package,
      'MainActivity.java',
    );
    fs.file(activityPath)
      ..createSync(recursive: true)
      ..writeAsStringSync(r'''
package foo.bar.baz;

import android.os.Bundle;
import io.flutter.app.FlutterActivity;
import android.os.Bundle;

public class MainActivity extends FlutterActivity {
  @Override
  protected void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
  }
}
''');
  }

  @override
  List<Target> get dependencies => <Target>[];

  @override
  List<Source> get inputs => <Source>[];

  @override
  String get name => 'generate_ephemeral_app';

  @override
  List<Source> get outputs => const <Source>[
    Source.pattern('{BUILD_DIR}/AndroidManifest.xml'),
    Source.pattern('{BUILD_DIR}/src/$package/MainActivity.java'),
  ];
}

/// Generates an R.java file from resources.
class ResourceFileTarget extends Target {
  const ResourceFileTarget();

  @override
  String get name => 'java_resource';

  @override
  List<Target> get dependencies => const <Target>[
    GenerateEphemeralProject(),
  ];

  @override
  List<Source> get inputs => const <Source>[
    Source.pattern('{BUILD_DIR}/AndroidManifest.xml'),
    Source.function(resourceFiles),
  ];

  @override
  List<Source> get outputs => const <Source>[
    //Source.pattern('{BUILD_DIR}/gen/$package/R.java'),
  ];

  @override
  Future<void> build(List<File> inputFiles, Environment environment) async {
    final File androidManifest = environment
        .buildDir.childFile('AndroidManifest.xml');
    final Directory resources = environment.buildDir.childDirectory('res');
    final Directory generated = environment.buildDir.childDirectory('gen');
    resources.createSync();
    generated.existsSync();

    final ProcessResult result = await processManager.run(<String>[
      aapt,
      'package',
      '-f',
      '-M',
      androidManifest.path,
      '-I',
      androidJar,
      '-S',
      resources.path,
      '-J',
      generated.path,
    ]);
    if (result.exitCode != 0) {
      throw Exception(result.stderr);
    }
  }
}

class CompileJavaTarget extends Target {
  const CompileJavaTarget();

  @override
  String get name => 'compile_java';

  @override
  List<Target> get dependencies => const <Target>[
    ResourceFileTarget(),
  ];

  @override
  List<Source> get inputs => const <Source>[
    Source.pattern('{BUILD_DIR}/src/$package/MainActivity.java'),
    // Source.pattern('{BUILD_DIR}/gen/$package/R.java'),
  ];

  @override
  List<Source> get outputs => const <Source>[
    Source.pattern('{BUILD_DIR}/obj/$package/MainActivity.class'),
    // Source.pattern('{BUILD_DIR}/obj/$package/R.class'),
    // Source.pattern('{BUILD_DIR}/obj/$package/R\$attr.class'),
    // Source.pattern('{BUILD_DIR}/obj/$package/R\$layout.class'),
  ];

  @override
  Future<void> build(List<File> inputFiles, Environment environment) async {
    environment.buildDir.childDirectory('obj').createSync();
    final File flutterJar = cache.getArtifactDirectory('engine').childDirectory('android-arm').childFile('flutter.jar');
    final String resourceFile = fs.path.join(environment.buildDir.path, 'gen', package, 'R.java');
    final ProcessResult result = await processManager.run(<String>[
      'javac',
      '-d',
      environment.buildDir.childDirectory('obj').path,
      '-classpath',
      <String>[
        environment.buildDir.childDirectory('src').path,
        flutterJar.path,
      ].join(':'),
      '-bootclasspath',
      androidJar,
      fs.path
          .join(environment.buildDir.path, 'src', package, 'MainActivity.java'),
      if (fs.isFileSync(resourceFile))
        fs.path.join(environment.buildDir.path, 'gen', package, 'R.java'),
    ]);
    if (result.exitCode != 0) {
      throw Exception(result.stderr);
    }
  }
}

class DexJavaTarget extends Target {
  const DexJavaTarget();

  @override
  String get name => 'dex_java';

  @override
  List<Target> get dependencies => const <Target>[
    CompileJavaTarget(),
  ];

  @override
  List<Source> get inputs => const <Source>[
    Source.pattern('{BUILD_DIR}/obj/$package/MainActivity.class'),
    // Source.pattern('{BUILD_DIR}/obj/$package/R.class'),
    // Source.pattern('{BUILD_DIR}/obj/$package/R\$attr.class'),
    // Source.pattern('{BUILD_DIR}/obj/$package/R\$layout.class'),
  ];

  @override
  List<Source> get outputs => const <Source>[
    Source.pattern('{BUILD_DIR}/bin/classes.dex'),
  ];

  @override
  Future<void> build(List<File> inputFiles, Environment environment) async {
    final File output = environment.buildDir
        .childDirectory('bin')
        .childFile('classes.dex');
    output.parent.createSync();
    final ProcessResult result = await processManager.run(<String>[
      dexer,
      '--dex',
      '--output=${output.path}',
      environment.buildDir.childDirectory('obj').path,
    ]);
    if (result.exitCode != 0) {
      throw Exception(result.stderr);
    }
  }
}

/// Copies the precompiled runtime for Android targets.
class CopyPrecompiledRuntime extends Target {
  const CopyPrecompiledRuntime();

  @override
  String get name => 'copy_precompiled_runtime';

  @override
  List<Source> get inputs => const <Source>[
    Source.artifact(Artifact.vmSnapshotData, mode: BuildMode.debug),
    Source.artifact(Artifact.isolateSnapshotData, mode: BuildMode.debug),
  ];

  @override
  List<Source> get outputs => const <Source>[
    Source.pattern('{BUILD_DIR}/vm_snapshot_data'),
    Source.pattern('{BUILD_DIR}/isolate_snapshot_data'),
  ];

  @override
  Future<void> build(List<File> inputFiles, Environment environment) async {
    final String vmSnapshotData = artifacts.getArtifactPath(Artifact.vmSnapshotData, mode: BuildMode.debug);
    final String isolateSnapshotData = artifacts.getArtifactPath(Artifact.isolateSnapshotData, mode: BuildMode.debug);
    fs.file(vmSnapshotData).copySync(environment.buildDir.childFile('vm_snapshot_data').path);
    fs.file(isolateSnapshotData).copySync(environment.buildDir.childFile('isolate_snapshot_data').path);
  }

  @override
  List<Target> get dependencies => const <Target>[];
}

class PackageApkTarget extends Target {
  const PackageApkTarget();

  @override
  String get name => 'package_apk';

  @override
  List<Target> get dependencies => const <Target>[
    DexJavaTarget(),
    CopyAssets(),
    KernelSnapshot(),
    CopyPrecompiledRuntime(),
    UnpackLibflutter(),
  ];

  @override
  List<Source> get inputs => const <Source>[
    Source.pattern('{BUILD_DIR}/bin/classes.dex'),
    Source.pattern('{BUILD_DIR}/app.dill'),
    Source.pattern('{BUILD_DIR}/vm_snapshot_data'),
    Source.pattern('{BUILD_DIR}/isolate_snapshot_data'),
    Source.pattern('{BUILD_DIR}/flutter_assets/*'),
  ];

  @override
  List<Source> get outputs => const <Source>[
    Source.pattern('{BUILD_DIR}/stump.unaligned.apk'),
  ];

  @override
  Future<void> build(List<File> inputFiles, Environment environment) async {
    final File androidManifest = environment
      .buildDir.childFile('AndroidManifest.xml');
    final File output = environment
      .buildDir.childFile('stump.unaligned.apk');
    final Directory assetDir = environment.buildDir
      .childDirectory('flutter_assets');
    environment.buildDir.childFile('app.dill')
        .copySync(assetDir.childFile('kernel_blob.bin').path);
    environment.buildDir.childFile('vm_snapshot_data')
        .copySync(assetDir.childFile('vm_snapshot_data').path);
    environment.buildDir.childFile('isolate_snapshot_data')
        .copySync(assetDir.childFile('isolate_snapshot_data').path);
    final File dexFile = environment.buildDir.childDirectory('bin').childFile('classes.dex');
    dexFile.copySync(environment.buildDir.childFile('classes.dex').path);

    copyDirectorySync(assetDir, environment.buildDir.childDirectory('foobar').childDirectory('flutter_assets')..createSync(recursive: true));

    final Directory nativeAssets = environment.buildDir.childDirectory('native_artifacts');

    final ProcessResult result = await processManager.run(<String>[
      aapt,
      'package',
      '--debug-mode',
      '-m',
      '-F',
      output.path,
      '-M',
      androidManifest.path,
      '-S',
      environment.buildDir.childDirectory('res').path,
      '-I',
      androidJar,
      '-A',
      environment.buildDir.childDirectory('foobar').path,
      nativeAssets.path,
    ]);
    if (result.exitCode != 0) {
      throw Exception(result.stderr);
    }
    // aapt is a broken.
    final Directory oldDirectory = fs.currentDirectory;
    fs.currentDirectory = environment.buildDir;
    final ProcessResult addResult = await processManager.run(<String>[
      aapt, 'add', output.path, 'classes.dex',
    ]);
     fs.currentDirectory = oldDirectory;
    if (addResult.exitCode != 0) {
      print(addResult.stderr);
    }
  }
}

class DebugKeytoolTarget extends Target {
  const DebugKeytoolTarget();

  @override
  List<Target> get dependencies => const <Target>[];

  @override
  List<Source> get inputs => <Source>[];

  @override
  String get name => 'debug_keytool_target';

  @override
  List<Source> get outputs => const <Source>[
    Source.pattern('{BUILD_DIR}/stump.keystore')
  ];

  @override
  Future<void> build(List<File> inputFiles, Environment environment) async {
    const String debugString = 'CN=cName, OU=orgUnit, O=org, L=city, S=state, C=countryCode';
    final File outputKeystore = environment.buildDir.childFile('stump.keystore');
    final ProcessResult result = await processManager.run(<String>[
      'keytool',
      '-genkeypair',
      '-validity',
      '365',
      '-keystore',
      outputKeystore.path,
      '-keyalg',
      'RSA',
      '-keysize',
      '2048',
      '-storepass', 'foobarbaz',
      '-keypass', 'foobarbaz',
      '-dname', debugString,
    ]);
    if (result.exitCode != 0) {
      throw Exception(result.stderr);
    }
  }
}

/// For some reason the libflutter.so are bundled in the jars.
class UnpackLibflutter extends Target {
  const UnpackLibflutter();

  @override
  String get name => 'unpack_lib_flutter';

  @override
  List<Target> get dependencies => <Target>[];

  @override
  List<Source> get inputs => <Source>[
    // TODO
  ];

  @override
  List<Source> get outputs => const <Source>[
    Source.pattern('{BUILD_DIR}/native_artifacts/lib/x86/libflutter.so'),
    Source.pattern('{BUILD_DIR}/native_artifacts/lib/x86_64/libflutter.so'),
    Source.pattern('{BUILD_DIR}/native_artifacts/lib/armeabi-v7a/libflutter.so'),
    Source.pattern('{BUILD_DIR}/native_artifacts/lib/arm64-v8a/libflutter.so'),
  ];

  @override
  Future<void> build(List<File> inputFiles, Environment environment) async {
    final List<File> flutterJars = <File>[
      cache.getArtifactDirectory('engine').childDirectory('android-arm').childFile('flutter.jar'),
      cache.getArtifactDirectory('engine').childDirectory('android-arm64').childFile('flutter.jar'),
      cache.getArtifactDirectory('engine').childDirectory('android-x64').childFile('flutter.jar'),
      cache.getArtifactDirectory('engine').childDirectory('android-x86').childFile('flutter.jar'),
    ];
    final Directory output = environment.buildDir.childDirectory('native_artifacts');
    for (File flutterJar in flutterJars) {
      await processManager.run(<String>[
        'unzip',
        flutterJar.path,
        '-x', 'io/flutter/*',
        '-d', output.path,
      ]);
    }
  }
}

class ZipAlignApkTarget extends Target {
  const ZipAlignApkTarget();

  @override
  String get name => 'align_apk';

  @override
  List<Target> get dependencies => const <Target>[
    PackageApkTarget(),
    DebugKeytoolTarget(),
  ];

  @override
  List<Source> get inputs => const <Source>[
    Source.pattern('{BUILD_DIR}/stump.unaligned.apk'),
  ];

  @override
  List<Source> get outputs => const <Source>[
    Source.pattern('{BUILD_DIR}/stump.apk')
  ];

  @override
  Future<void> build(List<File> inputFiles, Environment environment) async {
    final File keystore = environment.buildDir.childFile('stump.keystore');
    final ProcessResult alignResult = await processManager.run(<String>[
      zipAlign,
      '-f', '4',
      environment.buildDir.childFile('stump.unaligned.apk').path,
      environment.buildDir.childFile('stump.unsigned.apk').path,
    ]);
    if (alignResult.exitCode != 0) {
      print(alignResult.stderr);
    }
    final ProcessResult signResult = await processManager.run(<String>[
      apksigner,
      'sign',
      '--ks', keystore.path,
      '--ks-pass=pass:foobarbaz',
      '--in', environment.buildDir.childFile('stump.unsigned.apk').path,
      '--out', environment.buildDir.childFile('stump.apk').path

    ]);
    if (signResult.exitCode != 0) {
      print(signResult.stderr);
    }
  }
}
