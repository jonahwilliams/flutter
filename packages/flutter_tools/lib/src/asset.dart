// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:typed_data';

import 'package:meta/meta.dart';
import 'package:package_config/package_config.dart';
import 'package:typed_data/typed_data.dart';
import 'package:yaml/yaml.dart';

import 'base/context.dart';
import 'base/file_system.dart';
import 'base/utils.dart';
import 'build_info.dart';
import 'cache.dart';
import 'convert.dart';
import 'dart/package_map.dart';
import 'devfs.dart';
import 'flutter_manifest.dart';
import 'globals.dart' as globals;
import 'project.dart';

const AssetBundleFactory _kManifestFactory = _ManifestAssetBundleFactory();

const String defaultManifestPath = 'pubspec.yaml';

const String kFontManifestJson = 'FontManifest.json';

/// Injected factory class for spawning [AssetBundle] instances.
abstract class AssetBundleFactory {
  /// The singleton instance, pulled from the [AppContext].
  static AssetBundleFactory get instance => context.get<AssetBundleFactory>();

  static AssetBundleFactory get defaultInstance => _kManifestFactory;

  /// Creates a new [AssetBundle].
  AssetBundle createBundle();
}

abstract class AssetBundle {
  Map<String, DevFSContent> get entries;

  /// Additional files that this bundle depends on that are not included in the
  /// output result.
  List<File> get additionalDependencies;

  bool wasBuiltOnce();

  bool needsBuild({ String manifestPath = defaultManifestPath });

  /// Returns 0 for success; non-zero for failure.
  Future<int> build({
    String manifestPath = defaultManifestPath,
    String assetDirPath,
    @required String packagesPath,
    bool includeDefaultFonts = true,
    bool reportLicensedPackages = false,
  });
}

class _ManifestAssetBundleFactory implements AssetBundleFactory {
  const _ManifestAssetBundleFactory();

  @override
  AssetBundle createBundle() => ManifestAssetBundle();
}

/// An asset bundle based on a pubspec.yaml file.
class ManifestAssetBundle implements AssetBundle {
  /// Constructs an [ManifestAssetBundle] that gathers the set of assets from the
  /// pubspec.yaml manifest.
  ManifestAssetBundle();

  @override
  final Map<String, DevFSContent> entries = <String, DevFSContent>{};

  // If an asset corresponds to a wildcard directory, then it may have been
  // updated without changes to the manifest. These are only tracked for
  // the current project.
  final Map<Uri, Directory> _wildcardDirectories = <Uri, Directory>{};

  final LicenseCollector licenseCollector = LicenseCollector(fileSystem: globals.fs);

  DateTime _lastBuildTimestamp;

  static const String _kAssetManifestBin = 'AssetManifest.bin';
  static const String _kAssetManifestJson = 'AssetManifest.json';
  static const String _kFontSetMaterial = 'material';
  static const String _kNoticeFile = 'NOTICES';

  @override
  bool wasBuiltOnce() => _lastBuildTimestamp != null;

  @override
  bool needsBuild({ String manifestPath = defaultManifestPath }) {
    if (_lastBuildTimestamp == null) {
      return true;
    }

    final FileStat stat = globals.fs.file(manifestPath).statSync();
    if (stat.type == FileSystemEntityType.notFound) {
      return true;
    }

    for (final Directory directory in _wildcardDirectories.values) {
      if (!directory.existsSync()) {
        return true; // directory was deleted.
      }
      for (final File file in directory.listSync().whereType<File>()) {
        final DateTime dateTime = file.statSync().modified;
        if (dateTime == null) {
          continue;
        }
        if (dateTime.isAfter(_lastBuildTimestamp)) {
          return true;
        }
      }
    }

    return stat.modified.isAfter(_lastBuildTimestamp);
  }

