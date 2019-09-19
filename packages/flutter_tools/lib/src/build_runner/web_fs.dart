// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'dart:typed_data';

import 'package:build_daemon/data/build_status.dart';
import 'package:dwds/dwds.dart';
import 'package:flutter_tools/src/compile.dart';
import 'package:flutter_tools/src/resident_runner.dart';
import 'package:flutter_tools/src/run_hot.dart';
import 'package:flutter_tools/src/web/chrome.dart';
import 'package:http_multi_server/http_multi_server.dart';
import 'package:meta/meta.dart';
import 'package:pedantic/pedantic.dart';
import 'package:shelf/shelf.dart';
import 'package:shelf/shelf_io.dart' as shelf_io;
import 'package:webkit_inspection_protocol/webkit_inspection_protocol.dart';

import '../artifacts.dart';
import '../asset.dart';
import '../base/context.dart';
import '../base/file_system.dart';
import '../base/io.dart';
import '../base/os.dart';
import '../base/platform.dart';
import '../build_info.dart';
import '../bundle.dart';
import '../convert.dart';
import '../dart/package_map.dart';
import '../globals.dart';
import '../project.dart';

/// The name of the built web project.
const String kBuildTargetName = 'web';

/// A factory for creating a [Dwds] instance.
DwdsFactory get dwdsFactory => context.get<DwdsFactory>() ?? Dwds.start;

/// A factory for creating a [WebFs] instance.
WebFsFactory get webFsFactory => context.get<WebFsFactory>() ?? WebFs.start;

/// A factory for creating an [HttpMultiServer] instance.
HttpMultiServerFactory get httpMultiServerFactory => context.get<HttpMultiServerFactory>() ?? HttpMultiServer.bind;

/// A function with the same signature as [HttpMultiServer.bind].
typedef HttpMultiServerFactory = Future<HttpServer> Function(dynamic address, int port);

/// A function with the same signature as [Dwds.start].
typedef DwdsFactory = Future<Dwds> Function({
  @required int applicationPort,
  @required int assetServerPort,
  @required String applicationTarget,
  @required Stream<BuildResult> buildResults,
  @required ConnectionProvider chromeConnection,
  String hostname,
  ReloadConfiguration reloadConfiguration,
  bool serveDevTools,
  LogWriter logWriter,
  bool verbose,
  bool enableDebugExtension,
});

/// A function with the same signatuure as [WebFs.start].
typedef WebFsFactory = Future<WebFs> Function({
  @required String target,
  @required FlutterProject flutterProject,
  @required BuildInfo buildInfo,
  @required bool skipDwds,
  @required bool initializePlatform,
  @required String hostname,
  @required String port,
});

/// The dev filesystem responsible for building and serving  web applications.
class WebFs {
  @visibleForTesting
  WebFs(
    this._server,
    this._generator,
    this._mainUri,
    this._filesystem,
    this.uri,
  ) {
    unawaited(ChromeLauncher.connectedInstance.then((Chrome chrome) async {
      final ChromeTab chromeTab = await chrome.chromeConnection.getTab((ChromeTab chromeTab) {
        return chromeTab.url.contains('localhost');
      });
      _debugger = WipDebugger(await chromeTab.connect());
      print('CONNECTED');
    }));
  }

  /// The server uri.
  final String uri;

  final String _mainUri;
  final HttpServer _server;
  final ResidentCompiler _generator;
  final Map<String, Uint8List> _filesystem;
  StreamSubscription<void> _connectedApps;
  List<Uri> _sourcesToMonitor = <Uri>[];
  DateTime lastCompiled;

  static const String _kHostName = 'localhost';

  Future<void> stop() async {
    await _server.close(force: true);
    await _connectedApps?.cancel();
    await _generator.shutdown();
  }

  WipDebugger _debugger;

