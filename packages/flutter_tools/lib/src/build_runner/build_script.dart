// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ignore_for_file: implementation_imports
import 'dart:async';
import 'dart:collection';
import 'dart:convert'; // ignore: dart_convert_import
import 'dart:io'; // ignore: dart_io_import
import 'dart:isolate';

import 'package:analyzer/dart/analysis/results.dart';
import 'package:analyzer/dart/analysis/utilities.dart';
import 'package:analyzer/dart/ast/ast.dart';
import 'package:build/build.dart';
import 'package:build_config/build_config.dart';
import 'package:build_modules/build_modules.dart';
import 'package:build_modules/builders.dart';
import 'package:build_modules/src/module_builder.dart';
import 'package:build_modules/src/module_library.dart';
import 'package:build_modules/src/platform.dart';
import 'package:build_modules/src/workers.dart';
import 'package:build_runner/build_runner.dart' as build_runner;
import 'package:build_runner_core/build_runner_core.dart' as core;
import 'package:build_test/builder.dart';
import 'package:build_test/src/debug_test_builder.dart';
import 'package:build_web_compilers/build_web_compilers.dart';
import 'package:build_web_compilers/builders.dart';
import 'package:build_web_compilers/src/ddc_names.dart';
import 'package:build_web_compilers/src/dev_compiler_builder.dart';
import 'package:build_web_compilers/src/platforms.dart';
import 'package:crypto/crypto.dart';
import 'package:path/path.dart' as path; // ignore: package_path_import
import 'package:pool/pool.dart';
import 'package:scratch_space/scratch_space.dart';
import 'package:test_core/backend.dart';

const String ddcBootstrapExtension = '.dart.bootstrap.js';
const String jsEntrypointExtension = '.dart.js';
const String jsEntrypointSourceMapExtension = '.dart.js.map';
const String jsEntrypointArchiveExtension = '.dart.js.tar.gz';
const String digestsEntrypointExtension = '.digests';
const String jsModuleErrorsExtension = '.ddc.js.errors';
const String jsModuleExtension = '.ddc.js';
const String jsSourceMapExtension = '.ddc.js.map';
const String kReleaseFlag = 'release';
const String kProfileFlag = 'profile';

final DartPlatform flutterWebPlatform =
    DartPlatform.register('flutter_web', <String>[
  'async',
  'collection',
  'convert',
  'core',
  'developer',
  'html',
  'html_common',
  'indexed_db',
  'js',
  'js_util',
  'math',
  'svg',
  'typed_data',
  'web_audio',
  'web_gl',
  'web_sql',
  '_internal',
  // Flutter web specific libraries.
  'ui',
  '_engine',
]);