  @override
  Future<int> build({
    String manifestPath = defaultManifestPath,
    String assetDirPath,
    @required String packagesPath,
    bool includeDefaultFonts = true,
    bool reportLicensedPackages = false,
  }) async {
    assetDirPath ??= getAssetBuildDirectory();
    FlutterProject flutterProject;
    try {
      flutterProject = FlutterProject.fromDirectory(globals.fs.file(manifestPath).parent);
    } on Exception catch (e) {
      globals.printStatus('Error detected in pubspec.yaml:', emphasis: true);
      globals.printError('$e');
      return 1;
    }
    if (flutterProject == null) {
      return 1;
    }
    final FlutterManifest flutterManifest = flutterProject.manifest;
    // If the last build time isn't set before this early return, empty pubspecs will
    // hang on hot reload, as the incremental dill files will never be copied to the
    // device.
    _lastBuildTimestamp = DateTime.now();
    if (flutterManifest.isEmpty) {
      entries[_kAssetManifestJson] = DevFSStringContent('{}');
      entries[_kAssetManifestBin] = DevFSByteContent(<int>[]);
      return 0;
    }

    final String assetBasePath = globals.fs.path.dirname(globals.fs.path.absolute(manifestPath));
    final PackageConfig packageConfig = await loadPackageConfigWithLogging(
      globals.fs.file(packagesPath),
      logger: globals.logger,
    );
    final List<Uri> wildcardDirectories = <Uri>[];

    // The _assetVariants map contains an entry for each asset listed
    // in the pubspec.yaml file's assets and font and sections. The
    // value of each image asset is a list of resolution-specific "variants",
    // see _AssetDirectoryCache.
    final Map<_Asset, List<_Asset>> assetVariants = _parseAssets(
      packageConfig,
      flutterManifest,
      wildcardDirectories,
      assetBasePath,
      excludeDirs: <String>[
        assetDirPath,
        getBuildDirectory(),
        if (flutterProject.ios.existsSync())
          flutterProject.ios.hostAppRoot.path,
        if (flutterProject.macos.existsSync())
          flutterProject.macos.managedDirectory.path,
        if (flutterProject.windows.existsSync())
          flutterProject.windows.managedDirectory.path,
        if (flutterProject.linux.existsSync())
          flutterProject.linux.managedDirectory.path,
      ],
    );

    if (assetVariants == null) {
      return 1;
    }

    final bool includesMaterialFonts = flutterManifest.usesMaterialDesign;
    final List<Map<String, dynamic>> fonts = _parseFonts(
      flutterManifest,
      includeDefaultFonts,
      packageConfig,
      primary: true,
    );

    // Add fonts and assets from packages.
    for (final Package package in packageConfig.packages) {
      final Uri packageUri = package.packageUriRoot;
      if (packageUri != null && packageUri.scheme == 'file') {
        final String packageManifestPath = globals.fs.path.fromUri(packageUri.resolve('../pubspec.yaml'));
        final FlutterManifest packageFlutterManifest = FlutterManifest.createFromPath(
          packageManifestPath,
          logger: globals.logger,
          fileSystem: globals.fs,
        );
        if (packageFlutterManifest == null) {
          continue;
        }
        // Skip the app itself
        if (packageFlutterManifest.appName == flutterManifest.appName) {
          continue;
        }
        final String packageBasePath = globals.fs.path.dirname(packageManifestPath);

        final Map<_Asset, List<_Asset>> packageAssets = _parseAssets(
          packageConfig,
          packageFlutterManifest,
          // Do not track wildcard directories for dependencies.
          <Uri>[],
          packageBasePath,
          packageName: package.name,
          attributedPackage: package,
        );

        if (packageAssets == null) {
          return 1;
        }
        assetVariants.addAll(packageAssets);
        if (!includesMaterialFonts && packageFlutterManifest.usesMaterialDesign) {
          globals.printError(
            'package:${package.name} has `uses-material-design: true` set but '
            'the primary pubspec contains `uses-material-design: false`. '
            'If the application needs material icons, then `uses-material-design` '
            ' must be set to true.'
          );
        }
        fonts.addAll(_parseFonts(
          packageFlutterManifest,
          includeDefaultFonts,
          packageConfig,
          packageName: package.name,
          primary: false,
        ));
      }
    }

    // Save the contents of each image, image variant, and font
    // asset in entries.
    for (final _Asset asset in assetVariants.keys) {
      if (!asset.assetFileExists && assetVariants[asset].isEmpty) {
        globals.printStatus('Error detected in pubspec.yaml:', emphasis: true);
        globals.printError('No file or variants found for $asset.\n');
        if (asset.package != null) {
          globals.printError('This asset was included from package ${asset.package.name}.');
        }
        return 1;
      }
      // The file name for an asset's "main" entry is whatever appears in
      // the pubspec.yaml file. The main entry's file must always exist for
      // font assets. It need not exist for an image if resolution-specific
      // variant files exist. An image's main entry is treated the same as a
      // "1x" resolution variant and if both exist then the explicit 1x
      // variant is preferred.
      if (asset.assetFileExists) {
        assert(!assetVariants[asset].contains(asset));
        assetVariants[asset].insert(0, asset);
      }
      for (final _Asset variant in assetVariants[asset]) {
        assert(variant.assetFileExists);
        entries[variant.entryUri.path] ??= DevFSFileContent(variant.assetFile);
      }
    }
    final List<_Asset> materialAssets = <_Asset>[
      if (flutterManifest.usesMaterialDesign && includeDefaultFonts)
        ..._getMaterialAssets(_kFontSetMaterial),
    ];
    for (final _Asset asset in materialAssets) {
      assert(asset.assetFileExists);
      entries[asset.entryUri.path] ??= DevFSFileContent(asset.assetFile);
    }

    // Update wildcard directories we we can detect changes in them.
    for (final Uri uri in wildcardDirectories) {
      _wildcardDirectories[uri] ??= globals.fs.directory(uri);
    }

    final DevFSStringContent assetManifest  = _createAssetManifest(assetVariants);
    final DevFSByteContent assetBinaryManifest = _createAssetBinaryManifest(assetVariants);
    final DevFSStringContent fontManifest = DevFSStringContent(json.encode(fonts));
    final LicenseResult licenseResult = licenseCollector.obtainLicenses(packageConfig);
    final DevFSStringContent licenses = DevFSStringContent(licenseResult.combinedLicenses);
    additionalDependencies = licenseResult.dependencies;

    if (wildcardDirectories.isNotEmpty) {
      // Force the depfile to contain missing files so that Gradle does not skip
      // the task. Wildcard directories are not compatible with full incremental
      // builds. For more context see https://github.com/flutter/flutter/issues/56466 .
      globals.printTrace(
        'Manifest contained wildcard assets. Inserting missing file into '
        'build graph to force rerun. for more information see #56466.'
      );
      final int suffix = Object().hashCode;
      additionalDependencies.add(
        globals.fs.file('DOES_NOT_EXIST_RERUN_FOR_WILDCARD$suffix').absolute);
    }

    _setIfChanged(_kAssetManifestJson, assetManifest);
    _setIfChanged(_kAssetManifestBin, assetBinaryManifest);
    _setIfChanged(kFontManifestJson, fontManifest);
    _setIfChanged(_kNoticeFile, licenses);
    return 0;
  }

  @override
  List<File> additionalDependencies = <File>[];

  void _setIfChanged(String key, DevFSContent content) {
    if (!entries.containsKey(key)) {
      entries[key] = content;
      return;
    }
    final DevFSContent oldContent = entries[key] as DevFSStringContent;
    if (oldContent is DevFSStringContent && oldContent.string != (content as DevFSStringContent).string) {
      entries[key] = content;
    } else if (oldContent is DevFSByteContent && !listEquals(oldContent.bytes, (content as DevFSByteContent).bytes)) {
      entries[key] = content;
    } else {
      entries[key] = content;
    }
  }
}

