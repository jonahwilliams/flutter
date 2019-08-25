// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ignore_for_file: implementation_imports
import 'dart:async';
import 'dart:io';
import 'dart:math';

import 'package:flutter_tools/src/base/common.dart';
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
double angle = 0;
const String spicyReload = 'hello_world';

class SpicyReloadCommand extends FlutterCommand {
  SpicyReloadCommand() {
    this.argParser.addOption('invalidated');
    this.argParser.addOption('vmservice');
    this.argParser.addOption('target');
    this.argParser.addOption('class');
  }

  @override
  String get description => 'Mmm, spicy';

  @override
  String get name => 'spicy';

  final Completer<void> done = Completer<void>();
  Uri mainUri;
  Uri deviceUri;
  String className;
  VMService vmService;
  HttpClient httpClient;
  FrontendCompiler frontendCompiler;
  FlutterView flutterView;
  Component partialComponent;

  Future<void> _reloadSourcesService(
    String isolateId, {
    bool force = false,
    bool pause = false,
  }) async {}

  @override
  Future<FlutterCommandResult> runCommand() async {
    // Initialize arguments.
    final String target = 'package:hello_world/main.dart';
    final Uri observatoryUri = Uri.parse(argResults['vmservice']);
    className = argResults['class'];
    mainUri = Uri.parse(target);
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
    await frontendCompiler.compile(
      mainUri,
      packagesUri: File(PackageMap.globalPackagesPath).absolute.uri,
      sdkRoot: File(artifacts.getArtifactPath(Artifact.flutterPatchedSdkPath,
                  mode: BuildMode.debug) +
              '/')
          .absolute
          .uri,
    );
    frontendCompiler.acceptLastDelta();

    // Initialize dev_fs.
    Map<String, Object> response;
    try {
      response = await vmService.vm.createDevFS(spicyReload);
    } catch (err) {
      await vmService.vm.deleteDevFS(spicyReload);
      response = await vmService.vm.createDevFS(spicyReload);
    }
    deviceUri = Uri.parse(response['uri']);
    print('Starting spicy reload on $mainUri and $className...');
    while (true) {
      await processTerminalInput();
    }
  }

  Future<void> latch = Future.value();

  Future<void> processTerminalInput() async {
    print('>>>>>>>>>>');
    final Stopwatch total = Stopwatch()..start();
    final Stopwatch stopwatch = Stopwatch()..start();
    final String newString = String.fromCharCode(Random().nextInt(225) + 25);

    // Recompille kernel.
    final StringSmasher smasher =
        StringSmasher(File('lib/main.dart').absolute.uri, className, 'build', newString);
    if (partialComponent == null) {
      frontendCompiler.invalidate(Uri.base.resolveUri(mainUri));
      partialComponent = await frontendCompiler.recompileDelta();
      partialComponent.libraries.sort((Library l1, Library l2) {
        return '${l1.fileUri}'.compareTo('${l2.fileUri}');
      });
      partialComponent.computeCanonicalNames();
      for (Library library in partialComponent.libraries) {
        library.additionalExports.sort((Reference r1, Reference r2) {
          return '${r1.canonicalName}'.compareTo('${r2.canonicalName}');
        });
      }
      partialComponent.transformChildren(smasher);
      frontendCompiler.acceptLastDelta();
    } else {
      partialComponent.transformChildren(smasher);
    }
    print('mutate took ${stopwatch.elapsedMilliseconds}');
    stopwatch..reset();

    final IOSink sink = File('app.incremental.dill').openWrite();
    final BinaryPrinter printer = BinaryPrinterFactory().newBinaryPrinter(sink);
    printer.writeComponentFile(partialComponent);
    await sink.close();

    // Send dill to device.
    try {
      final HttpClientRequest request =
          await httpClient.putUrl(vmService.httpAddress);
      request.headers.removeAll(HttpHeaders.acceptEncodingHeader);
      request.headers.add('dev_fs_name', spicyReload);
      request.headers
          .add('dev_fs_uri_b64', base64.encode(utf8.encode('${deviceUri}lib/main.dart.incremental.dill')));

      request.add(gzip.encode(File('app.incremental.dill').readAsBytesSync()));
      await request.close();
    } catch (err) {
      throwToolExit('EXIT');
    }
    print('send took ${stopwatch.elapsedMilliseconds}');
    stopwatch..reset();

    // await previous frame.
    await latch;

    // Tell vmservice to reload and then reassemble.
    await flutterView.uiIsolate.reloadSources(
      pause: false,
      rootLibUri: Uri.parse('${deviceUri}lib/main.dart.incremental.dill'),
      packagesUri: Uri.parse('$deviceUri.packages'),
    );
    print('reloadSources took ${stopwatch.elapsedMilliseconds}');
    stopwatch..reset();
    print('TOTAL: ${total.elapsedMilliseconds}');
    final Completer<void> completer = Completer();
    latch = completer.future;
    unawaited(flutterView.uiIsolate.flutterReassemble().then((_) {
      completer.complete();
    }));
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
    _compilerOptions.onDiagnostic = (DiagnosticMessage message) {
      print(message.plainTextFormatted);
    };
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

  Future<Component> recompileDelta() async {
    errors.clear();
    final Component deltaProgram =
        await _generator.compile(entryPoint: _mainSource);
    return deltaProgram;
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
  StringSmasher(this.fileUri, this.dartClass, this.dartMethod, this.newString);

  final Uri fileUri;
  final String dartClass;
  final String dartMethod;
  final String newString;

  @override
  Library visitLibrary(Library node) {
    if (node.fileUri.toString() == fileUri.toString()) {
      node.transformChildren(this);
    }
    return node;
  }

  @override
  Procedure visitProcedure(Procedure node) {
    if (node.name.name == dartMethod) {
      node.transformChildren(this);
    }
    return node;
  }

  @override
  Class visitClass(Class node) {
    if (node.name == dartClass) {
      node.transformChildren(this);
    }
    return node;
  }

  @override
  ConstructorInvocation visitConstructorInvocation(ConstructorInvocation node) {
    node.transformChildren(this);
    final Constructor constructor = node.target;
    final Class constructedClass = constructor.enclosingClass;
    if (constructedClass.name == 'Transform') {
      changeAngleValue(node, constructor.function, constructedClass);
    }
    return node;
  }

  void changeAngleValue(InvocationExpression node, FunctionNode function,
      Class constructedClass) {
        angle += 2 * 3.14 / 36 ;
      final int index = node.arguments.named.indexWhere((NamedExpression expr) {
        return expr.name == 'angle';
      });
      node.arguments.named[index] = NamedExpression('angle', DoubleConstant(angle).asExpression());
    }

  void _changeTextValue(InvocationExpression node, FunctionNode function,
      Class constructedClass) {
    node.arguments.positional[0] =
        StringConstant(newString).asExpression();
  }
}
