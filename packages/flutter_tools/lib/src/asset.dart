// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';

import 'package:flutter_tools/src/project.dart';
import 'package:meta/meta.dart';
import 'package:platform/platform.dart';
import 'package:yaml/yaml.dart';

import 'base/context.dart';
import 'base/file_system.dart';
import 'base/logger.dart';
import 'base/utils.dart';
import 'build_info.dart';
import 'cache.dart';
import 'convert.dart';
import 'dart/package_map.dart';
import 'devfs.dart';
import 'flutter_manifest.dart';
import 'globals.dart' as globals;

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

  bool wasBuiltOnce();

  bool needsBuild({ String manifestPath = defaultManifestPath });

  /// Returns 0 for success; non-zero for failure.
  Future<int> build({
    String manifestPath = defaultManifestPath,
    String assetDirPath,
    String packagesPath,
    bool includeDefaultFonts = true,
    bool reportLicensedPackages = false,
  });
}

class _ManifestAssetBundleFactory implements AssetBundleFactory {
  const _ManifestAssetBundleFactory();

  @override
  AssetBundle createBundle() => ManifestAssetBundle(
    fileSystem: globals.fs,
    logger: globals.logger,
    platform: globals.platform,
  );
}

class ManifestAssetBundle implements AssetBundle {
  /// Constructs an [ManifestAssetBundle] that gathers the set of assets from the
  /// pubspec.yaml manifest.
  ManifestAssetBundle({
    @required FileSystem fileSystem,
    @required Logger logger,
    @required Platform platform,
  }) : _fileSystem = fileSystem,
       _logger = logger,
       _platform = platform;

  final Logger _logger;
  final FileSystem _fileSystem;
  final Platform _platform;

  @override
  final Map<String, DevFSContent> entries = <String, DevFSContent>{};

  // If an asset corresponds to a wildcard directory, then it may have been
  // updated without changes to the manifest.
  final Map<Uri, Directory> _wildcardDirectories = <Uri, Directory>{};

  DateTime _lastBuildTimestamp;

  static const String _assetManifestJson = 'AssetManifest.json';
  static const String _fontSetMaterial = 'material';
  static const String _license = 'LICENSE';

  @override
  bool wasBuiltOnce() => _lastBuildTimestamp != null;