@immutable
class _Asset {
  const _Asset({ this.baseDir, this.relativeUri, this.entryUri, @required this.package });

  final String baseDir;

  final Package package;

  /// A platform-independent URL where this asset can be found on disk on the
  /// host system relative to [baseDir].
  final Uri relativeUri;

  /// A platform-independent URL representing the entry for the asset manifest.
  final Uri entryUri;

  File get assetFile {
    return globals.fs.file(globals.fs.path.join(baseDir, globals.fs.path.fromUri(relativeUri)));
  }

  bool get assetFileExists => assetFile.existsSync();

  /// The delta between what the entryUri is and the relativeUri (e.g.,
  /// packages/flutter_gallery).
  Uri get symbolicPrefixUri {
    if (entryUri == relativeUri) {
      return null;
    }
    final int index = entryUri.path.indexOf(relativeUri.path);
    return index == -1 ? null : Uri(path: entryUri.path.substring(0, index));
  }

  @override
  String toString() => 'asset: $entryUri';

  @override
  bool operator ==(Object other) {
    if (identical(other, this)) {
      return true;
    }
    if (other.runtimeType != runtimeType) {
      return false;
    }
    return other is _Asset
        && other.baseDir == baseDir
        && other.relativeUri == relativeUri
        && other.entryUri == entryUri;
  }

  @override
  int get hashCode {
    return baseDir.hashCode
        ^ relativeUri.hashCode
        ^ entryUri.hashCode;
  }
}

Map<String, dynamic> _readMaterialFontsManifest() {
  final String fontsPath = globals.fs.path.join(globals.fs.path.absolute(Cache.flutterRoot),
      'packages', 'flutter_tools', 'schema', 'material_fonts.yaml');

  return castStringKeyedMap(loadYaml(globals.fs.file(fontsPath).readAsStringSync()));
}

final Map<String, dynamic> _materialFontsManifest = _readMaterialFontsManifest();

List<Map<String, dynamic>> _getMaterialFonts(String fontSet) {
  final List<dynamic> fontsList = _materialFontsManifest[fontSet] as List<dynamic>;
  return fontsList?.map<Map<String, dynamic>>(castStringKeyedMap)?.toList();
}

List<_Asset> _getMaterialAssets(String fontSet) {
  final List<_Asset> result = <_Asset>[];

  for (final Map<String, dynamic> family in _getMaterialFonts(fontSet)) {
    for (final Map<dynamic, dynamic> font in (family['fonts'] as List<dynamic>).cast<Map<dynamic, dynamic>>()) {
      final Uri entryUri = globals.fs.path.toUri(font['asset'] as String);
      result.add(_Asset(
        baseDir: globals.fs.path.join(Cache.flutterRoot, 'bin', 'cache', 'artifacts', 'material_fonts'),
        relativeUri: Uri(path: entryUri.pathSegments.last),
        entryUri: entryUri,
        package: null,
      ));
    }
  }

  return result;
}


/// Processes dependencies into a string representing the NOTICES file.
///
/// Reads the NOTICES or LICENSE file from each package in the .packages file,
/// splitting each one into each component license so that it can be de-duped
/// if possible. If the NOTICES file exists, it is preferred over the LICENSE
/// file.
///
/// Individual licenses inside each LICENSE file should be separated by 80
/// hyphens on their own on a line.
///
/// If a LICENSE or NOTICES file contains more than one component license,
/// then each component license must start with the names of the packages to
/// which the component license applies, with each package name on its own line
/// and the list of package names separated from the actual license text by a
/// blank line. The packages need not match the names of the pub package. For
/// example, a package might itself contain code from multiple third-party
/// sources, and might need to include a license for each one.
class LicenseCollector {
  LicenseCollector({
    @required FileSystem fileSystem
  }) : _fileSystem = fileSystem;

  final FileSystem _fileSystem;

  /// The expected separator for multiple licenses.
  static final String licenseSeparator = '\n' + ('-' * 80) + '\n';

  /// Obtain licenses from the `packageMap` into a single result.
  LicenseResult obtainLicenses(
    PackageConfig packageConfig,
  ) {
    final Map<String, Set<String>> packageLicenses = <String, Set<String>>{};
    final Set<String> allPackages = <String>{};
    final List<File> dependencies = <File>[];

    for (final Package package in packageConfig.packages) {
      final Uri packageUri = package.packageUriRoot;
      if (packageUri == null || packageUri.scheme != 'file') {
        continue;
      }
      // First check for NOTICES, then fallback to LICENSE
      File file = _fileSystem.file(packageUri.resolve('../NOTICES'));
      if (!file.existsSync()) {
        file = _fileSystem.file(packageUri.resolve('../LICENSE'));
      }
      if (!file.existsSync()) {
        continue;
      }

      dependencies.add(file);
      final List<String> rawLicenses = file
        .readAsStringSync()
        .split(licenseSeparator);
      for (final String rawLicense in rawLicenses) {
        List<String> packageNames;
        String licenseText;
        if (rawLicenses.length > 1) {
          final int split = rawLicense.indexOf('\n\n');
          if (split >= 0) {
            packageNames = rawLicense.substring(0, split).split('\n');
            licenseText = rawLicense.substring(split + 2);
          }
        }
        if (licenseText == null) {
          packageNames = <String>[package.name];
          licenseText = rawLicense;
        }
        packageLicenses.putIfAbsent(licenseText, () => <String>{})
          .addAll(packageNames);
        allPackages.addAll(packageNames);
      }
    }
    final List<String> combinedLicensesList = packageLicenses.keys
      .map<String>((String license) {
        final List<String> packageNames = packageLicenses[license].toList()
          ..sort();
        return packageNames.join('\n') + '\n\n' + license;
      }).toList();
    combinedLicensesList.sort();
    final String combinedLicenses = combinedLicensesList.join(licenseSeparator);

    return LicenseResult(
      combinedLicenses: combinedLicenses,
      dependencies: dependencies,
    );
  }
}

