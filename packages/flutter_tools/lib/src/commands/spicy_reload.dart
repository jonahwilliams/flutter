// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ignore_for_file: implementation_imports
import 'dart:async';
import 'dart:developer';
import 'dart:io';

import 'package:flutter_tools/src/build_info.dart';
import 'package:flutter_tools/src/dart/package_map.dart';
import 'package:front_end/src/api_prototype/compiler_options.dart'
    show CompilerOptions;
import 'package:front_end/src/api_unstable/vm.dart';
import 'package:kernel/ast.dart';
import 'package:kernel/binary/ast_to_binary.dart';
import 'package:kernel/kernel.dart'
    show Component, loadComponentSourceFromBytes;
import 'package:kernel/target/targets.dart';
import 'package:meta/meta.dart';
import 'package:vm/frontend_server.dart';
import 'package:vm/incremental_compiler.dart' show IncrementalCompiler;
import 'package:vm/kernel_front_end.dart';
import 'package:vm/kernel_front_end.dart'
    show
        convertFileOrUriArgumentToUri,
        createFrontEndTarget,
        createFrontEndFileSystem,
        setVMEnvironmentDefines;
import 'package:vm/target/flutter.dart';

import '../artifacts.dart';
import '../base/terminal.dart';
import '../convert.dart';
import '../globals.dart';
import '../runner/flutter_command.dart';
import '../vmservice.dart';

const String spicyReload = 'spicy_reload';

class SpicyReloadCommand extends FlutterCommand {
  SpicyReloadCommand() {
    this.argParser.addOption('invalidated');
    this.argParser.addOption('vmservice');
    this.argParser.addOption('target');
  }

  @override
  String get description => 'Mmm, spicy';

  @override
  String get name => 'spicy';

  final Completer<void> done = Completer<void>();
  Uri mainUri;
  Uri fileUri;
  Uri deviceUri;
  VMService vmService;
  HttpClient httpClient;
  FrontendCompiler frontendCompiler;
  FlutterView flutterView;
  Component component;

  Future<void> _reloadSourcesService(
    String isolateId, {
    bool force = false,
    bool pause = false,
  }) async {}

  @override
  Future<FlutterCommandResult> runCommand() async {
    // Initialize arguments.
    final String target = argResults['target'];
    final Uri observatoryUri = Uri.parse(argResults['vmservice']);
    mainUri = File(target).absolute.uri;
    fileUri = File(argResults['invalidated']).absolute.uri;
    // Connect to vm service.
    vmService = await VMService.connect(observatoryUri,
        reloadSources: _reloadSourcesService);
    await vmService.getVM();
    await vmService.refreshViews();
    flutterView = vmService.vm.firstView;

    // Create HTTP client for kernel upload.
    httpClient = HttpClient();

    // Create in memory kernel compiler.
    frontendCompiler = FrontendCompiler();
    component = await frontendCompiler.compile(
      mainUri,
      packagesUri: File(PackageMap.globalPackagesPath).absolute.uri,
      sdkRoot: File(artifacts.getArtifactPath(Artifact.flutterPatchedSdkPath,
                  mode: BuildMode.debug) +
              '/')
          .absolute
          .uri,
    );

    // Initialize dev_fs.
    Map<String, Object> response;
    try {
      response = await vmService.vm.createDevFS(spicyReload);
    } catch (err) {
      await vmService.vm.deleteDevFS(spicyReload);
      response = await vmService.vm.createDevFS(spicyReload);
    }
    deviceUri = Uri.parse(response['uri']);
    print('Ready...');
    terminal.singleCharMode = true;
    terminal.keystrokes.listen(processTerminalInput);
    await done.future;
    return null;
  }

