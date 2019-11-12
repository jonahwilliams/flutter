// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This library is a temporary shim until we can detangle the old vmservice library.

import 'package:flutter_tools/src/version.dart';
import 'package:meta/meta.dart';
import 'package:vm_service/vm_service.dart';
import 'package:vm_service/vm_service_io.dart';
import 'package:json_rpc_2/error_code.dart' as rpc_error_code;
import 'package:json_rpc_2/json_rpc_2.dart' as rpc;

import 'convert.dart';

export 'package:vm_service/vm_service.dart';

/// A function that reacts to the invocation of the 'reloadSources' service.
///
/// The VM Service Protocol allows clients to register custom services that
/// can be invoked by other clients through the service protocol itself.
///
/// Clients like Observatory use external 'reloadSources' services,
/// when available, instead of the VM internal one. This allows these clients to
/// invoke Flutter HotReload when connected to a Flutter Application started in
/// hot mode.
///
/// See: https://github.com/dart-lang/sdk/issues/30023
typedef ReloadSources = Future<void> Function(
  String isolateId, {
  bool force,
  bool pause,
});

typedef Restart = Future<void> Function({ bool pause });

typedef CompileExpression = Future<String> Function(
  String isolateId,
  String expression,
  List<String> definitions,
  List<String> typeDefinitions,
  String libraryUri,
  String klass,
  bool isStatic,
);

/// Connect to a [VmService] identifier by the websocket address [address].
Future<VmService> connectToVmService(String address, {
  ReloadSources reloadSources,
  Restart restart,
  CompileExpression compileExpression,
}) async {
  final VmService vmService = await vmServiceConnectUri(address);
  final List<Future<void>> pendingServiceRegistration = <Future<void>>[];

  if (reloadSources != null) {
    vmService.registerServiceCallback('reloadSources', (Map<String, Object> arguments) async {
      final String isolateId = arguments['isolateId'] as String;
      final bool force = (arguments['force'] as bool) ?? false;
      final bool pause = (arguments['pause'] as bool) ?? false;

      if (isolateId.isEmpty) {
        throw rpc.RpcException.invalidParams('Invalid \'isolateId\': $isolateId');
      }
      try {
        await reloadSources(isolateId, force: force, pause: pause);
        return <String, String>{'type': 'Success'};
      } on rpc.RpcException {
        rethrow;
      } catch (e, st) {
        throw rpc.RpcException(rpc_error_code.SERVER_ERROR,
            'Error during Sources Reload: $e\n$st');
      }
    });
    pendingServiceRegistration.add(vmService.registerService('reloadSources', 'Flutter Tools'));
  }
  if (compileExpression != null) {
    vmService.registerServiceCallback('compileExpression', (Map<String, Object> arguments) async {
      return null;
    });
    pendingServiceRegistration.add(vmService.registerService('compileExpression', 'Flutter Tools'));
  }
  if (restart != null) {
    vmService.registerServiceCallback('hotRestart', (Map<String, Object> arguments) async {
        final bool pause = (arguments['pause'] as bool) ?? false;

        try {
          await restart(pause: pause);
          return <String, String>{'type': 'Success'};
        } on rpc.RpcException {
          rethrow;
        } catch (e, st) {
          throw rpc.RpcException(rpc_error_code.SERVER_ERROR,
              'Error during Hot Restart: $e\n$st');
        }
    });
    pendingServiceRegistration.add(vmService.registerService('hotRestart', 'Flutter Tools'));
  }
  vmService.registerServiceCallback('flutterVersion', (Map<String, Object> arguments) async {
    final FlutterVersion version = FlutterVersion();
    final Map<String, Object> versionJson = version.toJson();
    versionJson['frameworkRevisionShort'] = version.frameworkRevisionShort;
    versionJson['engineRevisionShort'] = version.engineRevisionShort;
    return versionJson;
  });
  pendingServiceRegistration.add(
    vmService.registerService('flutterVersion', 'Flutter Tools'));

  await Future.wait(pendingServiceRegistration);
  return vmService;
}

/// Flutter specific functionality for the VmService.
extension FlutterTools on VmService {

  /// List all active flutter views.
  Future<List<FlutterView>> listFlutterViews() async {
    final Response response = await callMethod('_flutter.listViews');
    print(response.json);
    // TODO(jonahwilliams): what does this return?
    return <FlutterView>[];
  }

  /// Create a new development file system on the vm.
  Future<Map<String, dynamic>> createDevFS(String fsName) async {
    final Response response = await callMethod(
      '_createDevFS',
      args: <String, dynamic>{'fsName': fsName},
    );
    return response.json;
  }