/// The result of processing licenses with a [LicenseCollector].
class LicenseResult {
  const LicenseResult({
    @required this.combinedLicenses,
    @required this.dependencies,
  });

  /// The raw text of the consumed licenses.
  final String combinedLicenses;

  /// Each license file that was consumed as input.
  final List<File> dependencies;
}

int _byBasename(_Asset a, _Asset b) {
  return a.assetFile.basename.compareTo(b.assetFile.basename);
}

DevFSStringContent _createAssetManifest(Map<_Asset, List<_Asset>> assetVariants) {
  final Map<String, List<String>> jsonObject = <String, List<String>>{};

  // necessary for making unit tests deterministic
  final List<_Asset> sortedKeys = assetVariants
      .keys.toList()
    ..sort(_byBasename);

  for (final _Asset main in sortedKeys) {
    jsonObject[main.entryUri.path] = <String>[
      for (final _Asset variant in assetVariants[main])
        variant.entryUri.path,
    ];
  }
  return DevFSStringContent(json.encode(jsonObject));
}

DevFSByteContent _createAssetBinaryManifest(Map<_Asset, List<_Asset>> assetVariants) {
  const StandardMessageCodec codec = StandardMessageCodec();
  final Map<String, List<String>> jsonObject = <String, List<String>>{};

  // necessary for making unit tests deterministic
  final List<_Asset> sortedKeys = assetVariants
      .keys.toList()
    ..sort(_byBasename);

  for (final _Asset main in sortedKeys) {
    jsonObject[main.entryUri.path] = <String>[
      for (final _Asset variant in assetVariants[main])
        variant.entryUri.path,
    ];
  }
  final ByteData result = codec.encodeMessage(jsonObject);
  return DevFSByteContent(result.buffer.asUint8List());
}

List<Map<String, dynamic>> _parseFonts(
  FlutterManifest manifest,
  bool includeDefaultFonts,
  PackageConfig packageConfig, {
  String packageName,
  @required bool primary,
}) {
  return <Map<String, dynamic>>[
    if (primary && manifest.usesMaterialDesign && includeDefaultFonts)
      ..._getMaterialFonts(ManifestAssetBundle._kFontSetMaterial),
    if (packageName == null)
      ...manifest.fontsDescriptor
    else
      ..._createFontsDescriptor(_parsePackageFonts(
        manifest,
        packageName,
        packageConfig,
      )),
  ];
}

/// Prefixes family names and asset paths of fonts included from packages with
/// 'packages/<package_name>'
List<Font> _parsePackageFonts(
  FlutterManifest manifest,
  String packageName,
  PackageConfig packageConfig,
) {
  final List<Font> packageFonts = <Font>[];
  for (final Font font in manifest.fonts) {
    final List<FontAsset> packageFontAssets = <FontAsset>[];
    for (final FontAsset fontAsset in font.fontAssets) {
      final Uri assetUri = fontAsset.assetUri;
      if (assetUri.pathSegments.first == 'packages' &&
          !globals.fs.isFileSync(globals.fs.path.fromUri(
            packageConfig[packageName].packageUriRoot.resolve('../${assetUri.path}')))) {
        packageFontAssets.add(FontAsset(
          fontAsset.assetUri,
          weight: fontAsset.weight,
          style: fontAsset.style,
        ));
      } else {
        packageFontAssets.add(FontAsset(
          Uri(pathSegments: <String>['packages', packageName, ...assetUri.pathSegments]),
          weight: fontAsset.weight,
          style: fontAsset.style,
        ));
      }
    }
    packageFonts.add(Font('packages/$packageName/${font.familyName}', packageFontAssets));
  }
  return packageFonts;
}

List<Map<String, dynamic>> _createFontsDescriptor(List<Font> fonts) {
  return fonts.map<Map<String, dynamic>>((Font font) => font.descriptor).toList();
}

// Given an assets directory like this:
//
// assets/foo
// assets/var1/foo
// assets/var2/foo
// assets/bar
//
// variantsFor('assets/foo') => ['/assets/var1/foo', '/assets/var2/foo']
// variantsFor('assets/bar') => []
class _AssetDirectoryCache {
  _AssetDirectoryCache(Iterable<String> excluded)
    : _excluded = excluded
        .map<String>(globals.fs.path.absolute)
        .toList();

  final List<String> _excluded;
  final Map<String, Map<String, List<String>>> _cache = <String, Map<String, List<String>>>{};

  List<String> variantsFor(String assetPath) {
    final String assetName = globals.fs.path.basename(assetPath);
    final String directory = globals.fs.path.dirname(assetPath);

    if (!globals.fs.directory(directory).existsSync()) {
      return const <String>[];
    }

    if (_cache[directory] == null) {
      final List<String> paths = <String>[];
      for (final FileSystemEntity entity in globals.fs.directory(directory).listSync(recursive: true)) {
        final String path = entity.path;
        if (globals.fs.isFileSync(path)
          && assetPath != path
          && !_excluded.any((String exclude) => globals.fs.path.isWithin(exclude, path))) {
          paths.add(path);
        }
      }

      final Map<String, List<String>> variants = <String, List<String>>{};
      for (final String path in paths) {
        final String variantName = globals.fs.path.basename(path);
        if (directory == globals.fs.path.dirname(path)) {
          continue;
        }
        variants[variantName] ??= <String>[];
        variants[variantName].add(path);
      }
      _cache[directory] = variants;
    }

    return _cache[directory][assetName] ?? const <String>[];
  }
}