/// The builders required to compile a Flutter application to the web.
final List<core.BuilderApplication> builders = <core.BuilderApplication>[
  core.apply(
    'flutter_tools:test_bootstrap',
    <BuilderFactory>[
      (BuilderOptions options) => const DebugTestBuilder(),
      (BuilderOptions options) => const FlutterWebTestBootstrapBuilder(),
    ],
    core.toRoot(),
    hideOutput: true,
    defaultGenerateFor: const InputSet(
      include: <String>[
        'test/**',
      ],
    ),
  ),
  core.apply(
    'flutter_tools:shell',
    <BuilderFactory>[
      (BuilderOptions options) {
        final bool hasPlugins = options.config['hasPlugins'] == true;
        return FlutterWebShellBuilder(hasPlugins: hasPlugins);
      }
    ],
    core.toRoot(),
    hideOutput: true,
    defaultGenerateFor: const InputSet(
      include: <String>[
        'lib/**',
        'web/**',
      ],
    ),
  ),
  core.apply(
      'flutter_tools:module_library',
      <Builder Function(BuilderOptions)>[moduleLibraryBuilder],
      core.toAllPackages(),
      isOptional: true,
      hideOutput: true,
      appliesBuilders: <String>['flutter_tools:module_cleanup']),
  core.apply(
      'flutter_tools:ddc_modules',
      <Builder Function(BuilderOptions)>[
        (BuilderOptions options) => MetaModuleBuilder(flutterWebPlatform),
        (BuilderOptions options) => MetaModuleCleanBuilder(flutterWebPlatform),
        (BuilderOptions options) => ModuleBuilder(flutterWebPlatform),
      ],
      core.toNoneByDefault(),
      isOptional: true,
      hideOutput: true,
      appliesBuilders: <String>['flutter_tools:module_cleanup']),
  core.apply(
      'flutter_tools:ddc',
      <Builder Function(BuilderOptions)>[
        (BuilderOptions builderOptions) => KernelBuilder(
              platformSdk: builderOptions.config['flutterWebSdk'],
              summaryOnly: true,
              sdkKernelPath: path.join('kernel', 'flutter_ddc_sdk.dill'),
              outputExtension: ddcKernelExtension,
              platform: flutterWebPlatform,
              librariesPath: path.absolute(path.join(builderOptions.config['flutterWebSdk'], 'libraries.json')),
              kernelTargetName: 'ddc',
            ),
        (BuilderOptions builderOptions) => DevCompilerBuilder(
              useIncrementalCompiler: false,
              platform: flutterWebPlatform,
              platformSdk: builderOptions.config['flutterWebSdk'],
              sdkKernelPath: path.url.join('kernel', 'flutter_ddc_sdk.dill'),
              librariesPath: path.absolute(path.join(builderOptions.config['flutterWebSdk'], 'libraries.json')),
            ),
      ],
      core.toAllPackages(),
      isOptional: true,
      hideOutput: true,
      appliesBuilders: <String>['flutter_tools:ddc_modules']),
  core.apply(
    'flutter_tools:entrypoint',
    <BuilderFactory>[
      (BuilderOptions options) => FlutterWebEntrypointBuilder(
          options.config[kReleaseFlag] ?? false,
          options.config[kProfileFlag] ?? false,
          options.config['flutterWebSdk'],
      ),
    ],
    core.toRoot(),
    hideOutput: true,
    defaultGenerateFor: const InputSet(
      include: <String>[
        'lib/**_web_entrypoint.dart',
      ],
    ),
  ),
  core.apply(
    'flutter_tools:test_entrypoint',
    <BuilderFactory>[
      (BuilderOptions options) => const FlutterWebTestEntrypointBuilder(),
    ],
    core.toRoot(),
    hideOutput: true,
    defaultGenerateFor: const InputSet(
      include: <String>[
        'test/**_test.dart.browser_test.dart',
      ],
    ),
  ),
  core.applyPostProcess('flutter_tools:module_cleanup', moduleCleanup,
      defaultGenerateFor: const InputSet())
];

/// The entrypoint to this build script.
Future<void> main(List<String> args, [SendPort sendPort]) async {
  core.overrideGeneratedOutputDirectory('flutter_web');
  final int result = await build_runner.run(args, builders);
  sendPort?.send(result);
}

/// A ddc-only entrypoint builder that respects the Flutter target flag.
class FlutterWebTestEntrypointBuilder implements Builder {
  const FlutterWebTestEntrypointBuilder();

  @override
  Map<String, List<String>> get buildExtensions => const <String, List<String>>{
        '.dart': <String>[
          ddcBootstrapExtension,
          jsEntrypointExtension,
          jsEntrypointSourceMapExtension,
          jsEntrypointArchiveExtension,
          digestsEntrypointExtension,
        ],
      };

  @override
  Future<void> build(BuildStep buildStep) async {
    log.info('building for target ${buildStep.inputId.path}');
    await bootstrapDdc(buildStep, platform: flutterWebPlatform);
  }
}

/// A ddc-only entrypoint builder that respects the Flutter target flag.
class FlutterWebEntrypointBuilder implements Builder {
  const FlutterWebEntrypointBuilder(this.release, this.profile, this.flutterWebSdk);

  final bool release;
  final bool profile;
  final String flutterWebSdk;

  @override
  Map<String, List<String>> get buildExtensions => const <String, List<String>>{
        '.dart': <String>[
          ddcBootstrapExtension,
          jsEntrypointExtension,
          jsEntrypointSourceMapExtension,
          jsEntrypointArchiveExtension,
          digestsEntrypointExtension,
        ],
      };

  @override
  Future<void> build(BuildStep buildStep) async {
    if (release || profile) {
      await bootstrapDart2Js(buildStep, flutterWebSdk, profile);
    } else {
      await bootstrapDdc(buildStep, platform: flutterWebPlatform);
    }
  }
}

