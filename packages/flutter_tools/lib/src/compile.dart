// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'dart:typed_data';

import 'package:dev_compiler/dev_compiler.dart' show DevCompilerTarget;
import 'package:front_end/src/api_prototype/compiler_options.dart'
    show CompilerOptions, parseExperimentalFlags;
import 'package:front_end/src/api_unstable/vm.dart';
import 'package:front_end/src/fasta/incremental_serializer.dart';
import 'package:kernel/ast.dart';
import 'package:kernel/binary/ast_to_binary.dart';
import 'package:kernel/kernel.dart'
    show Component;
import 'package:kernel/target/targets.dart' show targets, TargetFlags;
import 'package:meta/meta.dart';
import 'package:vm/incremental_compiler.dart' show IncrementalCompiler;
import 'package:vm/kernel_front_end.dart';

import 'artifacts.dart';
import 'base/common.dart';
import 'base/context.dart';
import 'base/file_system.dart' hide FileSystem;
import 'base/io.dart';
import 'base/platform.dart';
import 'base/process_manager.dart';
import 'base/terminal.dart';
import 'build_info.dart';
import 'codegen.dart';
import 'convert.dart';
import 'dart/package_map.dart';
import 'globals.dart';
import 'project.dart';

KernelCompilerFactory get kernelCompilerFactory => context.get<KernelCompilerFactory>();

class KernelCompilerFactory {
  const KernelCompilerFactory();

  Future<KernelCompiler> create(FlutterProject flutterProject) async {
    if (flutterProject == null || !flutterProject.hasBuilders) {
      return const KernelCompiler();
    }
    return const CodeGeneratingKernelCompiler();
  }
}

typedef CompilerMessageConsumer = void Function(String message, { bool emphasis, TerminalColor color });

/// The target model describes the set of core libraries that are available within
/// the SDK.
class TargetModel {
  /// Parse a [TargetModel] from a raw string.
  ///
  /// Throws an [AssertionError] if passed a value other than 'flutter' or
  /// 'flutter_runner'.
  factory TargetModel(String rawValue) {
    switch (rawValue) {
      case 'flutter':
        return flutter;
      case 'flutter_runner':
        return flutterRunner;
      case 'vm':
        return vm;
      case 'dartdevc':
        return dartdevc;
    }
    assert(false);
    return null;
  }

  const TargetModel._(this._value);

  /// The flutter patched dart SDK
  static const TargetModel flutter = TargetModel._('flutter');

  /// The fuchsia patched SDK.
  static const TargetModel flutterRunner = TargetModel._('flutter_runner');

  /// The Dart vm.
  static const TargetModel vm = TargetModel._('vm');

  /// The development compiler for JavaScript.
  static const TargetModel dartdevc = TargetModel._('dartdevc');

  final String _value;

  @override
  String toString() => _value;
}

class CompilerOutput {
  const CompilerOutput(this.outputFilename, this.errorCount, this.sources);

  final String outputFilename;
  final int errorCount;
  final List<Uri> sources;
}

class DirectCompilerOutput {
  DirectCompilerOutput(this.buffer, this.sources);

  final Uint8List buffer;
  final List<Uri> sources;
  final int errorCount = 0;
}

enum StdoutState { CollectDiagnostic, CollectDependencies }

/// Handles stdin/stdout communication with the frontend server.
class StdoutHandler {
  StdoutHandler({this.consumer = printError}) {
    reset();
  }

  bool compilerMessageReceived = false;
  final CompilerMessageConsumer consumer;
  String boundaryKey;
  StdoutState state = StdoutState.CollectDiagnostic;
  Completer<CompilerOutput> compilerOutput;
  final List<Uri> sources = <Uri>[];

  bool _suppressCompilerMessages;
  bool _expectSources;
  bool _badState = false;