/// Given an assetBase location and a pubspec.yaml Flutter manifest, return a
/// map of assets to asset variants.
///
/// Returns null on missing assets.
///
/// Given package: 'test_package' and an assets directory like this:
///
/// - assets/foo
/// - assets/var1/foo
/// - assets/var2/foo
/// - assets/bar
///
/// This will return:
/// ```
/// {
///   asset: packages/test_package/assets/foo: [
///     asset: packages/test_package/assets/foo,
///     asset: packages/test_package/assets/var1/foo,
///     asset: packages/test_package/assets/var2/foo,
///   ],
///   asset: packages/test_package/assets/bar: [
///     asset: packages/test_package/assets/bar,
///   ],
/// }
/// ```

Map<_Asset, List<_Asset>> _parseAssets(
  PackageConfig packageConfig,
  FlutterManifest flutterManifest,
  List<Uri> wildcardDirectories,
  String assetBase, {
  List<String> excludeDirs = const <String>[],
  String packageName,
  Package attributedPackage,
}) {
  final Map<_Asset, List<_Asset>> result = <_Asset, List<_Asset>>{};

  final _AssetDirectoryCache cache = _AssetDirectoryCache(excludeDirs);
  for (final Uri assetUri in flutterManifest.assets) {
    if (assetUri.toString().endsWith('/')) {
      wildcardDirectories.add(assetUri);
      _parseAssetsFromFolder(
        packageConfig,
        flutterManifest,
        assetBase,
        cache,
        result,
        assetUri,
        excludeDirs: excludeDirs,
        packageName: packageName,
        attributedPackage: attributedPackage,
      );
    } else {
      _parseAssetFromFile(
        packageConfig,
        flutterManifest,
        assetBase,
        cache,
        result,
        assetUri,
        excludeDirs: excludeDirs,
        packageName: packageName,
        attributedPackage: attributedPackage,
      );
    }
  }

  // Add assets referenced in the fonts section of the manifest.
  for (final Font font in flutterManifest.fonts) {
    for (final FontAsset fontAsset in font.fontAssets) {
      final _Asset baseAsset = _resolveAsset(
        packageConfig,
        assetBase,
        fontAsset.assetUri,
        packageName,
        attributedPackage,
      );
      if (!baseAsset.assetFileExists) {
        globals.printError('Error: unable to locate asset entry in pubspec.yaml: "${fontAsset.assetUri}".');
        return null;
      }

      result[baseAsset] = <_Asset>[];
    }
  }

  return result;
}

void _parseAssetsFromFolder(
  PackageConfig packageConfig,
  FlutterManifest flutterManifest,
  String assetBase,
  _AssetDirectoryCache cache,
  Map<_Asset, List<_Asset>> result,
  Uri assetUri, {
  List<String> excludeDirs = const <String>[],
  String packageName,
  Package attributedPackage,
}) {
  final String directoryPath = globals.fs.path.join(
      assetBase, assetUri.toFilePath(windows: globals.platform.isWindows));

  if (!globals.fs.directory(directoryPath).existsSync()) {
    globals.printError('Error: unable to find directory entry in pubspec.yaml: $directoryPath');
    return;
  }

  final List<FileSystemEntity> lister = globals.fs.directory(directoryPath).listSync();

  for (final FileSystemEntity entity in lister) {
    if (entity is File) {
      final String relativePath = globals.fs.path.relative(entity.path, from: assetBase);
      final Uri uri = Uri.file(relativePath, windows: globals.platform.isWindows);

      _parseAssetFromFile(
        packageConfig,
        flutterManifest,
        assetBase,
        cache,
        result,
        uri,
        packageName: packageName,
        attributedPackage: attributedPackage,
      );
    }
  }
}

void _parseAssetFromFile(
  PackageConfig packageConfig,
  FlutterManifest flutterManifest,
  String assetBase,
  _AssetDirectoryCache cache,
  Map<_Asset, List<_Asset>> result,
  Uri assetUri, {
  List<String> excludeDirs = const <String>[],
  String packageName,
  Package attributedPackage,
}) {
  final _Asset asset = _resolveAsset(
    packageConfig,
    assetBase,
    assetUri,
    packageName,
    attributedPackage,
  );
  final List<_Asset> variants = <_Asset>[];
  for (final String path in cache.variantsFor(asset.assetFile.path)) {
    final String relativePath = globals.fs.path.relative(path, from: asset.baseDir);
    final Uri relativeUri = globals.fs.path.toUri(relativePath);
    final Uri entryUri = asset.symbolicPrefixUri == null
        ? relativeUri
        : asset.symbolicPrefixUri.resolveUri(relativeUri);

    variants.add(
      _Asset(
        baseDir: asset.baseDir,
        entryUri: entryUri,
        relativeUri: relativeUri,
        package: attributedPackage,
      ),
    );
  }

  result[asset] = variants;
}

_Asset _resolveAsset(
  PackageConfig packageConfig,
  String assetsBaseDir,
  Uri assetUri,
  String packageName,
  Package attributedPackage,
) {
  final String assetPath = globals.fs.path.fromUri(assetUri);
  if (assetUri.pathSegments.first == 'packages' && !globals.fs.isFileSync(globals.fs.path.join(assetsBaseDir, assetPath))) {
    // The asset is referenced in the pubspec.yaml as
    // 'packages/PACKAGE_NAME/PATH/TO/ASSET .
    final _Asset packageAsset = _resolvePackageAsset(
      assetUri,
      packageConfig,
      attributedPackage,
    );
    if (packageAsset != null) {
      return packageAsset;
    }
  }

  return _Asset(
    baseDir: assetsBaseDir,
    entryUri: packageName == null
        ? assetUri // Asset from the current application.
        : Uri(pathSegments: <String>['packages', packageName, ...assetUri.pathSegments]), // Asset from, and declared in $packageName.
    relativeUri: assetUri,
    package: attributedPackage,
  );
}