/// Bootstraps the test entrypoint.
class FlutterWebTestBootstrapBuilder implements Builder {
  const FlutterWebTestBootstrapBuilder();

  @override
  Map<String, List<String>> get buildExtensions => const <String, List<String>>{
    '_test.dart': <String>[
      '_test.dart.browser_test.dart',
    ]
  };

  @override
  Future<void> build(BuildStep buildStep) async {
    final AssetId id = buildStep.inputId;
    final String contents = await buildStep.readAsString(id);
    final String assetPath = id.pathSegments.first == 'lib'
        ? path.url.join('packages', id.package, id.path)
        : id.path;
    final Metadata metadata = parseMetadata(
        assetPath, contents, Runtime.builtIn.map((Runtime runtime) => runtime.name).toSet());

    if (metadata.testOn.evaluate(SuitePlatform(Runtime.chrome))) {
    await buildStep.writeAsString(id.addExtension('.browser_test.dart'), '''
import 'dart:ui' as ui;
import 'dart:html';
import 'dart:js';

import 'package:stream_channel/stream_channel.dart';
import 'package:test_api/src/backend/stack_trace_formatter.dart'; // ignore: implementation_imports
import 'package:test_api/src/util/stack_trace_mapper.dart'; // ignore: implementation_imports
import 'package:test_api/src/remote_listener.dart'; // ignore: implementation_imports
import 'package:test_api/src/suite_channel_manager.dart'; // ignore: implementation_imports

import "${path.url.basename(id.path)}" as test;

Future<void> main() async {
  // Extra initialization for flutter_web.
  // The following parameters are hard-coded in Flutter's test embedder. Since
  // we don't have an embedder yet this is the lowest-most layer we can put
  // this stuff in.
  await ui.webOnlyInitializeEngine();
  // TODO(flutterweb): remove need for dynamic cast.
  (ui.window as dynamic).debugOverrideDevicePixelRatio(3.0);
  (ui.window as dynamic).webOnlyDebugPhysicalSizeOverride = const ui.Size(2400, 1800);
  internalBootstrapBrowserTest(() => test.main);
}

void internalBootstrapBrowserTest(Function getMain()) {
  var channel =
      serializeSuite(getMain, hidePrints: false, beforeLoad: () async {
    var serialized =
        await suiteChannel("test.browser.mapper").stream.first as Map;
    if (serialized == null) return;
  });
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

void setStackTraceMapper(StackTraceMapper mapper) {
  var formatter = StackTraceFormatter.current;
  if (formatter == null) {
    throw StateError(
        'setStackTraceMapper() may only be called within a test worker.');
  }

  formatter.configure(mapper: mapper);
}
''');
    }
  }
}

/// A shell builder which generates the web specific entrypoint.
class FlutterWebShellBuilder implements Builder {
  const FlutterWebShellBuilder({this.hasPlugins = false});

  final bool hasPlugins;

  @override
  Future<void> build(BuildStep buildStep) async {
    final AssetId dartEntrypointId = buildStep.inputId;
    final bool isAppEntrypoint = await _isAppEntryPoint(dartEntrypointId, buildStep);
    if (!isAppEntrypoint) {
      return;
    }
    final AssetId outputId = buildStep.inputId.changeExtension('_web_entrypoint.dart');
    if (hasPlugins) {
      await buildStep.writeAsString(outputId, '''
import 'dart:ui' as ui;

import 'package:flutter_web_plugins/flutter_web_plugins.dart';

import 'generated_plugin_registrant.dart';
import "${path.url.basename(buildStep.inputId.path)}" as entrypoint;

Future<void> main() async {
  registerPlugins(webPluginRegistry);
  await ui.webOnlyInitializePlatform();
  entrypoint.main();
}
''');
    } else {
      await buildStep.writeAsString(outputId, '''
import 'dart:ui' as ui;

import "${path.url.basename(buildStep.inputId.path)}" as entrypoint;

Future<void> main() async {
  await ui.webOnlyInitializePlatform();
  entrypoint.main();
}
''');
    }
  }

