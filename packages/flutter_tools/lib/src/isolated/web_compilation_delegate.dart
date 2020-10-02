// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:typed_data';

import 'package:meta/meta.dart';
import 'package:package_config/package_config.dart';

import '../artifacts.dart';
import '../base/file_system.dart';
import '../base/utils.dart';
import '../build_info.dart';
import '../bundle.dart';
import '../compile.dart';
import '../convert.dart';
import '../dart/language_version.dart';
import '../globals.dart' as globals;
import '../project.dart';
import '../web/bootstrap.dart';
import '../web/compile.dart';

class BuildRunnerWebCompilationProxy extends WebCompilationProxy {
  BuildRunnerWebCompilationProxy({
    @required FileSystem fileSystem,
  }) : _fileSystem = fileSystem;

  final FileSystem _fileSystem;
  final Map<String, Uint8List> _files = <String, Uint8List>{};
  final Map<String, Uint8List> _sourcemaps = <String, Uint8List>{};

  @override
  Future<bool> initialize({
    Directory projectDirectory,
    String testOutputDir,
    List<String> testFiles,
    BuildMode mode,
    String projectName,
    bool initializePlatform,
    PackageConfig packageConfig,
    BuildInfo buildInfo,
  }) async {
    final Directory outputTestDirectory = projectDirectory
      .childDirectory('.dart_tool')
      .childDirectory('test_temp')
        ..createSync(recursive: true);
    final FlutterProject flutterProject = FlutterProject.current();
    final ResidentCompiler testCompiler = createTestCompiler(buildInfo);

    // To keep compilation fast, a single compiler process is used. For each
    // processed test file, the magic test file is updated to point at it. This
    // is recompiled, producing a delta of new files to save to the final output
    // directory.
    final File magicTestFile = outputTestDirectory.childFile('browser_test.dart');
    final Uri magicTestUri = Uri.parse('org-dartlang-app:///browser_test.dart');
    testCompiler.addFileSystemRoot(outputTestDirectory.path);
    testCompiler.addFileSystemRoot(projectDirectory.path);
    for (final String testFile in testFiles) {
      magicTestFile.writeAsStringSync(
        _dartTestMain(
          'org-dartlang-app:///' + _fileSystem.path.split(_fileSystem.path.relative(
            testFile, from: projectDirectory.path,
          )).join('/'),
          determineLanguageVersion(
            _fileSystem.file(testFile),
            packageConfig[flutterProject?.manifest?.appName],
          ),
        ),
      );
      final CompilerOutput compilerOutput = await testCompiler.recompile(
        magicTestUri,
        <Uri>[magicTestUri],
        outputPath: outputTestDirectory.childFile('app.dill').path,
        packageConfig: packageConfig,
      );
      if (compilerOutput == null || compilerOutput.errorCount > 0) {
        return false;
      }

      final File codeFile = outputTestDirectory
        .childFile('${compilerOutput.outputFilename}.sources');
      final File manifestFile = outputTestDirectory
        .childFile('${compilerOutput.outputFilename}.json');
      final File sourcemapFile = outputTestDirectory
        .childFile('${compilerOutput.outputFilename}.map');
      final File metadataFile = outputTestDirectory
        .childFile('${compilerOutput.outputFilename}.metadata');
      _writeManifestFiles(codeFile, manifestFile, sourcemapFile, metadataFile);
      for (final String file in _files.keys) {
        if (file.startsWith('packages/')) {
          final String sourcePath = _fileSystem.path
            .joinAll(<String>[testOutputDir, ...file.split('/')])
            .replaceFirst('.dart.lib.js', '.dart.js');
          _fileSystem
            .file(sourcePath)
            ..createSync(recursive: true)
            ..writeAsBytesSync(_files[file]);
        } else if (file == 'browser_test.dart.lib.js') {
          final String directory = _fileSystem.path.dirname(_fileSystem.path.relative(
            testFile, from: projectDirectory.childDirectory('test').path));
          final String name = _fileSystem.path.basename(testFile) + '.bootstrap.dart.js';
          final String launcherName = _fileSystem.path.basename(testFile) + '.browser_test.launcher.dart.js';
          final String content = utf8.decode(_files[file])
            .replaceAll('browser_test.dart.lib.js', name);
          _fileSystem
            .file(_fileSystem.path.join(
              testOutputDir,
              directory,
              name,
            ))
            ..createSync(recursive: true)
            ..writeAsStringSync(content);
          _fileSystem
            .file(_fileSystem.path.join(
              testOutputDir,

              directory,
              launcherName,
            ))
            .writeAsStringSync(generateMainModule(
              entrypoint: name,
              nullAssertions: false,
              selfRun: true,
            ));
          _fileSystem
            .file(_fileSystem.path.join(
              testOutputDir,
              directory,
              _fileSystem.path.basename(testFile) + '.browser_test.dart.js',
            ))
            .writeAsStringSync( generateBootstrapScript(
              requireUrl: 'require.js',
              mapperUrl: 'stack_trace_mapper.js',
              boostrapUrl: launcherName,
            ));
        } else {
          final String fileName = _fileSystem.path.joinAll(<String>[
            testOutputDir,
            ...file.split('/'),
          ]);
          final String newBaseName = fileName.replaceFirst('.dart.lib.js', '.dart.js');
          final File output = _fileSystem
            .file(newBaseName)
            ..createSync(recursive: true)
            ..writeAsBytesSync(_files[file]);
          print('WROTE ${output.path} from $file');
        }
      }
      _files.clear();
      _sourcemaps.clear();
    }
    return true;
  }