_Asset _resolvePackageAsset(Uri assetUri, PackageConfig packageConfig, Package attributedPackage) {
  assert(assetUri.pathSegments.first == 'packages');
  if (assetUri.pathSegments.length > 1) {
    final String packageName = assetUri.pathSegments[1];
    final Package package = packageConfig[packageName];
    final Uri packageUri = package?.packageUriRoot;
    if (packageUri != null && packageUri.scheme == 'file') {
      return _Asset(
        baseDir: globals.fs.path.fromUri(packageUri),
        entryUri: assetUri,
        relativeUri: Uri(pathSegments: assetUri.pathSegments.sublist(2)),
        package: attributedPackage,
      );
    }
  }
  globals.printStatus('Error detected in pubspec.yaml:', emphasis: true);
  globals.printError('Could not resolve package for asset $assetUri.\n');
  if (attributedPackage != null) {
    globals.printError('This asset was included from package ${attributedPackage.name}');
  }
  return null;
}

class StandardMessageCodec {
  /// Creates a [MessageCodec] using the Flutter standard binary encoding.
  const StandardMessageCodec();

  // The codec serializes messages as outlined below. This format must match the
  // Android and iOS counterparts and cannot change (as it's possible for
  // someone to end up using this for persistent storage).
  //
  // * A single byte with one of the constant values below determines the
  //   type of the value.
  // * The serialization of the value itself follows the type byte.
  // * Numbers are represented using the host endianness throughout.
  // * Lengths and sizes of serialized parts are encoded using an expanding
  //   format optimized for the common case of small non-negative integers:
  //   * values 0..253 inclusive using one byte with that value;
  //   * values 254..2^16 inclusive using three bytes, the first of which is
  //     254, the next two the usual unsigned representation of the value;
  //   * values 2^16+1..2^32 inclusive using five bytes, the first of which is
  //     255, the next four the usual unsigned representation of the value.
  // * null, true, and false have empty serialization; they are encoded directly
  //   in the type byte (using _valueNull, _valueTrue, _valueFalse)
  // * Integers representable in 32 bits are encoded using 4 bytes two's
  //   complement representation.
  // * Larger integers are encoded using 8 bytes two's complement
  //   representation.
  // * doubles are encoded using the IEEE 754 64-bit double-precision binary
  //   format. Zero bytes are added before the encoded double value to align it
  //   to a 64 bit boundary in the full message.
  // * Strings are encoded using their UTF-8 representation. First the length
  //   of that in bytes is encoded using the expanding format, then follows the
  //   UTF-8 encoding itself.
  // * Uint8Lists, Int32Lists, Int64Lists, and Float64Lists are encoded by first
  //   encoding the list's element count in the expanding format, then the
  //   smallest number of zero bytes needed to align the position in the full
  //   message with a multiple of the number of bytes per element, then the
  //   encoding of the list elements themselves, end-to-end with no additional
  //   type information, using two's complement or IEEE 754 as applicable.
  // * Lists are encoded by first encoding their length in the expanding format,
  //   then follows the recursive encoding of each element value, including the
  //   type byte (Lists are assumed to be heterogeneous).
  // * Maps are encoded by first encoding their length in the expanding format,
  //   then follows the recursive encoding of each key/value pair, including the
  //   type byte for both (Maps are assumed to be heterogeneous).
  //
  // The type labels below must not change, since it's possible for this interface
  // to be used for persistent storage.
  static const int _valueNull = 0;
  static const int _valueTrue = 1;
  static const int _valueFalse = 2;
  static const int _valueInt32 = 3;
  static const int _valueInt64 = 4;
  static const int _valueLargeInt = 5;
  static const int _valueFloat64 = 6;
  static const int _valueString = 7;
  static const int _valueUint8List = 8;
  static const int _valueInt32List = 9;
  static const int _valueInt64List = 10;
  static const int _valueFloat64List = 11;
  static const int _valueList = 12;
  static const int _valueMap = 13;

  ByteData encodeMessage(dynamic message) {
    if (message == null) {
      return null;
    }
    final WriteBuffer buffer = WriteBuffer();
    writeValue(buffer, message);
    return buffer.done();
  }

  dynamic decodeMessage(ByteData message) {
    if (message == null) {
      return null;
    }
    final ReadBuffer buffer = ReadBuffer(message);
    final dynamic result = readValue(buffer);
    if (buffer.hasRemaining) {
      throw const FormatException('Message corrupted');
    }
    return result;
  }

