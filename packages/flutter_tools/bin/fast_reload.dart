import 'dart:convert';
import 'dart:io';

import 'package:flutter_tools/src/compile.dart';
import 'package:flutter_tools/src/context_runner.dart';
import 'package:flutter_tools/src/vmservice.dart';
import 'package:watcher/watcher.dart';

final HttpClient client = HttpClient();

void main(List<String> args) async {
  await runInContext(() async {
    final int port = int.parse(args[0]);

    final residentCompiler = ResidentCompiler(
      '/Users/jonahwilliams/Documents/flutter/bin/cache/artifacts/engine/common/flutter_patched_sdk/',
      packagesPath:
          '/Users/jonahwilliams/Documents/flutter/examples/flutter_gallery/.packages',
    );
    final vmservice =
        await VMService.connect(Uri.parse('http://127.0.0.1:$port'));
    final outputPath = await residentCompiler
        .recompile('lib/main.dart', <String>[], outputPath: 'build/app.dill');
    residentCompiler.accept();
    await vmservice.getVM();
    await vmservice.refreshViews();
    final List<String> dirtyCode = <String>[];

    DirectoryWatcher('lib').events.listen((WatchEvent event) {
      if (event.type == ChangeType.REMOVE) {
        return;
      }
      if (event.path.endsWith('.dart')) {
        dirtyCode.add(event.path);
      }
    });

    Map<String, Object> result;
    try {
      result = await vmservice.vm.createDevFS('flutter_gallery');
    } catch (_) {
      await vmservice.vm.deleteDevFS('flutter_gallery');
      result = await vmservice.vm.createDevFS('flutter_gallery');
    }
    final String base = result['uri'];
    print("READY");
    stdin
        .transform(utf8.decoder)
        .transform(const LineSplitter())
        .listen((String line) async {
      if (line.trim() != 'r') {
        return;
      }
      var now = DateTime.now().millisecondsSinceEpoch;
      final dirtyCode2 = List<String>.from(dirtyCode);
      dirtyCode.clear();
      await residentCompiler.recompile('lib/main.dart', dirtyCode2,
          outputPath: 'build/app.dill');
      print('COMPILE: ${DateTime.now().millisecondsSinceEpoch - now}');
      await syncDill(Uri.parse('http://127.0.0.1:$port/'),
          File(outputPath.outputFilename));
      print('SYNC: ${DateTime.now().millisecondsSinceEpoch - now}');
      for (var view in vmservice.vm.views) {
        await view.uiIsolate.reloadSources(
          pause: false,
          rootLibUri: '${base}lib/main.dart.incremental.dill',
          packagesUri: '$base.packages',
        );
        print('RELOAD: ${DateTime.now().millisecondsSinceEpoch - now}');
        await view.uiIsolate.flutterReassemble();
        print('REASSMEBLE: ${DateTime.now().millisecondsSinceEpoch - now}');
      }
      print('DONE: ${DateTime.now().millisecondsSinceEpoch - now}');
    });
  });
}

Future<void> syncDill(Uri httpAddress, File file) async {
  final HttpClientRequest request = await client.putUrl(httpAddress);
  request.headers.removeAll(HttpHeaders.acceptEncodingHeader);
  request.headers.add('dev_fs_name', 'flutter_gallery');
  request.headers.add('dev_fs_uri_b64',
      base64.encode(utf8.encode('lib/main.dart.incremental.dill')));
  final bytes = file.readAsBytesSync();
  await request.add(gzip.encode(bytes));
  final HttpClientResponse response = await request.close();
  await response.drain<void>();
}