  @override
  Map<String, List<String>> get buildExtensions => const <String, List<String>>{
    '.dart': <String>['_web_entrypoint.dart'],
  };
}

Future<void> bootstrapDart2Js(BuildStep buildStep, String flutterWebSdk, bool profile) async {
  final AssetId dartEntrypointId = buildStep.inputId;
  final AssetId moduleId = dartEntrypointId.changeExtension(moduleExtension(flutterWebPlatform));
  final Module module = Module.fromJson(json.decode(await buildStep.readAsString(moduleId)));

  final List<Module> allDeps = await module.computeTransitiveDependencies(buildStep, throwIfUnsupported: false)..add(module);
  final ScratchSpace scratchSpace = await buildStep.fetchResource(scratchSpaceResource);
  final Iterable<AssetId> allSrcs = allDeps.expand((Module module) => module.sources);
  await scratchSpace.ensureAssets(allSrcs, buildStep);

  final String packageFile = _createPackageFile(allSrcs, buildStep, scratchSpace);
  final String dartPath = dartEntrypointId.path.startsWith('lib/')
      ? 'package:${dartEntrypointId.package}/'
          '${dartEntrypointId.path.substring('lib/'.length)}'
      : dartEntrypointId.path;
  final String jsOutputPath =
      '${path.withoutExtension(dartPath.replaceFirst('package:', 'packages/'))}'
      '$jsEntrypointExtension';
  final String flutterWebSdkPath = flutterWebSdk;
  final String librariesPath = path.join(flutterWebSdkPath, 'libraries.json');
  final List<String> args = <String>[
    '--libraries-spec="$librariesPath"',
    if (profile)
      '-O1'
    else
      '-O4',
    '-o',
    '$jsOutputPath',
    '--packages="$packageFile"',
    if (profile)
      '-Ddart.vm.profile=true'
    else
      '-Ddart.vm.product=true',
    dartPath,
  ];
  final Dart2JsBatchWorkerPool dart2js = await buildStep.fetchResource(dart2JsWorkerResource);
  final Dart2JsResult result = await dart2js.compile(args);
  final AssetId jsOutputId = dartEntrypointId.changeExtension(jsEntrypointExtension);
  final File jsOutputFile = scratchSpace.fileFor(jsOutputId);
  if (result.succeeded && jsOutputFile.existsSync()) {
    log.info(result.output);
    // Explicitly write out the original js file and sourcemap.
    await scratchSpace.copyOutput(jsOutputId, buildStep);
    final AssetId jsSourceMapId =
        dartEntrypointId.changeExtension(jsEntrypointSourceMapExtension);
    await _copyIfExists(jsSourceMapId, scratchSpace, buildStep);
  } else {
    log.severe(result.output);
  }
}

Future<void> _copyIfExists(
    AssetId id, ScratchSpace scratchSpace, AssetWriter writer) async {
  final File file = scratchSpace.fileFor(id);
  if (file.existsSync()) {
    await scratchSpace.copyOutput(id, writer);
  }
}

/// Creates a `.packages` file unique to this entrypoint at the root of the
/// scratch space and returns it's filename.
///
/// Since mulitple invocations of Dart2Js will share a scratch space and we only
/// know the set of packages involved the current entrypoint we can't construct
/// a `.packages` file that will work for all invocations of Dart2Js so a unique
/// file is created for every entrypoint that is run.
///
/// The filename is based off the MD5 hash of the asset path so that files are
/// unique regarless of situations like `web/foo/bar.dart` vs
/// `web/foo-bar.dart`.
String _createPackageFile(Iterable<AssetId> inputSources, BuildStep buildStep, ScratchSpace scratchSpace) {
  final Uri inputUri = buildStep.inputId.uri;
  final String packageFileName =
      '.package-${md5.convert(inputUri.toString().codeUnits)}';
  final File packagesFile =
      scratchSpace.fileFor(AssetId(buildStep.inputId.package, packageFileName));
  final Set<String> packageNames = inputSources.map((AssetId s) => s.package).toSet();
  final String packagesFileContent =
      packageNames.map((String name) => '$name:packages/$name/').join('\n');
  packagesFile .writeAsStringSync('# Generated for $inputUri\n$packagesFileContent');
  return packageFileName;
}