  /// Writes [value] to [buffer] by first writing a type discriminator
  /// byte, then the value itself.
  ///
  /// This method may be called recursively to serialize container values.
  ///
  /// Type discriminators 0 through 127 inclusive are reserved for use by the
  /// base class, as follows:
  ///
  ///  * null = 0
  ///  * true = 1
  ///  * false = 2
  ///  * 32 bit integer = 3
  ///  * 64 bit integer = 4
  ///  * larger integers = 5 (see below)
  ///  * 64 bit floating-point number = 6
  ///  * String = 7
  ///  * Uint8List = 8
  ///  * Int32List = 9
  ///  * Int64List = 10
  ///  * Float64List = 11
  ///  * List = 12
  ///  * Map = 13
  ///  * Reserved for future expansion: 14..127
  ///
  /// The codec can be extended by overriding this method, calling super
  /// for values that the extension does not handle. Type discriminators
  /// used by extensions must be greater than or equal to 128 in order to avoid
  /// clashes with any later extensions to the base class.
  ///
  /// The "larger integers" type, 5, is never used by [writeValue]. A subclass
  /// could represent big integers from another package using that type. The
  /// format is first the type byte (0x05), then the actual number as an ASCII
  /// string giving the hexadecimal representation of the integer, with the
  /// string's length as encoded by [writeSize] followed by the string bytes. On
  /// Android, that would get converted to a `java.math.BigInteger` object. On
  /// iOS, the string representation is returned.
  void writeValue(WriteBuffer buffer, dynamic value) {
    if (value == null) {
      buffer.putUint8(_valueNull);
    } else if (value is bool) {
      buffer.putUint8(value ? _valueTrue : _valueFalse);
    } else if (value is double) {  // Double precedes int because in JS everything is a double.
                                   // Therefore in JS, both `is int` and `is double` always
                                   // return `true`. If we check int first, we'll end up treating
                                   // all numbers as ints and attempt the int32/int64 conversion,
                                   // which is wrong. This precedence rule is irrelevant when
                                   // decoding because we use tags to detect the type of value.
      buffer.putUint8(_valueFloat64);
      buffer.putFloat64(value);
    } else if (value is int) {
      if (-0x7fffffff - 1 <= value && value <= 0x7fffffff) {
        buffer.putUint8(_valueInt32);
        buffer.putInt32(value);
      } else {
        buffer.putUint8(_valueInt64);
        buffer.putInt64(value);
      }
    } else if (value is String) {
      buffer.putUint8(_valueString);
      final Uint8List bytes = utf8.encoder.convert(value) as Uint8List;
      writeSize(buffer, bytes.length);
      buffer.putUint8List(bytes);
    } else if (value is Uint8List) {
      buffer.putUint8(_valueUint8List);
      writeSize(buffer, value.length);
      buffer.putUint8List(value);
    } else if (value is Int32List) {
      buffer.putUint8(_valueInt32List);
      writeSize(buffer, value.length);
      buffer.putInt32List(value);
    } else if (value is Int64List) {
      buffer.putUint8(_valueInt64List);
      writeSize(buffer, value.length);
      buffer.putInt64List(value);
    } else if (value is Float64List) {
      buffer.putUint8(_valueFloat64List);
      writeSize(buffer, value.length);
      buffer.putFloat64List(value);
    } else if (value is List) {
      buffer.putUint8(_valueList);
      writeSize(buffer, value.length);
      for (final dynamic item in value) {
        writeValue(buffer, item);
      }
    } else if (value is Map) {
      buffer.putUint8(_valueMap);
      writeSize(buffer, value.length);
      value.forEach((dynamic key, dynamic value) {
        writeValue(buffer, key);
        writeValue(buffer, value);
      });
    } else {
      throw ArgumentError.value(value);
    }
  }

  /// Reads a value from [buffer] as written by [writeValue].
  ///
  /// This method is intended for use by subclasses overriding
  /// [readValueOfType].
  dynamic readValue(ReadBuffer buffer) {
    if (!buffer.hasRemaining) {
      throw const FormatException('Message corrupted');
    }
    final int type = buffer.getUint8();
    return readValueOfType(type, buffer);
  }

  /// Reads a value of the indicated [type] from [buffer].
  ///
  /// The codec can be extended by overriding this method, calling super for
  /// types that the extension does not handle. See the discussion at
  /// [writeValue].
  dynamic readValueOfType(int type, ReadBuffer buffer) {
    switch (type) {
      case _valueNull:
        return null;
      case _valueTrue:
        return true;
      case _valueFalse:
        return false;
      case _valueInt32:
        return buffer.getInt32();
      case _valueInt64:
        return buffer.getInt64();
      case _valueFloat64:
        return buffer.getFloat64();
      case _valueLargeInt:
      case _valueString:
        final int length = readSize(buffer);
        return utf8.decoder.convert(buffer.getUint8List(length));
      case _valueUint8List:
        final int length = readSize(buffer);
        return buffer.getUint8List(length);
      case _valueInt32List:
        final int length = readSize(buffer);
        return buffer.getInt32List(length);
      case _valueInt64List:
        final int length = readSize(buffer);
        return buffer.getInt64List(length);
      case _valueFloat64List:
        final int length = readSize(buffer);
        return buffer.getFloat64List(length);
      case _valueList:
        final int length = readSize(buffer);
        final List<dynamic> result = List<dynamic>.filled(length, null, growable: false);
        for (int i = 0; i < length; i++) {
          result[i] = readValue(buffer);
        }
        return result;
      case _valueMap:
        final int length = readSize(buffer);
        final Map<dynamic, dynamic> result = <dynamic, dynamic>{};
        for (int i = 0; i < length; i++) {
          result[readValue(buffer)] = readValue(buffer);
        }
        return result;
      default: throw const FormatException('Message corrupted');
    }
  }

  /// Writes a non-negative 32-bit integer [value] to [buffer]
  /// using an expanding 1-5 byte encoding that optimizes for small values.
  ///
  /// This method is intended for use by subclasses overriding
  /// [writeValue].
  void writeSize(WriteBuffer buffer, int value) {
    assert(0 <= value && value <= 0xffffffff);
    if (value < 254) {
      buffer.putUint8(value);
    } else if (value <= 0xffff) {
      buffer.putUint8(254);
      buffer.putUint16(value);
    } else {
      buffer.putUint8(255);
      buffer.putUint32(value);
    }
  }

  /// Reads a non-negative int from [buffer] as written by [writeSize].
  ///
  /// This method is intended for use by subclasses overriding
  /// [readValueOfType].
  int readSize(ReadBuffer buffer) {
    final int value = buffer.getUint8();
    switch (value) {
      case 254:
        return buffer.getUint16();
      case 255:
        return buffer.getUint32();
      default:
        return value;
    }
  }
}


/// Read-only buffer for reading sequentially from a [ByteData] instance.
///
/// The byte order used is [Endian.host] throughout.
class ReadBuffer {
  /// Creates a [ReadBuffer] for reading from the specified [data].
  ReadBuffer(this.data)
    : assert(data != null);

  /// The underlying data being read.
  final ByteData data;

  /// The position to read next.
  int _position = 0;

  /// Whether the buffer has data remaining to read.
  bool get hasRemaining => _position < data.lengthInBytes;