  Future<void> processTerminalInput(String code) async {
    final TimelineTask task = TimelineTask();
    if (code == 'Q' || code == 'q') {
      done.complete();
    }
    if (code != 'r') {
      return;
    }
    final Stopwatch stopwatch = Stopwatch()..start();
    // Recompille kernel.
    task.start('invalidate');
    final StringSmasher smasher =
        StringSmasher(fileUri, '_GalleryHomeState', 'build');
    component.transformChildren(smasher);
    final Component partialComponent = Component(libraries: <Library>[
      if (smasher.currentLibrary != null) smasher.currentLibrary
    ]);
    print('mutate took ${stopwatch.elapsedMilliseconds}');
    stopwatch..reset();

    final IOSink sink = File('app.incremental.dill').openWrite();
    final BinaryPrinter printer = BinaryPrinterFactory().newBinaryPrinter(sink);
    partialComponent.libraries.sort((Library l1, Library l2) {
      return '${l1.fileUri}'.compareTo('${l2.fileUri}');
    });
    partialComponent.computeCanonicalNames();
    for (Library library in partialComponent.libraries) {
      library.additionalExports.sort((Reference r1, Reference r2) {
        return '${r1.canonicalName}'.compareTo('${r2.canonicalName}');
      });
    }
    printer.writeComponentFile(partialComponent);
    await sink.close();

    // Send dill to device.
    task.start('encode');
    final HttpClientRequest request =
        await httpClient.putUrl(vmService.httpAddress);
    request.headers.removeAll(HttpHeaders.acceptEncodingHeader);
    request.headers.add('dev_fs_name', spicyReload);
    request.headers
        .add('dev_fs_uri_b64', base64.encode(utf8.encode('$deviceUri')));
    request.add(gzip.encode(File('app.incremental.dill').readAsBytesSync()));
    final HttpClientResponse response = await request.close();
    await response.drain<void>();
    print('send took ${stopwatch.elapsedMilliseconds}');
    stopwatch..reset();
    task.finish();

    // Tell vmservice to reload and then reassemble.
    task.start('reload');
    final Object result = await flutterView.uiIsolate.reloadSources(
      pause: false,
      rootLibUri: Uri.parse('${deviceUri}lib/main.dart.incremental.dill'),
      packagesUri: Uri.parse('$deviceUri.packages'),
    );
    print(result);
    task.finish();

    task.start('reassemble');
    final Object result2 = await flutterView.uiIsolate.flutterReassemble();
    print(result2);
    task.finish();

    print('reassemble took ${stopwatch.elapsedMilliseconds}');
  }
}

class FrontendCompiler {
  FrontendCompiler(
      {this.printerFactory,
      this.transformer,
      this.unsafePackageSerialization}) {
    printerFactory ??= BinaryPrinterFactory();
  }

  BinaryPrinterFactory printerFactory;
  bool unsafePackageSerialization;

  CompilerOptions _compilerOptions;
  FileSystem _fileSystem;
  Uri _mainSource;

  IncrementalCompiler _generator;
  String _kernelBinaryFilename;
  String _kernelBinaryFilenameIncremental;
  String _kernelBinaryFilenameFull;

  Set<Uri> previouslyReportedDependencies = <Uri>{};
  final ProgramTransformer transformer;
  final List<String> errors = <String>[];

  Future<Component> compile(
    Uri entryPoint, {
    @required Uri sdkRoot,
    @required Uri packagesUri,
    IncrementalCompiler generator,
  }) async {
    _fileSystem = createFrontEndFileSystem(null, null);
    _mainSource = entryPoint;
    _kernelBinaryFilenameFull = 'app.dill';
    _kernelBinaryFilenameIncremental = 'app.incremental.dill';
    _kernelBinaryFilename = _kernelBinaryFilenameFull;
    const String platformKernelDill = 'platform_strong.dill';
    final CompilerOptions compilerOptions = CompilerOptions()
      ..sdkRoot = sdkRoot
      ..fileSystem = _fileSystem
      ..packagesFileUri = packagesUri
      ..sdkSummary = sdkRoot.resolve(platformKernelDill)
      ..verbose = false
      ..embedSourceText = false;
    final Map<String, String> environmentDefines = <String, String>{};
    compilerOptions.bytecode = false;
    compilerOptions.target =
        FlutterTarget(TargetFlags(trackWidgetCreation: true));
    _compilerOptions = compilerOptions;
    setVMEnvironmentDefines(environmentDefines, _compilerOptions);
    _compilerOptions.omitPlatform = false;
    _generator = generator ?? _createGenerator();
    final Component component =
        await _generator.compile(entryPoint: _mainSource);
    if (component != null) {
      if (transformer != null) {
        transformer.transform(component);
      }
      await writeDillFile(component, _kernelBinaryFilename,
          filterExternal: false);
      _kernelBinaryFilename = _kernelBinaryFilenameIncremental;
    }
    return component;
  }