  /// Update the in-memory asset server with the provided source and manifest files.
  ///
  /// Returns a list of updated modules.
  void _writeManifestFiles(
    File codeFile,
    File manifestFile,
    File sourcemapFile,
    File metadataFile,
  ) {
    final Uint8List codeBytes = codeFile.readAsBytesSync();
    final Uint8List sourcemapBytes = sourcemapFile.readAsBytesSync();
    final Map<String, dynamic> manifest = castStringKeyedMap(json.decode(manifestFile.readAsStringSync()));
    for (final String filePath in manifest.keys) {
      if (filePath == null) {
        continue;
      }
      final Map<String, dynamic> offsets = castStringKeyedMap(manifest[filePath]);
      final List<int> codeOffsets = (offsets['code'] as List<dynamic>).cast<int>();
      final List<int> sourcemapOffsets = (offsets['sourcemap'] as List<dynamic>).cast<int>();
      if (codeOffsets.length != 2 || sourcemapOffsets.length != 2) {
        continue;
      }

      final int codeStart = codeOffsets[0];
      final int codeEnd = codeOffsets[1];
      if (codeStart < 0 || codeEnd > codeBytes.lengthInBytes) {
        continue;
      }
      final Uint8List byteView = Uint8List.view(
        codeBytes.buffer,
        codeStart,
        codeEnd - codeStart,
      );
      final String fileName = filePath.startsWith('/')
        ? filePath.substring(1)
        : filePath;
      _files[fileName] = byteView;

      final int sourcemapStart = sourcemapOffsets[0];
      final int sourcemapEnd = sourcemapOffsets[1];
      if (sourcemapStart < 0 || sourcemapEnd > sourcemapBytes.lengthInBytes) {
        continue;
      }
      final Uint8List sourcemapView = Uint8List.view(
        sourcemapBytes.buffer,
        sourcemapStart,
        sourcemapEnd - sourcemapStart,
      );
      _sourcemaps[fileName] = sourcemapView;
    }
  }