/// Returns whether or not [dartId] is an app entrypoint (basically, whether
/// or not it has a `main` function).
Future<bool> _isAppEntryPoint(AssetId dartId, AssetReader reader) async {
  assert(dartId.extension == '.dart');
  // Skip reporting errors here, dartdevc will report them later with nicer
  // formatting.
  final ParseStringResult result = parseString(
    content: await reader.readAsString(dartId),
    throwIfDiagnostics: false,
  );
  // Allow two or fewer arguments so that entrypoints intended for use with
  // [spawnUri] get counted.
  return result.unit.declarations.any((CompilationUnitMember node) {
    return node is FunctionDeclaration &&
        node.name.name == 'main' &&
        node.functionExpression.parameters.parameters.length <= 2;
  });
}

String _modulePartialExtension = path.url.withoutExtension(jsModuleExtension);

Future<void> bootstrapDdc(BuildStep buildStep, {DartPlatform platform}) async {
  final AssetId dartEntrypointId = buildStep.inputId;
  final AssetId moduleId = buildStep.inputId
      .changeExtension(moduleExtension(platform ?? ddcPlatform));
  final Module module = Module.fromJson(json
      .decode(await buildStep.readAsString(moduleId)) as Map<String, dynamic>);

  // First, ensure all transitive modules are built.
  List<AssetId> transitiveJsModules;
  try {
    transitiveJsModules = await _ensureTransitiveJsModules(module, buildStep);
  } on UnsupportedModules catch (e) {
    final String librariesString = (await e.exactLibraries(buildStep).toList())
        .map((ModuleLibrary lib) => AssetId(lib.id.package,
            lib.id.path.replaceFirst(moduleLibraryExtension, '.dart')))
        .join('\n');
    log.warning('''
Skipping compiling ${buildStep.inputId} with ddc because some of its
transitive libraries have sdk dependencies that not supported on this platform:

$librariesString

https://github.com/dart-lang/build/blob/master/docs/faq.md#how-can-i-resolve-skipped-compiling-warnings
''');
    return;
  }
  final AssetId jsId = module.primarySource.changeExtension(jsModuleExtension);
  final String appModuleName = ddcModuleName(jsId);
  final AssetId appDigestsOutput =
      dartEntrypointId.changeExtension(digestsEntrypointExtension);

  // The name of the entrypoint dart library within the entrypoint JS module.
  //
  // This is used to invoke `main()` from within the bootstrap script.
  //
  // TODO(jakemac53): Sane module name creation, this only works in the most
  // basic of cases.
  //
  // See https://github.com/dart-lang/sdk/issues/27262 for the root issue
  // which will allow us to not rely on the naming schemes that dartdevc uses
  // internally, but instead specify our own.
  final String oldAppModuleScope = toJSIdentifier(
      path.url.withoutExtension(path.url.basename(buildStep.inputId.path)));

  // Like above but with a package-relative entrypoint.
  final String appModuleScope =
      pathToJSIdentifier(path.url.withoutExtension(buildStep.inputId.path));

  // Map from module name to module path for custom modules.
  final SplayTreeMap<String, String> modulePaths = SplayTreeMap<String, String>.of(
      <String, String>{'dart_sdk': r'packages/build_web_compilers/src/dev_compiler/dart_sdk'});
  for (AssetId jsId in transitiveJsModules) {
    // Strip out the top level dir from the path for any module, and set it to
    // `packages/` for lib modules. We set baseUrl to `/` to simplify things,
    // and we only allow you to serve top level directories.
    final String moduleName = ddcModuleName(jsId);
    modulePaths[moduleName] = path.url.withoutExtension(
        jsId.path.startsWith('lib')
            ? '$moduleName$jsModuleExtension'
            : path.url.joinAll(path.url.split(jsId.path).skip(1)));
  }

  final AssetId bootstrapId = dartEntrypointId.changeExtension(ddcBootstrapExtension);
  final String bootstrapModuleName = path.url.withoutExtension(path.url.relative(
      bootstrapId.path,
      from: path.url.dirname(dartEntrypointId.path)));

  final List<String> primarySourceParts = path.url.split(module.primarySource.path);
  final String appModuleUri = path.url.joinAll(<String>[
    // Convert to a package: uri for files under lib.
    if (primarySourceParts.first == 'lib')
      'package:${module.primarySource.package}',
    // Strip top-level directory from the path.
    ...primarySourceParts.skip(1),
  ]);

  final StringBuffer bootstrapContent =
      StringBuffer('$_entrypointExtensionMarker\n(function() {\n')
        ..write(_dartLoaderSetup(
            modulePaths,
            path.url.relative(appDigestsOutput.path,
                from: path.url.dirname(bootstrapId.path))))
        ..write(_requireJsConfig)
        ..write(_appBootstrap(
            bootstrapModuleName, appModuleName, appModuleScope, appModuleUri,
            oldModuleScope: oldAppModuleScope));

  await buildStep.writeAsString(bootstrapId, bootstrapContent.toString());

  final String entrypointJsContent = _entryPointJs(bootstrapModuleName);
  await buildStep.writeAsString(
      dartEntrypointId.changeExtension(jsEntrypointExtension),
      entrypointJsContent);

  // Output the digests for transitive modules.
  // These can be consumed for hot reloads.
  final Map<String, String> moduleDigests = <String, String>{
    for (AssetId jsId in transitiveJsModules)
      _moduleDigestKey(jsId): '${await buildStep.digest(jsId)}',
  };
  await buildStep.writeAsString(appDigestsOutput, jsonEncode(moduleDigests));
}

