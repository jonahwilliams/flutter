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
import 'package:http_multi_server/http_multi_server.dart';
import 'package:meta/meta.dart';
import 'package:shelf/shelf.dart';
import 'package:shelf/shelf_io.dart' as shelf_io;

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
  );

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

  /// Recompile the web application and return whether this was successful.
  Future<bool> recompile() async {
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
    final Map<String, Object> fileIndex = json.decode(fs.file('build/app.incremental.dill.json').readAsStringSync());
    final Uint8List sourcesBuffer = fs.file('build/app.dill.incremental.sources').readAsBytesSync();
    for (String filename in fileIndex.keys) {
      final List<Object> indexes = fileIndex[filename];
      final int start = indexes[0];
      final int end = indexes[1];
      _filesystem[filename] = Uint8List.view(sourcesBuffer.buffer, start, end - start);
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
      filesystem[filename] = Uint8List.view(sourcesBuffer.buffer, start, end - start);
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

  static String _getEntrypoint(FlutterProject flutterProject, String target) {
return r'''
/* ENTRYPOINT_EXTENTION_MARKER */
(function() {
var _currentDirectory = (function () {
  var _url;
  var lines = new Error().stack.split('\n');
  function lookupUrl() {
    if (lines.length > 2) {
      var match = lines[1].match(/^\s+at (.+):\d+:\d+$/);
      // Chrome.
      if (match) return match[1];
      // Chrome nested eval case.
      match = lines[1].match(/^\s+at eval [(](.+):\d+:\d+[)]$/);
      if (match) return match[1];
      // Edge.
      match = lines[1].match(/^\s+at.+\((.+):\d+:\d+\)$/);
      if (match) return match[1];
      // Firefox.
      match = lines[0].match(/[<][@](.+):\d+:\d+$/)
      if (match) return match[1];
    }
    // Safari.
    return lines[0].match(/(.+):\d+:\d+$/)[1];
  }
  _url = lookupUrl();
  var lastSlash = _url.lastIndexOf('/');
  if (lastSlash == -1) return _url;
  var currentDirectory = _url.substring(0, lastSlash + 1);
  return currentDirectory;
})();
'''
'''
var baseUrl = (function () {
  // Attempt to detect --precompiled mode for tests, and set the base url
  // appropriately, otherwise set it to '/'.
  var pathParts = location.pathname.split("/");
  if (pathParts[0] == "") {
    pathParts.shift();
  }
  if (pathParts.length > 1 && pathParts[1] == "test") {
    return "/" + pathParts.slice(0, 2).join("/") + "/";
  }
  // Attempt to detect base url using <base href> html tag
  // base href should start and end with "/"
  if (typeof document !== 'undefined') {
    var el = document.getElementsByTagName('base');
    if (el && el[0] && el[0].getAttribute("href") && el[0].getAttribute
    ("href").startsWith("/") && el[0].getAttribute("href").endsWith("/")){
      return el[0].getAttribute("href");
    }
  }
  // return default value
  return "/";
}());

let modulePaths = {};
if(!window.\$dartLoader) {
   window.\$dartLoader = {
     appDigests: _currentDirectory + 'main_web_entrypoint.digests',
     moduleIdToUrl: new Map(),
     urlToModuleId: new Map(),
     rootDirectories: new Array(),
     // Used in package:build_runner/src/server/build_updates_client/hot_reload_client.dart
     moduleParentsGraph: new Map(),
     moduleLoadingErrorCallbacks: new Map(),
     forceLoadModule: function (moduleName, callback, onError) {
       // dartdevc only strips the final extension when adding modules to source
       // maps, so we need to do the same.
       if (moduleName.endsWith('.ddc')) {
         moduleName = moduleName.substring(0, moduleName.length - 4);
       }
       if (typeof onError != 'undefined') {
         var errorCallbacks = \$dartLoader.moduleLoadingErrorCallbacks;
         if (!errorCallbacks.has(moduleName)) {
           errorCallbacks.set(moduleName, new Set());
         }
         errorCallbacks.get(moduleName).add(onError);
       }
       requirejs.undef(moduleName);
       requirejs([moduleName], function() {
         if (typeof onError != 'undefined') {
           errorCallbacks.get(moduleName).delete(onError);
         }
         if (typeof callback != 'undefined') {
           callback();
         }
       });
     },
     getModuleLibraries: null, // set up by _initializeTools
   };
}
let customModulePaths = {};
window.\$dartLoader.rootDirectories.push(window.location.origin + baseUrl);
for (let moduleName of Object.getOwnPropertyNames(modulePaths)) {
  let modulePath = modulePaths[moduleName];
  if (modulePath != moduleName) {
    customModulePaths[moduleName] = modulePath;
  }
  var src = window.location.origin + '/' + modulePath + '.js';
  if (window.\$dartLoader.moduleIdToUrl.has(moduleName)) {
    continue;
  }
  \$dartLoader.moduleIdToUrl.set(moduleName, src);
  \$dartLoader.urlToModuleId.set(src, moduleName);
}
// Whenever we fail to load a JS module, try to request the corresponding
// `.errors` file, and log it to the console.
(function() {
  var oldOnError = requirejs.onError;
  requirejs.onError = function(e) {
    if (e.requireModules) {
      if (e.message) {
        // If error occurred on loading dependencies, we need to invalidate ancessor too.
        var ancesor = e.message.match(/needed by: (.*)/);
        if (ancesor) {
          e.requireModules.push(ancesor[1]);
        }
      }
      for (const module of e.requireModules) {
        var errorCallbacks = \$dartLoader.moduleLoadingErrorCallbacks.get(module);
        if (errorCallbacks) {
          for (const callback of errorCallbacks) callback(e);
          errorCallbacks.clear();
        }
      }
    }
    if (e.originalError && e.originalError.srcElement) {
      var xhr = new XMLHttpRequest();
      xhr.onreadystatechange = function() {
        if (this.readyState == 4) {
          var message;
          if (this.status == 200) {
            message = this.responseText;
          } else {
            message = "Unknown error loading " + e.originalError.srcElement.src;
          }
          console.error(message);
          var errorEvent = new CustomEvent(
            'dartLoadException', { detail: message });
          window.dispatchEvent(errorEvent);
        }
      };
      xhr.open("GET", e.originalError.srcElement.src + ".errors", true);
      xhr.send();
    }
    // Also handle errors the normal way.
    if (oldOnError) oldOnError(e);
  };
}());

var baseUrl = (function () {
  // Attempt to detect --precompiled mode for tests, and set the base url
  // appropriately, otherwise set it to '/'.
  var pathParts = location.pathname.split("/");
  if (pathParts[0] == "") {
    pathParts.shift();
  }
  if (pathParts.length > 1 && pathParts[1] == "test") {
    return "/" + pathParts.slice(0, 2).join("/") + "/";
  }
  // Attempt to detect base url using <base href> html tag
  // base href should start and end with "/"
  if (typeof document !== 'undefined') {
    var el = document.getElementsByTagName('base');
    if (el && el[0] && el[0].getAttribute("href") && el[0].getAttribute
    ("href").startsWith("/") && el[0].getAttribute("href").endsWith("/")){
      return el[0].getAttribute("href");
    }
  }
  // return default value
  return "/";
}());
;

require.config({
    baseUrl: baseUrl,
    waitSeconds: 0,
    paths: customModulePaths
});

const modulesGraph = new Map();
function getRegisteredModuleName(moduleMap) {
  if (\$dartLoader.moduleIdToUrl.has(moduleMap.name + '.ddc')) {
    return moduleMap.name + '.ddc';
  }
  return moduleMap.name;
}
requirejs.onResourceLoad = function (context, map, depArray) {
  const name = getRegisteredModuleName(map);
  const depNameArray = depArray.map(getRegisteredModuleName);
  if (modulesGraph.has(name)) {
    // TODO Move this logic to better place
    var previousDeps = modulesGraph.get(name);
    var changed = previousDeps.length != depNameArray.length;
    changed = changed || depNameArray.some(function(depName) {
      return !previousDeps.includes(depName);
    });
    if (changed) {
      console.warn("Dependencies graph change for module '" + name + "' detected. " +
        "Dependencies was [" + previousDeps + "], now [" +  depNameArray.map((depName) => depName) +"]. " +
        "Page can't be hot-reloaded, firing full page reload.");
      window.location.reload();
    }
  } else {
    modulesGraph.set(name, []);
    for (const depName of depNameArray) {
      if (!\$dartLoader.moduleParentsGraph.has(depName)) {
        \$dartLoader.moduleParentsGraph.set(depName, []);
      }
      \$dartLoader.moduleParentsGraph.get(depName).push(name);
      modulesGraph.get(name).push(depName);
    }
  }
};
define("main_web_entrypoint.dart.bootstrap", ["${fs.path.absolute(target)}", "dart_sdk"], function(app, dart_sdk) {
  dart_sdk.dart.setStartAsyncSynchronously(true);
  dart_sdk._isolate_helper.startRootIsolate(() => {}, []);
  var baseUrl = (function () {
  // Attempt to detect --precompiled mode for tests, and set the base url
  // appropriately, otherwise set it to '/'.
  var pathParts = location.pathname.split("/");
  if (pathParts[0] == "") {
    pathParts.shift();
  }
  if (pathParts.length > 1 && pathParts[1] == "test") {
    return "/" + pathParts.slice(0, 2).join("/") + "/";
  }
  // Attempt to detect base url using <base href> html tag
  // base href should start and end with "/"
  if (typeof document !== 'undefined') {
    var el = document.getElementsByTagName('base');
    if (el && el[0] && el[0].getAttribute("href") && el[0].getAttribute
    ("href").startsWith("/") && el[0].getAttribute("href").endsWith("/")){
      return el[0].getAttribute("href");
    }
  }
  // return default value
  return "/";
}());

  dart_sdk._debugger.registerDevtoolsFormatter();
  \$dartLoader.getModuleLibraries = dart_sdk.dart.getModuleLibraries;
  if (window.\$dartStackTraceUtility && !window.\$dartStackTraceUtility.ready) {
    window.\$dartStackTraceUtility.ready = true;
    let dart = dart_sdk.dart;
    window.\$dartStackTraceUtility.setSourceMapProvider(
      function(url) {
        url = url.replace(baseUrl, '/');
        var module = window.\$dartLoader.urlToModuleId.get(url);
        if (!module) return null;
        return dart.getSourceMap(module);
      });
  }
  if (typeof document != 'undefined') {
    window.postMessage({ type: "DDC_STATE_CHANGE", state: "start" }, "*");
  }

  /* MAIN_EXTENSION_MARKER */
  (app.lib__main_web_entrypoint || app.main_web_entrypoint).main();
  var bootstrap = {
      hot\$onChildUpdate: function(childName, child) {
        // Special handling for the multi-root scheme uris. We need to strip
        // out the scheme and the top level directory, to match the source path
        // that chrome sees.
        if (childName.startsWith('org-dartlang-app:///')) {
          childName = childName.substring('org-dartlang-app:///'.length);
          var firstSlash = childName.indexOf('/');
          if (firstSlash == -1) return false;
          childName = childName.substring(firstSlash + 1);
        }
        if (childName === "package:hello_world/main_web_entrypoint.dart") {
          // Clear static caches.
          dart_sdk.dart.hotRestart();
          child.main();
          return true;
        }
      }
    }
  dart_sdk.dart.trackLibraries("main_web_entrypoint.dart.bootstrap", {
    "main_web_entrypoint.dart.bootstrap": bootstrap
  }, '');
  return {
    bootstrap: bootstrap
  };
});
})();
''';
  }

  static Future<Response> Function(Request request) _assetHandler(FlutterProject flutterProject, Map<String, Uint8List> filesystem, String target) {
    final PackageMap packageMap = PackageMap(PackageMap.globalPackagesPath);
    return (Request request) async {
      print('Requested: ${request.url.path}');
      final String path = request.url.path;
      if (filesystem.containsKey(path)) {
        return Response.ok(filesystem[path], headers: <String, String>{
          'Content-Type': 'text/javascript',
        });
      } else if (request.url.path == 'main.dart.js') {
        return Response.ok(utf8.encode(_getEntrypoint(flutterProject, target)), headers: <String, String>{
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