  void handler(String message) {
    if (_badState) {
      return;
    }
    const String kResultPrefix = 'result ';
    if (boundaryKey == null && message.startsWith(kResultPrefix)) {
      boundaryKey = message.substring(kResultPrefix.length);
      return;
    }
    // Invalid state, see commented issue below for more information.
    // NB: both the completeError and _badState flags are required to avoid
    // filling the console with exceptions.
    if (boundaryKey == null) {
      // Throwing a synchronous exception via throwToolExit will fail to cancel
      // the stream. Instead use completeError so that the error is returned
      // from the awaited future that the compiler consumers are expecting.
      compilerOutput.completeError(ToolExit(
        'The Dart compiler encountered an internal problem. '
        'The Flutter team would greatly appreciate if you could leave a '
        'comment on the issue https://github.com/flutter/flutter/issues/35924 '
        'describing what you were doing when the crash happened.\n\n'
        'Additional debugging information:\n'
        '  StdoutState: $state\n'
        '  compilerMessageReceived: $compilerMessageReceived\n'
        '  _expectSources: $_expectSources\n'
        '  sources: $sources\n'
      ));
      // There are several event turns before the tool actually exits from a
      // tool exception. Normally, the stream should be cancelled to prevent
      // more events from entering the bad state, but because the error
      // is coming from handler itself, there is no clean way to pipe this
      // through. Instead, we set a flag to prevent more messages from
      // registering.
      _badState = true;
      return;
    }
    if (message.startsWith(boundaryKey)) {
      if (_expectSources) {
        if (state == StdoutState.CollectDiagnostic) {
          state = StdoutState.CollectDependencies;
          return;
        }
      }
      if (message.length <= boundaryKey.length) {
        compilerOutput.complete(null);
        return;
      }
      final int spaceDelimiter = message.lastIndexOf(' ');
      compilerOutput.complete(
          CompilerOutput(
              message.substring(boundaryKey.length + 1, spaceDelimiter),
              int.parse(message.substring(spaceDelimiter + 1).trim()),
              sources));
      return;
    }
    if (state == StdoutState.CollectDiagnostic) {
      if (!_suppressCompilerMessages) {
        if (compilerMessageReceived == false) {
          consumer('\nCompiler message:');
          compilerMessageReceived = true;
        }
        consumer(message);
      }
    } else {
      assert(state == StdoutState.CollectDependencies);
      switch (message[0]) {
        case '+':
          sources.add(Uri.parse(message.substring(1)));
          break;
        case '-':
          sources.remove(Uri.parse(message.substring(1)));
          break;
        default:
          printTrace('Unexpected prefix for $message uri - ignoring');
      }
    }
  }

  // This is needed to get ready to process next compilation result output,
  // with its own boundary key and new completer.
  void reset({ bool suppressCompilerMessages = false, bool expectSources = true }) {
    boundaryKey = null;
    compilerMessageReceived = false;
    compilerOutput = Completer<CompilerOutput>();
    _suppressCompilerMessages = suppressCompilerMessages;
    _expectSources = expectSources;
    state = StdoutState.CollectDiagnostic;
  }
}

/// Converts filesystem paths to package URIs.
class PackageUriMapper {
  PackageUriMapper(String scriptPath, String packagesPath, String fileSystemScheme, List<String> fileSystemRoots) {
    final Map<String, Uri> packageMap = PackageMap(fs.path.absolute(packagesPath)).map;
    final String scriptUri = Uri.file(scriptPath, windows: platform.isWindows).toString();
    for (String packageName in packageMap.keys) {
      final String prefix = packageMap[packageName].toString();
      // Only perform a multi-root mapping if there are multiple roots.
      if (fileSystemScheme != null
        && fileSystemRoots != null
        && fileSystemRoots.length > 1
        && prefix.contains(fileSystemScheme)) {
        _packageName = packageName;
        _uriPrefixes = fileSystemRoots
          .map((String name) => Uri.file(name, windows: platform.isWindows).toString())
          .toList();
        return;
      }
      if (scriptUri.startsWith(prefix)) {
        _packageName = packageName;
        _uriPrefixes = <String>[prefix];
        return;
      }
    }
  }

  String _packageName;
  List<String> _uriPrefixes;

  Uri map(String scriptPath) {
    if (_packageName == null) {
      return null;
    }
    final String scriptUri = Uri.file(scriptPath, windows: platform.isWindows).toString();
    for (String uriPrefix in _uriPrefixes) {
      if (scriptUri.startsWith(uriPrefix)) {
        return Uri.parse('package:$_packageName/${scriptUri.substring(uriPrefix.length)}');
      }
    }
    return null;
  }

  static Uri findUri(String scriptPath, String packagesPath, String fileSystemScheme, List<String> fileSystemRoots) {
    return PackageUriMapper(scriptPath, packagesPath, fileSystemScheme, fileSystemRoots).map(scriptPath);
  }
}