  Future<void> writeDillFile(Component component, String filename,
      {bool filterExternal = false}) async {
    final IOSink sink = File(filename).openWrite();
    final BinaryPrinter printer = printerFactory.newBinaryPrinter(sink);

    component.libraries.sort((Library l1, Library l2) {
      return '${l1.fileUri}'.compareTo('${l2.fileUri}');
    });

    component.computeCanonicalNames();
    for (Library library in component.libraries) {
      library.additionalExports.sort((Reference r1, Reference r2) {
        return '${r1.canonicalName}'.compareTo('${r2.canonicalName}');
      });
    }
    printer.writeComponentFile(component);
    await sink.close();
  }

  Future<void> recompileDelta() async {
    errors.clear();
    final Component deltaProgram =
        await _generator.compile(entryPoint: _mainSource);

    if (deltaProgram != null && transformer != null) {
      transformer.transform(deltaProgram);
    }
    await writeDillFile(deltaProgram, _kernelBinaryFilename);
    _kernelBinaryFilename = _kernelBinaryFilenameIncremental;
  }

  Future<void> compileExpression(
      String expression,
      List<String> definitions,
      List<String> typeDefinitions,
      String libraryUri,
      String klass,
      bool isStatic) async {
    final Procedure procedure = await _generator.compileExpression(
        expression, definitions, typeDefinitions, libraryUri, klass, isStatic);
    if (procedure != null) {
      final Component component =
          createExpressionEvaluationComponent(procedure);
      final IOSink sink = File(_kernelBinaryFilename).openWrite();
      sink.add(serializeComponent(component));
      await sink.close();
      _kernelBinaryFilename = _kernelBinaryFilenameIncremental;
    }
  }

  void reportError(String msg) {
    printError(msg);
  }

  void acceptLastDelta() {
    _generator.accept();
  }

  Future<void> rejectLastDelta() async {
    await _generator.reject();
  }

  void invalidate(Uri uri) {
    _generator.invalidate(uri);
  }

  void resetIncrementalCompiler() {
    _generator.resetDeltaState();
    _kernelBinaryFilename = _kernelBinaryFilenameFull;
  }

  IncrementalCompiler _createGenerator() {
    return IncrementalCompiler(_compilerOptions, _mainSource);
  }
}

class FlutterVisitor extends RecursiveVisitor<dynamic> {
  final List<InstanceConstant> textFields = <InstanceConstant>[];

  @override
  dynamic visitInstanceConstantReference(InstanceConstant node) {
    if (node.classNode.name == 'Text') {
      textFields.add(node);
    }
  }
}

class StringSmasher extends Transformer {
  StringSmasher(this.fileUri, this.dartClass, this.dartMethod);

  Library currentLibrary;
  final Uri fileUri;
  final String dartClass;
  final String dartMethod;

  @override
  Library visitLibrary(Library node) {
    if (node.fileUri.toString() == fileUri.toString()) {
      currentLibrary = node;
      return node.transformChildren(this);
    }
    return node;
  }

  @override
  Procedure visitProcedure(Procedure node) {
    if (node.name.name == dartMethod) {
      return node.transformChildren(this);
    }
    return node;
  }

  @override
  Class visitClass(Class node) {
    if (node.name == dartClass) {
      return node.transformChildren(this);
    }
    return node;
  }

  @override
  ConstructorInvocation visitConstructorInvocation(ConstructorInvocation node) {
    node.transformChildren(this);
    final Constructor constructor = node.target;
    final Class constructedClass = constructor.enclosingClass;
    if (constructedClass.name == 'Text') {
      print('CHANGE TEXT VALUE');
      _changeTextValue(node, constructor.function, constructedClass);
    }
    return node;
  }

  void _changeTextValue(InvocationExpression node, FunctionNode function,
      Class constructedClass) {
    node.arguments.positional[0] =
        StringConstant('DO YOU BELIEVE IN MAGIC').asExpression();
  }
}
