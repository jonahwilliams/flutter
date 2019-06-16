import 'package:flutter_tools/src/base/file_system.dart';
import 'package:flutter_tools/src/base/io.dart';
import 'package:flutter_tools/src/base/process_manager.dart';

import '../build_system.dart';

Future<void> embedFlutterFrameworksInvocation(
    Map inputs, Environment environment) {}

/// Adds the App.framework as an embedded binary and the flutter_assets as resources.
const Target embedFlutterFrameworks = Target(
  inputs: <Source>[],
  invocation: embedFlutterFrameworksInvocation,
  name: 'embed_flutter_frameworks',
  outputs: <Source>[],
);

Future<void> thinAppFrameworksInvocation(
  Map inputs,
  Environment environment,
) {}

/// Locate the executable file from the framework Plist file.
List<SourceFile> findExecutableFromPlist(Environment environment)  {
  final String frameworkDir = fs.path.join(environment
    .projectDir.path,
    'ios',
    'Flutter',
    'Flutter.framework',
  );
  final String infoPlistPath = fs.path.join(frameworkDir, 'Info.plist');
  final ProcessResult result = processManager.runSync(<String>[
      'default', 'read', infoPlistPath, 'CFBundleExecutable']);
  if (result.exitCode != 0) {
    throw Exception('Failed to read iInfo.plist for CFBundleExecutable at $infoPlistPath');
  }
  final String relativeExecutable = result.stdout.trim();
  return <SourceFile>[
    SourceFile(fs.file(fs.path.join(frameworkDir, relativeExecutable))),
  ];
}

/// Destructively thins the specified framework to include only the specified architectures.
const Target thinAppFrameworks = Target(
  inputs: <Source>[
    Source.pattern('{PROJECT_ROOT}/ios/Flutter/Flutter.framework/Info.plist'),
    Source.function(findExecutableFromPlist),
  ],
  invocation: thinAppFrameworksInvocation,
  name: 'thin_app_frameworks',
  outputs: <Source>[],
);