/// The module name according to ddc for [jsId] which represents the real js
/// module file.
String ddcModuleName(AssetId jsId) {
  final String jsPath = jsId.path.startsWith('lib/')
      ? jsId.path.replaceFirst('lib/', 'packages/${jsId.package}/')
      : jsId.path;
  return jsPath.substring(0, jsPath.length - jsModuleExtension.length);
}

String _moduleDigestKey(AssetId jsId) =>
    '${ddcModuleName(jsId)}$jsModuleExtension';

final Pool _lazyBuildPool = Pool(16);

/// Ensures that all transitive js modules for [module] are available and built.
///
/// Throws an [UnsupportedModules] exception if there are any
/// unsupported modules.
Future<List<AssetId>> _ensureTransitiveJsModules(
    Module module, BuildStep buildStep) async {
  // Collect all the modules this module depends on, plus this module.
  final List<Module> transitiveDeps = await module.computeTransitiveDependencies(buildStep,
      throwIfUnsupported: false);

  final List<AssetId> jsModules = <AssetId>[
    module.primarySource.changeExtension(jsModuleExtension),
    for (Module dep in transitiveDeps)
      dep.primarySource.changeExtension(jsModuleExtension),
  ];
  // Check that each module is readable, and warn otherwise.
  await Future.wait(jsModules.map((AssetId jsId) async {
    if (await _lazyBuildPool.withResource(() => buildStep.canRead(jsId))) {
      return;
    }
    final AssetId errorsId = jsId.addExtension('.errors');
    await buildStep.canRead(errorsId);
    log.warning('Unable to read $jsId, check your console or the '
        '`.dart_tool/build/generated/${errorsId.package}/${errorsId.path}` '
        'log file.');
  }));
  return jsModules;
}

/// Code that actually imports the [moduleName] module, and calls the
/// `[moduleScope].main()` function on it.
///
/// Also performs other necessary initialization.
String _appBootstrap(String bootstrapModuleName, String moduleName,
        String moduleScope, String appModuleUri,
        {String oldModuleScope}) =>
    '''
define("$bootstrapModuleName", ["$moduleName", "dart_sdk"], function(app, dart_sdk) {
  dart_sdk.dart.setStartAsyncSynchronously(true);
  dart_sdk._isolate_helper.startRootIsolate(() => {}, []);
  $_initializeTools
  $_mainExtensionMarker
  (app.$moduleScope || app.$oldModuleScope).main();
  var bootstrap = {
      hot\$onChildUpdate: function(childName, child) {
        // Special handling for the multi-root scheme uris. We need to strip
        // out the scheme and the top level directory, to match the source path
        // that chrome sees.
        if (childName.startsWith('$multiRootScheme:///')) {
          childName = childName.substring('$multiRootScheme:///'.length);
          var firstSlash = childName.indexOf('/');
          if (firstSlash == -1) return false;
          childName = childName.substring(firstSlash + 1);
        }
        if (childName === "$appModuleUri") {
          // Clear static caches.
          dart_sdk.dart.hotRestart();
          child.main();
          return true;
        }
      }
    }
  dart_sdk.dart.trackLibraries("$bootstrapModuleName", {
    "$bootstrapModuleName": bootstrap
  }, '');
  return {
    bootstrap: bootstrap
  };
});
})();
''';