  /// List the development file systems on the device.
  Future<List<String>> listDevFS() async {
    final Response response = await callMethod('_listDevFS');
    return (response.json['fsNames'] as List<Object>).cast<String>();
  }

  /// Write one file into a file system.
  ///
  /// This function base64 encodes the file, do not use on anything larger
  /// than a few KB.
  Future<Map<String, dynamic>> writeDevFSFile(
    String fsName, {
    @required String path,
    @required List<int> fileContents,
  }) async {
    assert(path != null);
    assert(fileContents != null);
    final Response response = await callMethod(
      '_writeDevFSFile',
      args: <String, dynamic>{
        'fsName': fsName,
        'path': path,
        'fileContents': base64.encode(fileContents),
      },
    );
    return response.json;
  }

  /// Read one file from a file system.
  ///
  /// This function base64 decodes the file, do not use on anything larger
  /// than a few KB.
  Future<List<int>> readDevFSFile(String fsName, String path) async {
    final Response response = await callMethod(
      '_readDevFSFile',
      args: <String, dynamic>{
        'fsName': fsName,
        'path': path,
      },
    );
    return base64.decode(response.json['fileContents'] as String);
  }

  /// The complete list of the file system named [fsName].
  Future<List<String>> listDevFSFiles(String fsName) async {
    final Response response = await callMethod(
      '_listDevFSFiles',
      args: <String, dynamic>{'fsName': fsName},
    );
    return (response.json['files'] as List<Object>).cast<String>();
  }

  /// Delete an existing file system.
  Future<Map<String, dynamic>> deleteDevFS(String fsName) async {
    final Response response = await callMethod(
      '_deleteDevFS',
      args: <String, dynamic>{'fsName': fsName},
    );
    return response.json;
  }

  Future<Map<String, dynamic>> reloadSources({
    bool pause = false,
    Uri rootLibUri,
    Uri packagesUri,
  }) async {
    try {
      final Response response = await callMethod('_reloadSources',
        args: <String, Object>{
          'pause': pause,
          if (rootLibUri != null)
            'rootLibUri': rootLibUri.toString(),
          if (packagesUri != null)
            'packagesUri': packagesUri.toString()
      });
      return response.json;
    } on rpc.RpcException catch (e) {
      return Future<Map<String, dynamic>>.value(<String, dynamic>{
        'code': e.code,
        'message': e.message,
        'data': e.data,
      });
    }
  }

  /// Updates the asset directory path of the flutter view [viewId] in
  /// isolate [isolateId].
  Future<void> setAssetDirectory({
    @required Uri assetsDirectory,
    @required String isolateId,
    @required String viewId,
  }) async {
    return callMethod('_flutter.setAssetBundlePath',
      isolateId: isolateId,
      args: <String, Object>{
        'viewId': viewId,
        'assetDirectory': assetsDirectory.toFilePath(windows: false),
      }
    );
  }

  /// Sets whether semantics is enabled/disabled on the flutter view [viewId]
  /// in isolate [isolateId].
  Future<void> setSemanticsEnabled({
    @required bool enabled,
    @required String isolateId,
    @required String viewId,
  }) async {
    return callMethod('_flutter.setSemanticsEnabled',
      isolateId: isolateId,
      args: <String, Object>{
        'viewId': viewId,
        'enabled': enabled,
      }
    );
  }

  /// Flushes the tasks on the ui threa on the flutter view [viewId] in
  /// isolate [isolateId].
  Future<void> flushUIThreadTasks({
    @required String isolateId,
    @required String viewId,
  }) async {
    return callMethod('_flutter.flushUIThreadTasks',
      isolateId: isolateId,
      args: <String, Object>{
        'viewId': viewId,
      }
    );
  }

  /// Run the application entrypoint [main] in the flutter view [viewId] in
  /// isolate [isolateId].
  ///
  /// The [main] URI should be a relative URI from the root of the devfs
  /// created in the vm that is running the isolate.
  Future<void> runInView({
    @required String isolateId,
    @required String viewId,
    @required Uri main,
    Uri packages, // TODO(jonahwilliams): this is no longer used in Dart 2.
    Uri assetsDirectory, // TODO(jonahwilliams): verify how this is used.
  }) {
    return callMethod('_flutter.runInView',
      isolateId: isolateId,
      args: <String, dynamic>{
        'viewId': viewId,
        'mainScript': main.toString(),
        'packagesFile': packages.toString(),
        'assetDirectory': assetsDirectory.toString(),
    });
  }

  // All of the following methods are registered by the flutter application.
  // They do not have access to the isolateId or viewId in Dart, so these
  // lack the ability to specify which view/isolate (if there are multiple)
  // should be chosen.