List<String> _buildModeOptions(BuildMode mode) {
  switch (mode) {
    case BuildMode.debug:
      return <String>[
        '-Ddart.vm.profile=false',
        '-Ddart.vm.product=false',
        '--bytecode-options=source-positions,local-var-info,debugger-stops,instance-field-initializers,keep-unreachable-code,avoid-closure-call-instructions',
        '--enable-asserts',
      ];
    case BuildMode.profile:
      return <String>[
        '-Ddart.vm.profile=true',
        '-Ddart.vm.product=false',
        '--bytecode-options=source-positions',
      ];
    case BuildMode.release:
      return <String>[
        '-Ddart.vm.profile=false',
        '-Ddart.vm.product=true',
        '--bytecode-options=source-positions',
      ];
  }
  throw Exception('Unknown BuildMode: $mode');
}

class KernelCompiler {
  const KernelCompiler();

  Future<CompilerOutput> compile({
    String sdkRoot,
    String mainPath,
    String outputFilePath,
    String depFilePath,
    TargetModel targetModel = TargetModel.flutter,
    @required BuildMode buildMode,
    bool linkPlatformKernelIn = false,
    bool aot = false,
    bool causalAsyncStacks = true,
    @required bool trackWidgetCreation,
    List<String> extraFrontEndOptions,
    String packagesPath,
    List<String> fileSystemRoots,
    String fileSystemScheme,
    String initializeFromDill,
    String platformDill,
    @required List<String> dartDefines,
  }) async {
    final String frontendServer = artifacts.getArtifactPath(
      Artifact.frontendServerSnapshotForEngineDartSdk
    );
    // This is a URI, not a file path, so the forward slash is correct even on Windows.
    if (!sdkRoot.endsWith('/')) {
      sdkRoot = '$sdkRoot/';
    }
    final String engineDartPath = artifacts.getArtifactPath(Artifact.engineDartBinary);
    if (!processManager.canRun(engineDartPath)) {
      throwToolExit('Unable to find Dart binary at $engineDartPath');
    }
    Uri mainUri;
    if (packagesPath != null) {
      mainUri = PackageUriMapper.findUri(mainPath, packagesPath, fileSystemScheme, fileSystemRoots);
    }
    // TODO(jonahwilliams): The output file must already exist, but this seems
    // unnecessary.
    if (outputFilePath != null && !fs.isFileSync(outputFilePath)) {
      fs.file(outputFilePath).createSync(recursive: true);
    }
    final List<String> command = <String>[
      engineDartPath,
      frontendServer,
      '--sdk-root',
      sdkRoot,
      '--target=$targetModel',
      '-Ddart.developer.causal_async_stacks=$causalAsyncStacks',
      for (Object dartDefine in dartDefines)
        '-D$dartDefine',
      ..._buildModeOptions(buildMode),
      if (trackWidgetCreation) '--track-widget-creation',
      if (!linkPlatformKernelIn) '--no-link-platform',
      if (aot) ...<String>[
        '--aot',
        '--tfa',
      ],
      if (packagesPath != null) ...<String>[
        '--packages',
        packagesPath,
      ],
      if (outputFilePath != null) ...<String>[
        '--output-dill',
        outputFilePath,
      ],
      if (depFilePath != null && (fileSystemRoots == null || fileSystemRoots.isEmpty)) ...<String>[
        '--depfile',
        depFilePath,
      ],
      if (fileSystemRoots != null)
        for (String root in fileSystemRoots) ...<String>[
          '--filesystem-root',
          root,
        ],
      if (fileSystemScheme != null) ...<String>[
        '--filesystem-scheme',
        fileSystemScheme,
      ],
      if (initializeFromDill != null) ...<String>[
        '--initialize-from-dill',
        initializeFromDill,
      ],
      if (platformDill != null) ...<String>[
        '--platform',
        platformDill,
      ],
      ...?extraFrontEndOptions,
      mainUri?.toString() ?? mainPath,
    ];

    printTrace(command.join(' '));
    final Process server = await processManager
      .start(command)
      .catchError((dynamic error, StackTrace stack) {
        printError('Failed to start frontend server $error, $stack');
      });

    final StdoutHandler _stdoutHandler = StdoutHandler();

    server.stderr
      .transform<String>(utf8.decoder)
      .listen(printError);
    server.stdout
      .transform<String>(utf8.decoder)
      .transform<String>(const LineSplitter())
      .listen(_stdoutHandler.handler);
    final int exitCode = await server.exitCode;
    if (exitCode == 0) {
      return _stdoutHandler.compilerOutput.future;
    }
    return null;
  }
}