/// The actual entrypoint JS file which injects all the necessary scripts to
/// run the app.
String _entryPointJs(String bootstrapModuleName) => '''
(function() {
  $_currentDirectoryScript
  $_baseUrlScript

  var mapperUri = baseUrl + "packages/build_web_compilers/src/" +
      "dev_compiler_stack_trace/stack_trace_mapper.dart.js";
  var requireUri = baseUrl +
      "packages/build_web_compilers/src/dev_compiler/require.js";
  var mainUri = _currentDirectory + "$bootstrapModuleName";

  if (typeof document != 'undefined') {
    var el = document.createElement("script");
    el.defer = true;
    el.async = false;
    el.src = mapperUri;
    document.head.appendChild(el);

    el = document.createElement("script");
    el.defer = true;
    el.async = false;
    el.src = requireUri;
    el.setAttribute("data-main", mainUri);
    document.head.appendChild(el);
  } else {
    importScripts(mapperUri, requireUri);
    require.config({
      baseUrl: baseUrl,
    });
    // TODO: update bootstrap code to take argument - dart-lang/build#1115
    window = self;
    require([mainUri + '.js']);
  }
})();
''';

/// JavaScript snippet to determine the directory a script was run from.
const String _currentDirectoryScript = r'''
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
''';

/// Sets up `window.$dartLoader` based on [modulePaths].
String _dartLoaderSetup(Map<String, String> modulePaths, String appDigests) =>
    '''
$_currentDirectoryScript
$_baseUrlScript
let modulePaths = ${const JsonEncoder.withIndent(" ").convert(modulePaths)};
if(!window.\$dartLoader) {
   window.\$dartLoader = {
     appDigests: _currentDirectory + '$appDigests',
     moduleIdToUrl: new Map(),
     urlToModuleId: new Map(),
     rootDirectories: new Array(),
     // Used in package:build_runner/src/server/build_updates_client/hot_reload_client.dart
     moduleParentsGraph: new Map(),
     moduleLoadingErrorCallbacks: new Map(),
     forceLoadModule: function (moduleName, callback, onError) {
       // dartdevc only strips the final extension when adding modules to source
       // maps, so we need to do the same.
       if (moduleName.endsWith('$_modulePartialExtension')) {
         moduleName = moduleName.substring(0, moduleName.length - ${_modulePartialExtension.length});
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
''';

/// Code to initialize the dev tools formatter, stack trace mapper, and any
/// other tools.
///
/// Posts a message to the window when done.
const String _initializeTools = '''
$_baseUrlScript
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
''';

/// Require JS config for ddc.
///
/// Sets the base url to `/` so that all modules can be loaded using absolute
/// paths which simplifies a lot of scenarios.
///
/// Sets the timeout for loading modules to infinity (0).
///
/// Sets up the custom module paths.
///
/// Adds error handler code for require.js which requests a `.errors` file for
/// any failed module, and logs it to the console.
final String _requireJsConfig = '''
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

$_baseUrlScript;

require.config({
    baseUrl: baseUrl,
    waitSeconds: 0,
    paths: customModulePaths
});

const modulesGraph = new Map();
function getRegisteredModuleName(moduleMap) {
  if (\$dartLoader.moduleIdToUrl.has(moduleMap.name + '$_modulePartialExtension')) {
    return moduleMap.name + '$_modulePartialExtension';
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
''';

/// Marker comment used by tools to identify the entrypoint file,
/// to inject custom code.
const String _entrypointExtensionMarker = '/* ENTRYPOINT_EXTENTION_MARKER */';

/// Marker comment used by tools to identify the main function
/// to inject custom code.
const String _mainExtensionMarker = '/* MAIN_EXTENSION_MARKER */';

const String _baseUrlScript = '''
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
''';