  /// Recompile the web application and return whether this was successful.
  Future<bool> recompile() async {
    Stopwatch sw = Stopwatch()..start();
    final List<Uri> invalidated = ProjectFileInvalidator.findInvalidated(
      packagesPath: PackageMap.globalPackagesPath,
      lastCompiled: lastCompiled,
      urisToMonitor: _sourcesToMonitor,
    );
    final CompilerOutput compilerOutput = await _generator.recompile(
      _mainUri,
      invalidated,
      outputPath: 'build/app.dill',
    );
    if (compilerOutput.errorCount > 0) {
      await _generator.reject();
      return false;
    }
    _sourcesToMonitor = compilerOutput.sources;
    lastCompiled = DateTime.now();
    print('${sw.elapsedMilliseconds}');
    try {
      // final Map<String, String> modules = json
      final Map<String, Object> fileIndex = json.decode(fs.file('build/app.dill.incremental.dill.json').readAsStringSync());
      final Uint8List sourcesBuffer = fs.file('build/app.dill.incremental.dill.sources').readAsBytesSync();
      for (String filename in fileIndex.keys) {
        final List<Object> indexes = fileIndex[filename];
        final int start = indexes[0];
        final int end = indexes[1];
        if (end > sourcesBuffer.lengthInBytes) {
          printError('Warning: $filename out of bounds');
          continue;
        }
        _filesystem[filename + '.js'] = Uint8List.view(sourcesBuffer.buffer, start, end - start - 1);
      }
      final String command = '\$reload([${fileIndex.keys.map((x) => '"$x"').join(',')}])';
      await _debugger.sendCommand('Runtime.evaluate', params: <String, Object>{
        'expression': command,
        'awaitPromise': true,
      });
    } on FileSystemException catch (err) {
      printError(err.toString());
    }
    _generator.accept();
    return true;
  }

  /// Start the web compiler and asset server.
  static Future<WebFs> start({
    @required String target,
    @required FlutterProject flutterProject,
    @required BuildInfo buildInfo,
    @required String hostname,
    @required String port,
    @required FlutterDevice device,
  }) async {
    await device.generator.recompile(target, <Uri>[], outputPath: 'build/app.dill', packagesFilePath: PackageMap.globalPackagesPath);
    device.generator.accept();

    final Map<String, Object> fileIndex = json.decode(fs.file('build/app.dill.json').readAsStringSync());
    final Uint8List sourcesBuffer = fs.file('build/app.dill.sources').readAsBytesSync();
    final Map<String, Uint8List> filesystem = <String, Uint8List>{};
    for (String filename in fileIndex.keys) {
      final List<Object> indexes = fileIndex[filename];
      final int start = indexes[0];
      final int end = indexes[1];
      if (end > sourcesBuffer.lengthInBytes) {
        printError('Warning: $filename out of bounds');
        continue;
      }
      filesystem[filename + '.js'] = Uint8List.view(sourcesBuffer.buffer, start, end - start);
    }

    // Initialize the asset bundle.
    final AssetBundle assetBundle = AssetBundleFactory.instance.createBundle();
    await assetBundle.build();
    await writeBundle(fs.directory(getAssetBuildDirectory()), assetBundle.entries);

    // Initialize the dwds server.
    final int hostPort = port == null ? await os.findFreePort() : int.tryParse(port);

    // Map the bootstrap files to the correct package directory.
    Cascade cascade = Cascade();
    cascade = cascade.add(_assetHandler(flutterProject, filesystem, target));
    final HttpServer server = await httpMultiServerFactory(hostname ?? _kHostName, hostPort);
    shelf_io.serveRequests(server, cascade.handler);

    return WebFs(
      server,
      device.generator,
      target,
      filesystem,
      'http://$_kHostName:$hostPort/',
    );
  }