  /// Reads a Uint8 from the buffer.
  int getUint8() {
    return data.getUint8(_position++);
  }

  /// Reads a Uint16 from the buffer.
  int getUint16({Endian endian}) {
    final int value = data.getUint16(_position, endian ?? Endian.host);
    _position += 2;
    return value;
  }

  /// Reads a Uint32 from the buffer.
  int getUint32({Endian endian}) {
    final int value = data.getUint32(_position, endian ?? Endian.host);
    _position += 4;
    return value;
  }

  /// Reads an Int32 from the buffer.
  int getInt32({Endian endian}) {
    final int value = data.getInt32(_position, endian ?? Endian.host);
    _position += 4;
    return value;
  }

  /// Reads an Int64 from the buffer.
  int getInt64({Endian endian}) {
    final int value = data.getInt64(_position, endian ?? Endian.host);
    _position += 8;
    return value;
  }

  /// Reads a Float64 from the buffer.
  double getFloat64({Endian endian}) {
    _alignTo(8);
    final double value = data.getFloat64(_position, endian ?? Endian.host);
    _position += 8;
    return value;
  }

  /// Reads the given number of Uint8s from the buffer.
  Uint8List getUint8List(int length) {
    final Uint8List list = data.buffer.asUint8List(data.offsetInBytes + _position, length);
    _position += length;
    return list;
  }

  /// Reads the given number of Int32s from the buffer.
  Int32List getInt32List(int length) {
    _alignTo(4);
    final Int32List list = data.buffer.asInt32List(data.offsetInBytes + _position, length);
    _position += 4 * length;
    return list;
  }

  /// Reads the given number of Int64s from the buffer.
  Int64List getInt64List(int length) {
    _alignTo(8);
    final Int64List list = data.buffer.asInt64List(data.offsetInBytes + _position, length);
    _position += 8 * length;
    return list;
  }

  /// Reads the given number of Float64s from the buffer.
  Float64List getFloat64List(int length) {
    _alignTo(8);
    final Float64List list = data.buffer.asFloat64List(data.offsetInBytes + _position, length);
    _position += 8 * length;
    return list;
  }

  void _alignTo(int alignment) {
    final int mod = _position % alignment;
    if (mod != 0) {
      _position += alignment - mod;
    }
  }
}


/// Write-only buffer for incrementally building a [ByteData] instance.
///
/// A WriteBuffer instance can be used only once. Attempts to reuse will result
/// in [NoSuchMethodError]s being thrown.
///
/// The byte order used is [Endian.host] throughout.
class WriteBuffer {
  /// Creates an interface for incrementally building a [ByteData] instance.
  WriteBuffer()
    : _buffer = Uint8Buffer(),
      _eightBytes = ByteData(8) {
    _eightBytesAsList = _eightBytes.buffer.asUint8List();
  }

  Uint8Buffer _buffer;
  final ByteData _eightBytes;
  Uint8List _eightBytesAsList;

  /// Write a Uint8 into the buffer.
  void putUint8(int byte) {
    _buffer.add(byte);
  }

  /// Write a Uint16 into the buffer.
  void putUint16(int value, {Endian endian}) {
    _eightBytes.setUint16(0, value, endian ?? Endian.host);
    _buffer.addAll(_eightBytesAsList, 0, 2);
  }

  /// Write a Uint32 into the buffer.
  void putUint32(int value, {Endian endian}) {
    _eightBytes.setUint32(0, value, endian ?? Endian.host);
    _buffer.addAll(_eightBytesAsList, 0, 4);
  }

  /// Write an Int32 into the buffer.
  void putInt32(int value, {Endian endian}) {
    _eightBytes.setInt32(0, value, endian ?? Endian.host);
    _buffer.addAll(_eightBytesAsList, 0, 4);
  }

  /// Write an Int64 into the buffer.
  void putInt64(int value, {Endian endian}) {
    _eightBytes.setInt64(0, value, endian ?? Endian.host);
    _buffer.addAll(_eightBytesAsList, 0, 8);
  }

  /// Write an Float64 into the buffer.
  void putFloat64(double value, {Endian endian}) {
    _alignTo(8);
    _eightBytes.setFloat64(0, value, endian ?? Endian.host);
    _buffer.addAll(_eightBytesAsList);
  }

  /// Write all the values from a [Uint8List] into the buffer.
  void putUint8List(Uint8List list) {
    _buffer.addAll(list);
  }

  /// Write all the values from an [Int32List] into the buffer.
  void putInt32List(Int32List list) {
    _alignTo(4);
    _buffer.addAll(list.buffer.asUint8List(list.offsetInBytes, 4 * list.length));
  }

  /// Write all the values from an [Int64List] into the buffer.
  void putInt64List(Int64List list) {
    _alignTo(8);
    _buffer.addAll(list.buffer.asUint8List(list.offsetInBytes, 8 * list.length));
  }

  /// Write all the values from a [Float64List] into the buffer.
  void putFloat64List(Float64List list) {
    _alignTo(8);
    _buffer.addAll(list.buffer.asUint8List(list.offsetInBytes, 8 * list.length));
  }

  void _alignTo(int alignment) {
    final int mod = _buffer.length % alignment;
    if (mod != 0) {
      for (int i = 0; i < alignment - mod; i++) {
        _buffer.add(0);
      }
    }
  }

  /// Finalize and return the written [ByteData].
  ByteData done() {
    final ByteData result = _buffer.buffer.asByteData(0, _buffer.lengthInBytes);
    _buffer = null;
    return result;
  }
}

bool listEquals<T>(List<T> a, List<T> b) {
  if (a == null) {
    return b == null;
  }
  if (b == null || a.length != b.length) {
    return false;
  }
  if (identical(a, b)) {
    return true;
  }
  for (int index = 0; index < a.length; index += 1) {
    if (a[index] != b[index]) {
      return false;
    }
  }
  return true;
}
