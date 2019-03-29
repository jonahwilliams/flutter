import 'package:build/build.dart';
import 'package:build_web_compilers/build_web_compilers.dart';

/// Dev compiler builder.
Builder devCompilerBuilder(BuilderOptions builderOptions) {
  final String platformSdk = builderOptions.config['platformSdk'];
  return DevCompilerBuilder(
    platformSdk: platformSdk,
    useKernel: false,
  );
}

/// Web entrypoint builder.
Builder webEntrypointBuilder(BuilderOptions options) {
  return const WebEntrypointBuilder(WebCompiler.DartDevc);
}

/// Extractor for dart archive files.
PostProcessBuilder dart2JsArchiveExtractor(BuilderOptions options) =>
    Dart2JsArchiveExtractor.fromOptions(options);

/// Cleanup for temporary dart files.
PostProcessBuilder dartSourceCleanup(BuilderOptions options) {
  return (options.config['enabled'] ?? false)
      ? const FileDeletingBuilder(<String>['.dart', '.js.map'])
      : const FileDeletingBuilder(<String>['.dart', '.js.map'], isEnabled: false);
}