  Future<Map<String, dynamic>> flutterDebugDumpApp() async {
    final Response response = await callMethod('ext.flutter.debugDumpApp');
    return response.json;
  }

  Future<Map<String, dynamic>> flutterDebugDumpRenderTree() async {
    final Response response = await callMethod('ext.flutter.debugDumpRenderTree');
    return response.json;
  }

  Future<Map<String, dynamic>> flutterDebugDumpLayerTree() async {
    final Response response = await callMethod('ext.flutter.debugDumpLayerTree');
    return response.json;
  }

  Future<Map<String, dynamic>> flutterDebugDumpSemanticsTreeInTraversalOrder() async {
    final Response response = await callMethod('ext.flutter.debugDumpSemanticsTreeInTraversalOrder');
    return response.json;
  }

  Future<Map<String, dynamic>> flutterDebugDumpSemanticsTreeInInverseHitTestOrder() async {
    final Response response = await callMethod('ext.flutter.debugDumpSemanticsTreeInInverseHitTestOrder');
    return response.json;
  }

   Future<Map<String, dynamic>> _flutterToggle(String name) async {
    final Response response = await callMethod('ext.flutter.$name');
    Map<String, Object> state = response.json;
    if (state != null && state.containsKey('enabled') && state['enabled'] is String) {
      final Response response  = await callMethod(
        'ext.flutter.$name',
        args: <String, dynamic>{'enabled': state['enabled'] == 'true' ? 'false' : 'true'},
      );
      state = response.json;
    }
    return state;
  }

  Future<Map<String, dynamic>> flutterToggleDebugPaintSizeEnabled() => _flutterToggle('debugPaint');

  Future<Map<String, dynamic>> flutterToggleDebugCheckElevationsEnabled() => _flutterToggle('debugCheckElevationsEnabled');

  Future<Map<String, dynamic>> flutterTogglePerformanceOverlayOverride() => _flutterToggle('showPerformanceOverlay');

  Future<Map<String, dynamic>> flutterToggleWidgetInspector() => _flutterToggle('inspector.show');

  Future<Map<String, dynamic>> flutterToggleProfileWidgetBuilds() => _flutterToggle('profileWidgetBuilds');

  Future<Map<String, dynamic>> flutterDebugAllowBanner(bool show) async {
    final Response response = await callMethod(
      'ext.flutter.debugAllowBanner',
      args: <String, dynamic>{'enabled': show ? 'true' : 'false'},
    );
    return response.json;
  }

  Future<Map<String, dynamic>> flutterReassemble() async {
    final Response response = await callMethod('ext.flutter.reassemble');
    return response.json;
  }

  Future<bool> flutterAlreadyPaintedFirstUsefulFrame() async {
    final Response response = await callMethod('ext.flutter.didSendFirstFrameRasterizedEvent');
    final Map<String, Object> result = response.json;
    // result might be null when the service extension is not initialized
    return result != null && result['enabled'] == 'true';
  }

  Future<Map<String, dynamic>> uiWindowScheduleFrame() async {
    final Response response = await callMethod('ext.ui.window.scheduleFrame');
    return response.json;
  }

  Future<Map<String, dynamic>> flutterEvictAsset(String assetPath) async {
    final Response response = await callMethod(
      'ext.flutter.evict',
      args: <String, dynamic>{
        'value': assetPath,
      },
    );
    return response.json;
  }

  Future<List<int>> flutterDebugSaveCompilationTrace() async {
    final Response response = await callMethod('ext.flutter.saveCompilationTrace');
    final Map<String, Object> result = response.json;
    if (result != null && result['value'] is List<dynamic>) {
      return (result['value'] as List<dynamic>).cast<int>();
    }
    return null;
  }

  // Application control extension methods.
  Future<Map<String, dynamic>> flutterExit() async {
    final Response response = await callMethod('ext.flutter.exit');
    return response.json;
  }

  Future<String> flutterPlatformOverride([ String platform ]) async {
    final Response response = await callMethod(
      'ext.flutter.platformOverride',
      args: platform != null ? <String, dynamic>{'value': platform} : <String, String>{},
    );
    final Map<String, dynamic> result = response.json;
    if (result != null && result['value'] is String) {
      return result['value'];
    }
    return 'unknown';
  }
}

class FlutterView extends Obj {
  FlutterView._(this.uiIsolate);

  factory FlutterView.parse(Map<String, Object> json) {
    return FlutterView._(json['uiIsolate'] as String)
      ..id = json['id'];
  }

  /// The id of the [Isolate] this flutter view is running in.
  final String uiIsolate;
}