/// Class that allows to serialize compilation requests to the compiler.
abstract class _CompilationRequest {
  _CompilationRequest(this.completer);

  Completer<DirectCompilerOutput> completer;

  Future<DirectCompilerOutput> _run(DefaultResidentCompiler compiler);

  Future<void> run(DefaultResidentCompiler compiler) async {
    completer.complete(await _run(compiler));
  }
}

class _RecompileRequest extends _CompilationRequest {
  _RecompileRequest(
    Completer<DirectCompilerOutput> completer,
    this.mainPath,
    this.invalidatedFiles,
    this.outputPath,
    this.packagesFilePath,
  ) : super(completer);

  String mainPath;
  List<Uri> invalidatedFiles;
  String outputPath;
  String packagesFilePath;

  @override
  Future<DirectCompilerOutput> _run(DefaultResidentCompiler compiler) async {
    final Uint8List results = await compiler._recompile(this);
    return DirectCompilerOutput(
      results,
      compiler._frontendCompiler.previouslyReportedDependencies.toList(),
    );
  }
}

/// Wrapper around incremental frontend server compiler, that communicates with
/// server via stdin/stdout.
///
/// The wrapper is intended to stay resident in memory as user changes, reloads,
/// restarts the Flutter app.
abstract class ResidentCompiler {
  factory ResidentCompiler(String sdkRoot, {
    @required BuildMode buildMode,
    bool causalAsyncStacks,
    bool trackWidgetCreation,
    String packagesPath,
    List<String> fileSystemRoots,
    String fileSystemScheme,
    String initializeFromDill,
    TargetModel targetModel,
    bool unsafePackageSerialization,
    List<String> experimentalFlags,
    String platformDill,
    List<String> dartDefines,
  }) = DefaultResidentCompiler;


  /// If invoked for the first time, it compiles Dart script identified by
  /// [mainPath], [invalidatedFiles] list is ignored.
  /// On successive runs [invalidatedFiles] indicates which files need to be
  /// recompiled. If [mainPath] is [null], previously used [mainPath] entry
  /// point that is used for recompilation.
  /// Binary file name is returned if compilation was successful, otherwise
  /// null is returned.
  Future<DirectCompilerOutput> recompile(
    String mainPath,
    List<Uri> invalidatedFiles, {
    @required String outputPath,
    String packagesFilePath,
  });

  Future<DirectCompilerOutput> compileExpression(
    String expression,
    List<String> definitions,
    List<String> typeDefinitions,
    String libraryUri,
    String klass,
    bool isStatic,
  );

  /// Should be invoked when results of compilation are accepted by the client.
  ///
  /// Either [accept] or [reject] should be called after every [recompile] call.
  void accept();

  /// Should be invoked when results of compilation are rejected by the client.
  ///
  /// Either [accept] or [reject] should be called after every [recompile] call.
  Future<void> reject();

  /// Should be invoked when frontend server compiler should forget what was
  /// accepted previously so that next call to [recompile] produces complete
  /// kernel file.
  void reset();

  Future<dynamic> shutdown();
}

@visibleForTesting
class DefaultResidentCompiler implements ResidentCompiler {
  DefaultResidentCompiler(
    String sdkRoot, {
    @required this.buildMode,
    this.causalAsyncStacks = true,
    this.trackWidgetCreation = true,
    this.packagesPath,
    this.fileSystemRoots,
    this.fileSystemScheme,
    this.initializeFromDill,
    this.targetModel = TargetModel.flutter,
    this.unsafePackageSerialization,
    this.experimentalFlags,
    this.platformDill,
    List<String> dartDefines,
  }) : assert(sdkRoot != null),
       dartDefines = dartDefines ?? const <String>[],
       // This is a URI, not a file path, so the forward slash is correct even on Windows.
       sdkRoot = sdkRoot.endsWith('/') ? sdkRoot : '$sdkRoot/';

  PackageUriMapper _packageUriMapper;
  final BuildMode buildMode;
  final bool causalAsyncStacks;
  final bool trackWidgetCreation;
  final String packagesPath;
  final TargetModel targetModel;
  final List<String> fileSystemRoots;
  final String fileSystemScheme;
  final String initializeFromDill;
  final bool unsafePackageSerialization;
  final List<String> experimentalFlags;
  final List<String> dartDefines;