  static Future<Response> Function(Request request) _assetHandler(FlutterProject flutterProject, Map<String, Uint8List> filesystem, String target) {
    final PackageMap packageMap = PackageMap(PackageMap.globalPackagesPath);
    return (Request request) async {
      final String modulePath = '/' + request.url.path + (request.url.path.endsWith('.js') ? '' : '.js');
      if (filesystem.containsKey(modulePath)) {
        return Response.ok(filesystem[modulePath], headers: <String, String>{
          'Content-Type': 'text/javascript',
        });
      }
      if (request.url.path == 'main.dart.js') {
        return Response.ok(utf8.encode(_appBootstrap(
          fs.file(target).absolute.path,
        )), headers: <String, String>{
          'Content-Type': 'text/javascript',
        });
      } else if (request.url.path == 'index.html' || request.url.path.isEmpty) {
        final File file = flutterProject.web.indexFile;
        return Response.ok(file.readAsBytesSync(), headers: <String, String>{
          'Content-Type': 'text/html',
        });
      } else if (request.url.path.contains('stack_trace_mapper')) {
        final File file = fs.file(fs.path.join(
          artifacts.getArtifactPath(Artifact.engineDartSdkPath),
          'lib',
          'dev_compiler',
          'web',
          'dart_stack_trace_mapper.js'
        ));
        return Response.ok(file.readAsBytesSync(), headers: <String, String>{
          'Content-Type': 'text/javascript',
        });
      } else if (request.url.path.contains('require.js')) {
        final File file = fs.file(fs.path.join(
          artifacts.getArtifactPath(Artifact.engineDartSdkPath),
          'lib',
          'dev_compiler',
          'kernel',
          'amd',
          'require.js'
        ));
        return Response.ok(file.readAsBytesSync(), headers: <String, String>{
          'Content-Type': 'text/javascript',
        });
      } else if (request.url.path.endsWith('dart_sdk.js')) {
        final File file = fs.file(fs.path.join(
          artifacts.getArtifactPath(Artifact.flutterWebSdk),
          'kernel',
          'amd',
          'dart_sdk.js',
        ));
        return Response.ok(file.readAsBytesSync(), headers: <String, String>{
          'Content-Type': 'text/javascript',
        });
      } else if (request.url.path.endsWith('dart_sdk.js.map')) {
        final File file = fs.file(fs.path.join(
          artifacts.getArtifactPath(Artifact.flutterWebSdk),
          'kernel',
          'amd',
          'dart_sdk.js.map',
        ));
        return Response.ok(file.readAsBytesSync());
      } else if (request.url.path.endsWith('.dart')) {
        // This is likely a sourcemap request. The first segment is the
        // package name, and the rest is the path to the file relative to
        // the package uri. For example, `foo/bar.dart` would represent a
        // file at a path like `foo/lib/bar.dart`. If there is no leading
        // segment, then we assume it is from the current package.

        // Handle sdk requests that have mangled urls from engine build.
        if (request.url.path.contains('flutter_web_sdk')) {
          // Note: the request is a uri and not a file path, so they always use `/`.
          final String sdkPath = fs.path.joinAll(request.url.path.split('flutter_web_sdk/').last.split('/'));
          final String webSdkPath = artifacts.getArtifactPath(Artifact.flutterWebSdk);
          return Response.ok(fs.file(fs.path.join(webSdkPath, sdkPath)).readAsBytesSync());
        }

        final String packageName = request.url.pathSegments.length == 1
          ? flutterProject.manifest.appName
          : request.url.pathSegments.first;
        String filePath = fs.path.joinAll(request.url.pathSegments.length == 1
          ? request.url.pathSegments
          : request.url.pathSegments.skip(1));
        String packagePath = packageMap.map[packageName]?.toFilePath(windows: platform.isWindows);
        // If the package isn't found, then we have an issue with relative
        // paths within the main project.
        if (packagePath == null) {
          packagePath = packageMap.map[flutterProject.manifest.appName]
            .toFilePath(windows: platform.isWindows);
          filePath = request.url.path;
        }
        final File file = fs.file(fs.path.join(packagePath, filePath));
        if (file.existsSync()) {
          return Response.ok(file.readAsBytesSync());
        }
        return Response.notFound('');
      } else if (request.url.path.contains('assets')) {
        final String assetPath = request.url.path.replaceFirst('assets/', '');
        final File file = fs.file(fs.path.join(getAssetBuildDirectory(), assetPath));
        if (file.existsSync()) {
          return Response.ok(file.readAsBytesSync());
        } else {
          return Response.notFound('');
        }
      }
      return Response.notFound('');
    };
  }
}

String _appBootstrap(String moduleName) =>
    '''
define("main.dart.js", ["$moduleName", "dart_sdk"], function(app, dart_sdk) {
  window.\$appMain = app.main.main;
  if (window.\$afterReload == null) {
    window.\$afterReload = function(resolve) {
      dart_sdk.developer._extensions._get('ext.flutter.disassemble')({}, {});
      dart_sdk.dart.hotRestart();
      window.\$appMain();
      resolve();
    }
    app.main.main();
  }
});
require.config({
  waitSeconds: 0,
});
if (window.\$reload == null) {
  window.\$reload = function(modules) {
    var promise = new Promise(function(resolve, reject) {
      if (modules == null || modules.length == 0) {
        window.\$afterReload(resolve);
        return;
      }
      var loaded = 0;
      for (var i = 0; i < modules.length; i++) {
        var moduleName = modules[i];
        requirejs.undef(moduleName);
        requirejs([moduleName], function() {
          loaded += 1;
          if (loaded == modules.length) {
            requirejs.undef("main.dart.js");
            requirejs(["main.dart.js"], function() {
              window.\$afterReload(resolve);
            });
          }
        });
      }
    });
    return promise;
  }
}
requirejs(["main.dart.js"]);
''';