  ResidentCompiler createTestCompiler(BuildInfo buildInfo) {
    Artifact platformDillArtifact;
    List<String> extraFrontEndOptions;
    if (buildInfo.nullSafetyMode == NullSafetyMode.unsound) {
      platformDillArtifact = Artifact.webPlatformKernelDill;
      extraFrontEndOptions = buildInfo.extraFrontEndOptions;
    } else {
      platformDillArtifact = Artifact.webPlatformSoundKernelDill;
      extraFrontEndOptions = <String>[...?buildInfo?.extraFrontEndOptions];
      if (!extraFrontEndOptions.contains('--no-sound-null-safety') &&
          !extraFrontEndOptions.contains('--sound-null-safety')) {
        extraFrontEndOptions.add('--sound-null-safety');
      }
    }
    return ResidentCompiler(
      globals.artifacts.getArtifactPath(Artifact.flutterWebSdk, mode: buildInfo.mode),
      buildMode: buildInfo.mode,
      trackWidgetCreation: buildInfo.trackWidgetCreation,
      fileSystemRoots: <String>[],
      // Override the filesystem scheme so that the frontend_server can find
      // the generated entrypoint code.
      fileSystemScheme: 'org-dartlang-app',
      initializeFromDill: getDefaultCachedKernelPath(
        trackWidgetCreation: buildInfo.trackWidgetCreation,
        dartDefines: buildInfo.dartDefines,
        extraFrontEndOptions: extraFrontEndOptions,
        test: false,
      ),
      targetModel: TargetModel.dartdevc,
      extraFrontEndOptions: extraFrontEndOptions,
      platformDill: _fileSystem.file(globals.artifacts
        .getArtifactPath(platformDillArtifact, mode: buildInfo.mode))
        .absolute.uri.toString(),
      dartDefines: buildInfo.dartDefines,
      librariesSpec: _fileSystem.file(globals.artifacts
        .getArtifactPath(Artifact.flutterWebLibrariesJson)).uri.toString(),
      packagesPath: buildInfo.packagesPath,
      artifacts: globals.artifacts,
      processManager: globals.processManager,
      logger: globals.logger,
      platform: globals.platform,
    );
  }

  String _dartTestMain(String path, String languageVersion) {
    return '''
$languageVersion
import 'dart:ui' as ui;
import 'dart:html';
import 'dart:js';

import 'package:stream_channel/stream_channel.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:test_api/src/backend/stack_trace_formatter.dart'; // ignore: implementation_imports
import 'package:test_api/src/remote_listener.dart'; // ignore: implementation_imports
import 'package:test_api/src/suite_channel_manager.dart'; // ignore: implementation_imports

import "$path" as test;

Future<void> main() async {
  // Extra initialization for flutter_web.
  // The following parameters are hard-coded in Flutter's test embedder. Since
  // we don't have an embedder yet this is the lowest-most layer we can put
  // this stuff in.
  ui.debugEmulateFlutterTesterEnvironment = true;
  await ui.webOnlyInitializePlatform();
  (ui.window as dynamic).debugOverrideDevicePixelRatio(3.0);
  (ui.window as dynamic).webOnlyDebugPhysicalSizeOverride = const ui.Size(2400, 1800);
  internalBootstrapBrowserTest(() => test.main);
}

void internalBootstrapBrowserTest(Function getMain()) {
  var channel = serializeSuite(getMain, hidePrints: false);
  postMessageChannel().pipe(channel);
}
StreamChannel serializeSuite(Function getMain(),
        {bool hidePrints = true, Future beforeLoad()}) =>
    RemoteListener.start(getMain,
        hidePrints: hidePrints, beforeLoad: beforeLoad);

StreamChannel suiteChannel(String name) {
  var manager = SuiteChannelManager.current;
  if (manager == null) {
    throw StateError('suiteChannel() may only be called within a test worker.');
  }

  return manager.connectOut(name);
}

StreamChannel postMessageChannel() {
  var controller = StreamChannelController(sync: true);
  window.onMessage.firstWhere((message) {
    return message.origin == window.location.origin && message.data == "port";
  }).then((message) {
    var port = message.ports.first;
    var portSubscription = port.onMessage.listen((message) {
      controller.local.sink.add(message.data);
    });

    controller.local.stream.listen((data) {
      port.postMessage({"data": data});
    }, onDone: () {
      port.postMessage({"event": "done"});
      portSubscription.cancel();
    });
  });

  context['parent'].callMethod('postMessage', [
    JsObject.jsify({"href": window.location.href, "ready": true}),
    window.location.origin,
  ]);
  return controller.foreign;
}
''';
  }
}