  @override
  bool needsBuild({ String manifestPath = defaultManifestPath }) {
    if (_lastBuildTimestamp == null) {
      return true;
    }

    final FileStat stat = _fileSystem.file(manifestPath).statSync();
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
    String packagesPath,
    bool includeDefaultFonts = true,
    bool reportLicensedPackages = false,
  }) async {
    assetDirPath ??= getAssetBuildDirectory();
    packagesPath ??= _fileSystem.path.absolute(PackageMap.globalPackagesPath);
    FlutterProject flutterProject;
    FlutterManifest flutterManifest;
    try {
      flutterProject = FlutterProject.fromPath(manifestPath);
      flutterManifest = flutterProject.manifest;
    } catch (e) {
      _logger.printStatus('Error detected in pubspec.yaml:', emphasis: true);
      _logger.printError('$e');
      return 1;
    }
    if (flutterManifest == null) {
      return 1;
    }

    // If the last build time isn't set before this early return, empty pubspecs will
    // hang on hot reload, as the incremental dill files will never be copied to the
    // device.
    _lastBuildTimestamp = DateTime.now();
    if (flutterManifest.isEmpty) {
      entries[_assetManifestJson] = DevFSStringContent('{}');
      return 0;
    }

    final String assetBasePath = _fileSystem.path.dirname(_fileSystem.path.absolute(manifestPath));

    final PackageMap packageMap = PackageMap(packagesPath);
    final List<Uri> wildcardDirectories = <Uri>[];

    // The _assetVariants map contains an entry for each asset listed
    // in the pubspec.yaml file's assets and font and sections. The
    // value of each image asset is a list of resolution-specific "variants",
    // see _AssetDirectoryCache.
    final Map<Asset, List<Asset>> assetVariants = _parseAssets(
      packageMap,
      flutterManifest,
      wildcardDirectories,
      assetBasePath,
      excludeDirs: <String>[
        assetDirPath,
        getBuildDirectory(),
        // Desktop projects include an ephemeral directory under the project directory.
        flutterProject.linux.ephemeralDirectory.path,
        flutterProject.macos.ephemeralDirectory.path,
        flutterProject.windows.ephemeralDirectory.path,
      ],
    );

    if (assetVariants == null) {
      return 1;
    }

    final List<Map<String, dynamic>> fonts = _parseFonts(
      flutterManifest,
      includeDefaultFonts,
      packageMap,
    );

    // Add fonts and assets from packages.
    for (final String packageName in packageMap.map.keys) {
      final Uri package = packageMap.map[packageName];
      if (package != null && package.scheme == 'file') {
        final String packageManifestPath = _fileSystem.path.fromUri(package.resolve('../pubspec.yaml'));
        final FlutterManifest packageFlutterManifest = FlutterManifest.createFromPath(packageManifestPath);
        if (packageFlutterManifest == null) {
          continue;
        }
        // Skip the app itself
        if (packageFlutterManifest.appName == flutterManifest.appName) {
          continue;
        }
        final String packageBasePath = _fileSystem.path.dirname(packageManifestPath);

        final Map<Asset, List<Asset>> packageAssets = _parseAssets(
          packageMap,
          packageFlutterManifest,
          wildcardDirectories,
          packageBasePath,
          packageName: packageName,
        );

        if (packageAssets == null) {
          return 1;
        }
        assetVariants.addAll(packageAssets);

        fonts.addAll(_parseFonts(
          packageFlutterManifest,
          includeDefaultFonts,
          packageMap,
          packageName: packageName,
        ));
      }
    }

    // Save the contents of each image, image variant, and font
    // asset in entries.
    for (final Asset asset in assetVariants.keys) {
      if (!asset.assetFileExists(_fileSystem) && assetVariants[asset].isEmpty) {
        _logger.printStatus('Error detected in pubspec.yaml:', emphasis: true);
        _logger.printError('No file or variants found for $asset.\n');
        return 1;
      }
      // The file name for an asset's "main" entry is whatever appears in
      // the pubspec.yaml file. The main entry's file must always exist for
      // font assets. It need not exist for an image if resolution-specific
      // variant files exist. An image's main entry is treated the same as a
      // "1x" resolution variant and if both exist then the explicit 1x
      // variant is preferred.
      if (asset.assetFileExists(_fileSystem)) {
        assert(!assetVariants[asset].contains(asset));
        assetVariants[asset].insert(0, asset);
      }
      for (final Asset variant in assetVariants[asset]) {
        assert(variant.assetFileExists(_fileSystem));
        entries[variant.entryUri.path] ??= DevFSFileContent(variant.getAssetFile(_fileSystem));
      }
    }

    final List<Asset> materialAssets = <Asset>[
      if (flutterManifest.usesMaterialDesign && includeDefaultFonts)
        ..._getMaterialAssets(_fontSetMaterial),
    ];
    for (final Asset asset in materialAssets) {
      assert(asset.assetFileExists(_fileSystem));
      entries[asset.entryUri.path] ??= DevFSFileContent(asset.getAssetFile(_fileSystem));
    }

    // Update wildcard directories we we can detect changes in them.
    for (final Uri uri in wildcardDirectories) {
      _wildcardDirectories[uri] ??= _fileSystem.directory(uri);
    }

    entries[_assetManifestJson] = _createAssetManifest(assetVariants);

    entries[kFontManifestJson] = DevFSStringContent(json.encode(fonts));

    // TODO(ianh): Only do the following line if we've changed packages or if our LICENSE file changed
    entries[_license] = _obtainLicenses(packageMap, assetBasePath, reportPackages: reportLicensedPackages);

    return 0;
  }

  Map<String, dynamic> _readMaterialFontsManifest() {
    final String fontsPath = _fileSystem.path.join(_fileSystem.path.absolute(Cache.flutterRoot),
        'packages', 'flutter_tools', 'schema', 'material_fonts.yaml');

    return castStringKeyedMap(loadYaml(_fileSystem.file(fontsPath).readAsStringSync()));
  }

  Map<String, dynamic> _materialFontsManifest;

  List<Map<String, dynamic>> _getMaterialFonts(String fontSet) {
    _materialFontsManifest ??= _readMaterialFontsManifest();
    final List<dynamic> fontsList = _materialFontsManifest[fontSet] as List<dynamic>;
    return fontsList?.map<Map<String, dynamic>>(castStringKeyedMap)?.toList();
  }

  List<Asset> _getMaterialAssets(String fontSet) {
    final List<Asset> result = <Asset>[];

    for (final Map<String, dynamic> family in _getMaterialFonts(fontSet)) {
      for (final Map<dynamic, dynamic> font in family['fonts']) {
        final Uri entryUri = _fileSystem.path.toUri(font['asset'] as String);
        result.add(Asset(
          baseDir: _fileSystem.path.join(Cache.flutterRoot, 'bin', 'cache', 'artifacts', 'material_fonts'),
          relativeUri: Uri(path: entryUri.pathSegments.last),
          entryUri: entryUri,
        ));
      }
    }

    return result;
  }

  final String _licenseSeparator = '\n' + ('-' * 80) + '\n';

  /// Returns a DevFSContent representing the license file.
  DevFSContent _obtainLicenses(
    PackageMap packageMap,
    String assetBase, {
    bool reportPackages,
  }) {
    // Read the LICENSE file from each package in the .packages file, splitting
    // each one into each component license (so that we can de-dupe if possible).
    //
    // Individual licenses inside each LICENSE file should be separated by 80
    // hyphens on their own on a line.
    //
    // If a LICENSE file contains more than one component license, then each
    // component license must start with the names of the packages to which the
    // component license applies, with each package name on its own line, and the
    // list of package names separated from the actual license text by a blank
    // line. (The packages need not match the names of the pub package. For
    // example, a package might itself contain code from multiple third-party
    // sources, and might need to include a license for each one.)
    final Map<String, Set<String>> packageLicenses = <String, Set<String>>{};
    final Set<String> allPackages = <String>{};
    for (final String packageName in packageMap.map.keys) {
      final Uri package = packageMap.map[packageName];
      if (package == null || package.scheme != 'file') {
        continue;
      }
      final File file = _fileSystem.file(package.resolve('../LICENSE'));
      if (!file.existsSync()) {
        continue;
      }
      final List<String> rawLicenses =
          file.readAsStringSync().split(_licenseSeparator);
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
          packageNames = <String>[packageName];
          licenseText = rawLicense;
        }
        packageLicenses.putIfAbsent(licenseText, () => <String>{})
          ..addAll(packageNames);
        allPackages.addAll(packageNames);
      }
    }

    if (reportPackages) {
      final List<String> allPackagesList = allPackages.toList()..sort();
      _logger.printStatus('Licenses were found for the following packages:');
      _logger.printStatus(allPackagesList.join(', '));
    }

    final List<String> combinedLicensesList = packageLicenses.keys.map<String>(
      (String license) {
        final List<String> packageNames = packageLicenses[license].toList()
        ..sort();
        return packageNames.join('\n') + '\n\n' + license;
      }
    ).toList();
    combinedLicensesList.sort();

    final String combinedLicenses = combinedLicensesList.join(_licenseSeparator);

    return DevFSStringContent(combinedLicenses);
  }

  DevFSContent _createAssetManifest(Map<Asset, List<Asset>> assetVariants) {
    final Map<String, List<String>> jsonObject = <String, List<String>>{};

    // necessary for making unit tests deterministic
    final List<Asset> sortedKeys = assetVariants
        .keys.toList()
      ..sort((Asset a, Asset b) {
      return a.getAssetFile(_fileSystem).basename.compareTo(b.getAssetFile(_fileSystem).basename);
    });

    for (final Asset main in sortedKeys) {
      jsonObject[main.entryUri.path] = <String>[
        for (final Asset variant in assetVariants[main])
          variant.entryUri.path,
      ];
    }
    return DevFSStringContent(json.encode(jsonObject));
  }

  List<Map<String, dynamic>> _parseFonts(
    FlutterManifest manifest,
    bool includeDefaultFonts,
    PackageMap packageMap, {
    String packageName,
  }) {
    return <Map<String, dynamic>>[
      if (manifest.usesMaterialDesign && includeDefaultFonts)
        ..._getMaterialFonts(ManifestAssetBundle._fontSetMaterial),
      if (packageName == null)
        ...manifest.fontsDescriptor
      else
        ..._createFontsDescriptor(_parsePackageFonts(
          manifest,
          packageName,
          packageMap,
        )),
    ];
  }

  /// Prefixes family names and asset paths of fonts included from packages with
  /// 'packages/<package_name>'
  List<Font> _parsePackageFonts(
    FlutterManifest manifest,
    String packageName,
    PackageMap packageMap,
  ) {
    final List<Font> packageFonts = <Font>[];
    for (final Font font in manifest.fonts) {
      final List<FontAsset> packageFontAssets = <FontAsset>[];
      for (final FontAsset fontAsset in font.fontAssets) {
        final Uri assetUri = fontAsset.assetUri;
        if (assetUri.pathSegments.first == 'packages' &&
            !_fileSystem.isFileSync(_fileSystem.path.fromUri(packageMap.map[packageName].resolve('../${assetUri.path}')))) {
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

  /// Given an assetBase location and a pubspec.yaml Flutter manifest, return a
  /// map of assets to asset variants.
  ///
  /// Returns null on missing assets.
  ///
  /// Given package: 'test_package' and an assets directory like this:
  ///
  /// assets/foo
  /// assets/var1/foo
  /// assets/var2/foo
  /// assets/bar
  ///
  /// returns
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
  ///
  Map<Asset, List<Asset>> _parseAssets(
    PackageMap packageMap,
    FlutterManifest flutterManifest,
    List<Uri> wildcardDirectories,
    String assetBase, {
    List<String> excludeDirs = const <String>[],
    String packageName,
  }) {
    final Map<Asset, List<Asset>> result = <Asset, List<Asset>>{};

    final _AssetDirectoryCache cache = _AssetDirectoryCache(excludeDirs, fileSystem: _fileSystem);
    for (final Uri assetUri in flutterManifest.assets) {
      if (assetUri.toString().endsWith('/')) {
        wildcardDirectories.add(assetUri);
        _parseAssetsFromFolder(packageMap, flutterManifest, assetBase,
            cache, result, assetUri,
            excludeDirs: excludeDirs, packageName: packageName);
      } else {
        _parseAssetFromFile(packageMap, flutterManifest, assetBase,
            cache, result, assetUri,
            excludeDirs: excludeDirs, packageName: packageName);
      }
    }

    // Add assets referenced in the fonts section of the manifest.
    for (final Font font in flutterManifest.fonts) {
      for (final FontAsset fontAsset in font.fontAssets) {
        final Asset baseAsset = _resolveAsset(
          packageMap,
          assetBase,
          fontAsset.assetUri,
          packageName,
        );
        if (!baseAsset.assetFileExists(_fileSystem)) {
          _logger.printError('Error: unable to locate asset entry in pubspec.yaml: "${fontAsset.assetUri}".');
          return null;
        }

        result[baseAsset] = <Asset>[];
      }
    }

    return result;
  }

  void _parseAssetsFromFolder(
    PackageMap packageMap,
    FlutterManifest flutterManifest,
    String assetBase,
    _AssetDirectoryCache cache,
    Map<Asset, List<Asset>> result,
    Uri assetUri, {
    List<String> excludeDirs = const <String>[],
    String packageName,
  }) {
    final String directoryPath = _fileSystem.path.join(
        assetBase, assetUri.toFilePath(windows: _platform.isWindows));

    if (!_fileSystem.directory(directoryPath).existsSync()) {
      _logger.printError('Error: unable to find directory entry in pubspec.yaml: $directoryPath');
      return;
    }

    final List<FileSystemEntity> lister = _fileSystem.directory(directoryPath).listSync();
    for (final FileSystemEntity entity in lister) {
      if (entity is File) {
        final String relativePath = _fileSystem.path.relative(entity.path, from: assetBase);
        final Uri uri = Uri.file(relativePath, windows: _platform.isWindows);

        _parseAssetFromFile(packageMap, flutterManifest, assetBase, cache, result,
            uri, packageName: packageName);
      }
    }
  }

  void _parseAssetFromFile(
    PackageMap packageMap,
    FlutterManifest flutterManifest,
    String assetBase,
    _AssetDirectoryCache cache,
    Map<Asset, List<Asset>> result,
    Uri assetUri, {
    List<String> excludeDirs = const <String>[],
    String packageName,
  }) {
    final Asset asset = _resolveAsset(
      packageMap,
      assetBase,
      assetUri,
      packageName,
    );
    final List<Asset> variants = <Asset>[];
    for (final String path in cache.variantsFor(asset.getAssetFile(_fileSystem).path)) {
      final String relativePath = _fileSystem.path.relative(path, from: asset.baseDir);
      final Uri relativeUri = _fileSystem.path.toUri(relativePath);
      final Uri entryUri = asset.symbolicPrefixUri == null
          ? relativeUri
          : asset.symbolicPrefixUri.resolveUri(relativeUri);

      variants.add(
        Asset(
          baseDir: asset.baseDir,
          entryUri: entryUri,
          relativeUri: relativeUri,
        ),
      );
    }

    result[asset] = variants;
  }

  Asset _resolveAsset(
    PackageMap packageMap,
    String assetsBaseDir,
    Uri assetUri,
    String packageName,
  ) {
    final String assetPath = _fileSystem.path.fromUri(assetUri);
    if (assetUri.pathSegments.first == 'packages' && !_fileSystem.isFileSync(_fileSystem.path.join(assetsBaseDir, assetPath))) {
      // The asset is referenced in the pubspec.yaml as
      // 'packages/PACKAGE_NAME/PATH/TO/ASSET .
      final Asset packageAsset = _resolvePackageAsset(assetUri, packageMap);
      if (packageAsset != null) {
        return packageAsset;
      }
    }

    return Asset(
      baseDir: assetsBaseDir,
      entryUri: packageName == null
          ? assetUri // Asset from the current application.
          : Uri(pathSegments: <String>['packages', packageName, ...assetUri.pathSegments]), // Asset from, and declared in $packageName.
      relativeUri: assetUri,
    );
  }

  Asset _resolvePackageAsset(Uri assetUri, PackageMap packageMap) {
    assert(assetUri.pathSegments.first == 'packages');
    if (assetUri.pathSegments.length > 1) {
      final String packageName = assetUri.pathSegments[1];
      final Uri packageUri = packageMap.map[packageName];
      if (packageUri != null && packageUri.scheme == 'file') {
        return Asset(
          baseDir: _fileSystem.path.fromUri(packageUri),
          entryUri: assetUri,
          relativeUri: Uri(pathSegments: assetUri.pathSegments.sublist(2)),
        );
      }
    }
    _logger.printStatus('Error detected in pubspec.yaml:', emphasis: true);
    _logger.printError('Could not resolve package for asset $assetUri.\n');
    return null;
  }
}

class Asset {
  Asset({ this.baseDir, this.relativeUri, this.entryUri });

  final String baseDir;

  /// A platform-independent URL where this asset can be found on disk on the
  /// host system relative to [baseDir].
  final Uri relativeUri;

  /// A platform-independent URL representing the entry for the asset manifest.
  final Uri entryUri;

  File getAssetFile(FileSystem fileSystem) {
    return fileSystem.file(fileSystem.path.join(baseDir, fileSystem.path.fromUri(relativeUri)));
  }

  bool assetFileExists(FileSystem fileSystem) => getAssetFile(fileSystem).existsSync();

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
    return other is Asset
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
  _AssetDirectoryCache(Iterable<String> excluded, {
    @required FileSystem fileSystem,
  }) : _excluded = excluded.map<String>((String path) => fileSystem.path.absolute(path) + fileSystem.path.separator),
       _fileSystem = fileSystem;

  final FileSystem _fileSystem;
  final Iterable<String> _excluded;
  final Map<String, Map<String, List<String>>> _cache = <String, Map<String, List<String>>>{};

  List<String> variantsFor(String assetPath) {
    final String assetName = _fileSystem.path.basename(assetPath);
    final String directory = _fileSystem.path.dirname(assetPath);

    if (!_fileSystem.directory(directory).existsSync()) {
      return const <String>[];
    }

    if (_cache[directory] == null) {
      final List<String> paths = <String>[];
      for (final FileSystemEntity entity in _fileSystem.directory(directory).listSync(recursive: true)) {
        final String path = entity.path;
        if (_fileSystem.isFileSync(path) && !_excluded.any((String exclude) => path.startsWith(exclude))) {
          paths.add(path);
        }
      }

      final Map<String, List<String>> variants = <String, List<String>>{};
      for (final String path in paths) {
        final String variantName = _fileSystem.path.basename(path);
        if (directory == _fileSystem.path.dirname(path)) {
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