  /// The path to the root of the Dart SDK used to compile.
  ///
  /// This is used to resolve the [platformDill].
  final String sdkRoot;

  /// The path to the platform dill file.
  ///
  /// This does not need to be provided for the normal Flutter workflow.
  final String platformDill;

  // Process _server;
  // final StdoutHandler _stdoutHandler;
  // bool _compileRequestNeedsConfirmation = false;
  FrontendCompiler _frontendCompiler;

  final StreamController<_CompilationRequest> _controller = StreamController<_CompilationRequest>();

  @override
  Future<DirectCompilerOutput> recompile(
    String mainPath,
    List<Uri> invalidatedFiles, {
    @required String outputPath,
    String packagesFilePath,
  }) async {
    assert (outputPath != null);
    if (!_controller.hasListener) {
      _controller.stream.listen(_handleCompilationRequest);
    }

    final Completer<DirectCompilerOutput> completer = Completer<DirectCompilerOutput>();
    _controller.add(
        _RecompileRequest(completer, mainPath, invalidatedFiles, outputPath, packagesFilePath)
    );
    return completer.future;
  }

  Future<Uint8List> _recompile(_RecompileRequest request) async {
    if (_frontendCompiler == null) {
      _packageUriMapper = PackageUriMapper(request.mainPath, packagesPath ?? request.packagesFilePath, fileSystemScheme, fileSystemRoots);
      print(_packageUriMapper.map(request.mainPath).toString());
      return _compile(_packageUriMapper.map(request.mainPath).toString(), request.packagesFilePath ?? packagesPath);
    }
    request.invalidatedFiles.forEach(_frontendCompiler.invalidate);
    return _frontendCompiler.recompileDelta(entryPoint: _packageUriMapper.map(request.mainPath).toString());
  }

  final List<_CompilationRequest> _compilationQueue = <_CompilationRequest>[];

  Future<void> _handleCompilationRequest(_CompilationRequest request) async {
    final bool isEmpty = _compilationQueue.isEmpty;
    _compilationQueue.add(request);
    // Only trigger processing if queue was empty - i.e. no other requests
    // are currently being processed. This effectively enforces "one
    // compilation request at a time".
    if (isEmpty) {
      while (_compilationQueue.isNotEmpty) {
        final _CompilationRequest request = _compilationQueue.first;
        await request.run(this);
        _compilationQueue.removeAt(0);
      }
    }
  }

  Future<Uint8List> _compile(
    String scriptUri,
    String packagesFilePath,
  ) async {
    _frontendCompiler = FrontendCompiler(
      sdkRootPath: sdkRoot,
      packagesPath: packagesFilePath,
    );
    return _frontendCompiler.compile(scriptUri);
  }

  @override
  Future<DirectCompilerOutput> compileExpression(
    String expression,
    List<String> definitions,
    List<String> typeDefinitions,
    String libraryUri,
    String klass,
    bool isStatic,
  ) {
    if (!_controller.hasListener) {
      _controller.stream.listen(_handleCompilationRequest);
    }
    return Future<DirectCompilerOutput>.value(null);
  }

  @override
  void accept() {
    _frontendCompiler.acceptLastDelta();
  }

  @override
  Future<void> reject() {
    return _frontendCompiler?.rejectLastDelta();
  }


  @override
  void reset() {
    _frontendCompiler?.resetIncrementalCompiler();
  }

  @override
  Future<dynamic> shutdown() async {}
}


//////////////////

class FrontendCompiler {
  FrontendCompiler({
    this.incrementalSerialization = true,
    this.sdkRootPath,
    this.packagesPath,
  });

  final String sdkRootPath;
  final String packagesPath;

  bool incrementalSerialization;
  CompilerOptions _compilerOptions;
  FileSystem _fileSystem = createFrontEndFileSystem(null, null);
  Uri _mainSource;
  IncrementalCompiler _generator;

  Set<Uri> previouslyReportedDependencies = <Uri>{};
  final List<String> errors = <String>[];

  Future<Uint8List> compile(
    String entryPoint, {
    IncrementalCompiler generator,
  }) async {
    _fileSystem = createFrontEndFileSystem(null, null);
    _mainSource = _getFileOrUri(entryPoint);
    final Uri sdkRoot = _ensureFolderPath(sdkRootPath);
    const String platformKernelDill = 'platform_strong.dill';
    final CompilerOptions compilerOptions = CompilerOptions()
      ..sdkRoot = sdkRoot
      ..fileSystem = _fileSystem
      ..packagesFileUri = _getFileOrUri(packagesPath)
      ..sdkSummary = sdkRoot.resolve(platformKernelDill)
      ..verbose = false
      ..embedSourceText = true
      ..experimentalFlags = parseExperimentalFlags(
          parseExperimentalArguments(<String>[]),
          onError: (String msg) => errors.add(msg));
    final Map<String, String> environmentDefines = <String, String>{};
    compilerOptions.bytecode = false;

    // Initialize additional supported kernel targets.
    targets['dartdevc'] = (TargetFlags flags) => DevCompilerTarget(flags);
    compilerOptions.target = createFrontEndTarget(
      'flutter',
      trackWidgetCreation: true,
    );
    _compilerOptions = compilerOptions;

    KernelCompilationResults results;
    IncrementalSerializer incrementalSerializer;
      setVMEnvironmentDefines(environmentDefines, _compilerOptions);
    _compilerOptions.omitPlatform = false;
    _generator = generator ?? _createGenerator(null);
    final Component component = await _generator.compile();
    results = KernelCompilationResults(
        component,
        null,
        _generator.getClassHierarchy(),
        _generator.getCoreTypes(),
        component.uriToSource.keys);

    incrementalSerializer = _generator.incrementalSerializer;

    if (results.component != null) {
        return writeDillFile(results,
            filterExternal: false,
            incrementalSerializer: incrementalSerializer);
    }
    return null;
  }

  Future<Uint8List> writeDillFile(KernelCompilationResults results,
      {bool filterExternal = false,
      IncrementalSerializer incrementalSerializer}) async {
    final Component component = results.component;

    previouslyReportedDependencies?.clear();
    for (Uri uri in results.compiledSources) {
      if (uri == null || uri.scheme == 'org-dartlang-sdk') {
        continue;
      }
      previouslyReportedDependencies.add(uri);
    }
    final ByteSink sink = ByteSink();
    final BinaryPrinter printer = BinaryPrinter(sink);
    sortComponent(component);
    printer.writeComponentFile(component);
    return sink.builder.takeBytes();
  }

  Future<Uint8List> recompileDelta({String entryPoint}) async {
    if (entryPoint != null) {
      _mainSource = _getFileOrUri(entryPoint);
    }
    errors.clear();
    final Component deltaProgram = await _generator.compile(entryPoint: _mainSource);
    final KernelCompilationResults results = KernelCompilationResults(
        deltaProgram,
        null,
        _generator.getClassHierarchy(),
        _generator.getCoreTypes(),
        deltaProgram.uriToSource.keys);

    return writeDillFile(results,
      incrementalSerializer: _generator.incrementalSerializer);
  }

  Future<Uint8List> compileExpression(
      String expression,
      List<String> definitions,
      List<String> typeDefinitions,
      String libraryUri,
      String klass,
      bool isStatic) async {
    final Procedure procedure = await _generator.compileExpression(
        expression, definitions, typeDefinitions, libraryUri, klass, isStatic);
    if (procedure != null) {
      final Component component = createExpressionEvaluationComponent(procedure);
      return serializeComponent(component);
    }
    return null;
  }

  void reportError(String msg) {
    printError(msg);
  }

  void acceptLastDelta() {
    _generator.accept();
  }

  Future<void> rejectLastDelta() async {
    await _generator?.reject();
  }

  void invalidate(Uri uri) {
    _generator?.invalidate(uri);
  }


  void resetIncrementalCompiler() {
    _generator?.resetDeltaState();
  }

  Uri _getFileOrUri(String fileOrUri) =>
      convertFileOrUriArgumentToUri(_fileSystem, fileOrUri);

  IncrementalCompiler _createGenerator(Uri initializeFromDillUri) {
    return IncrementalCompiler(_compilerOptions, _mainSource,
        initializeFromDillUri: initializeFromDillUri,
        incrementalSerialization: incrementalSerialization);
  }

  Uri _ensureFolderPath(String path) {
    String uriPath = Uri.file(path).toString();
    if (!uriPath.endsWith('/')) {
      uriPath = '$uriPath/';
    }
    return Uri.base.resolve(uriPath);
  }
}

/// A [Sink] that directly writes data into a byte builder.
class ByteSink implements Sink<List<int>> {
  final BytesBuilder builder = BytesBuilder();

  @override
  void add(List<int> data) {
    builder.add(data);
  }

  @override
  void close() {}
}