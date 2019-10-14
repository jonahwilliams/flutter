// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'dart:collection';
import 'dart:io' as io;
import 'dart:isolate';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:meta/meta.dart';

import '../base/file_system.dart';
import '../base/io.dart';
import '../convert.dart';
import '../globals.dart';
import 'build_system.dart';

/// An encoded representation of all file hashes.
class FileStorage {
  FileStorage(this.version, this.files);

  factory FileStorage.fromBuffer(Uint8List buffer) {
    final Map<String, Object> json = jsonDecode(utf8.decode(buffer));
    final int version = json['version'];
    final List<Object> rawCachedFiles = json['files'];
    final List<FileHash> cachedFiles = <FileHash>[
      for (Map<String, Object> rawFile in rawCachedFiles) FileHash.fromJson(rawFile),
    ];
    return FileStorage(version, cachedFiles);
  }

  final int version;
  final List<FileHash> files;

  List<int> toBuffer() {
    final Map<String, Object> json = <String, Object>{
      'version': version,
      'files': <Object>[
        for (FileHash file in files) file.toJson(),
      ],
    };
    return utf8.encode(jsonEncode(json));
  }
}

/// A stored file hash and path.
class FileHash {
  FileHash(this.path, this.hash);

  factory FileHash.fromJson(Map<String, Object> json) {
    return FileHash(json['path'], json['hash']);
  }

  final String path;
  final String hash;

  Object toJson() {
    return <String, Object>{
      'path': path,
      'hash': hash,
    };
  }
}

/// A globally accessible cache of file hashes.
///
/// In cases where multiple targets read the same source files as inputs, we
/// avoid recomputing or storing multiple copies of hashes by delegating
/// through this class. All file hashes are held in memory during a build
/// operation, and persisted to cache in the root build directory.
///
/// The format of the file store is subject to change and not part of its API.
class FileHashStore {
  FileHashStore(this.environment);

  final Environment environment;
  final HashMap<String, String> previousHashes = HashMap<String, String>();
  final HashMap<String, String> currentHashes = HashMap<String, String>();

  // The name of the file which stores the file hashes.
  static const String _kFileCache = '.filecache';

  // The current version of the file cache storage format.
  static const int _kVersion = 2;

  /// Read file hashes from disk.
  void initialize() {
    printTrace('Initializing file store');
    if (!_cacheFile.existsSync()) {
      return;
    }
    final List<int> data = _cacheFile.readAsBytesSync();
    FileStorage fileStorage;
    try {
      fileStorage = FileStorage.fromBuffer(data);
    } catch (err) {
      printTrace('Filestorage format changed');
      _cacheFile.deleteSync();
      return;
    }
    if (fileStorage.version != _kVersion) {
      _cacheFile.deleteSync();
      return;
    }
    for (FileHash fileHash in fileStorage.files) {
      previousHashes[fileHash.path] = fileHash.hash;
    }
    printTrace('Done initializing file store');
  }

  /// Persist file hashes to disk.
  void persist() {
    printTrace('Persisting file store');
    final File file = _cacheFile;
    if (!file.existsSync()) {
      file.createSync(recursive: true);
    }
    final List<FileHash> fileHashes = <FileHash>[];
    for (MapEntry<String, String> entry in currentHashes.entries) {
      previousHashes[entry.key] = entry.value;
    }
    for (MapEntry<String, String> entry in previousHashes.entries) {
      fileHashes.add(FileHash(entry.key, entry.value));
    }
    final FileStorage fileStorage = FileStorage(
      _kVersion,
      fileHashes,
    );
    final Uint8List buffer = fileStorage.toBuffer();
    file.writeAsBytesSync(buffer);
    printTrace('Done persisting file store');
  }

  /// Computes a hash of the provided files and returns a list of entities
  /// that were dirty.
  Future<List<File>> hashFiles(List<File> files) async {
    final List<File> dirty = <File>[];
    final int before = processInfo.currentRss;
    final Stopwatch stopwatch = Stopwatch()..start();
    final List<List<File>> partitions = _partitionFiles(files);

    Future<void> work(List<File> files) async {
      if (files.isEmpty) {
        return;
      }
      final ReceivePort resultPort = ReceivePort();
      final List<String> paths = files.map((File file) => file.path).toList();
      final Completer<List<String>> result = Completer<List<String>>();
      resultPort.listen((dynamic resultData) {
        if (!result.isCompleted) {
          result.complete(resultData);
        }
      });
      final Isolate isolate = await Isolate.spawn(_isolateEntrypoint, _IsolateMessage(paths, resultPort.sendPort));
      final List<String> hashes = await result.future;
      isolate.kill();
      for (int i = 0; i < files.length; i++) {
        final File currentFile = files[i];
        final String absolutePath = currentFile.path;
        final String previousHash = previousHashes[absolutePath];
        final String currentHash = hashes[i];
        if (currentHash != previousHash) {
          dirty.add(currentFile);
        }
      }
    }
    if (files.length < 16) {
      final Uint8List rawBuffer = Uint8List(_kBlockSize);
      final Uint32List digest = Uint32List(4);
      for (int i = 0; i < files.length; i++) {
        final File currentFile = files[i];
        final String absolutePath = currentFile.path;
        final String previousHash = previousHashes[absolutePath];
        final String currentHash = computeHash(io.File(files[i].path), rawBuffer, digest);
        if (currentHash != previousHash) {
          dirty.add(currentFile);
        }
      }
    } else {
      await Future.wait(<Future<void>>[
        for (List<File> files in partitions) work(files)
      ]);
    }
    printTrace('hashing ${files.length} file(s) increased rss by '
      '${math.max(processInfo.currentRss - before, 0)} and took '
      '${stopwatch.elapsedMilliseconds} ms or '
      '${stopwatch.elapsedMilliseconds / files.length} ms per file.');
    return dirty;
  }

  /// Partition each file into buckets.
  List<List<File>> _partitionFiles(List<File> files) {
    if (files.length <= 16) {
      return <List<File>>[files];
    }
    final List<List<File>> buckets = List<List<File>>(64);
    for (int i = 0; i < files.length; i += 64) {
      for (int j = 0; j < 64 && i + j < files.length; j++) {
        buckets[j] ??= <File>[];
        buckets[j].add(files[i + j]);
      }
    }
    return buckets;
  }

  static void _isolateEntrypoint(_IsolateMessage message) {
    final List<String> files = message.filePaths;
    // buffers for reading file chunks and maintaining digests.
    final Uint8List rawBuffer = Uint8List(_kBlockSize);
    final Uint32List digest = Uint32List(4);
    final List<String> results = <String>[];
    for (String file in files) {
      results.add(computeHash(io.File(file), rawBuffer, digest));
    }
    message.resultPort.send(results);
  }

  // 64 bytes (512 bits), The block size of the md5 algorithm.
  static const int _kChunkSize = 64;
  // 64k block size for reading the file.
  static const int _kBlockSize = _kChunkSize * 1024;

  @visibleForTesting
  static String computeHash(io.File file, Uint8List rawBuffer, Uint32List digest) {
    // typed views into buffers for easier numerics.
    final Uint64List lengthView = Uint64List.view(rawBuffer.buffer);
    final Uint8List hexView = Uint8List.view(digest.buffer);

    final RandomAccessFile randomAccessFile = file.openSync();

    // zero buffers before use. There are no yields (await) within this method
    // which means the static buffers cannot be written to by multiple calls.
    // TODO(jonahwilliams): is this necessary.
    for (int i = 0; i < rawBuffer.length; i++) {
      rawBuffer[i] = 0;
    }
    // Initialize md5 digest.
    digest[0] = 0x67452301;
    digest[1] = 0xefcdab89;
    digest[2] = 0x98badcfe;
    digest[3] = 0x10325476;

    final int length = randomAccessFile.lengthSync();
    int offset = 0;

    /// Read blocks of 64K at a time from the file. Once we are unable to
    /// read a full block, we switch to the padding buffer. There we can pad
    /// the remaining length to ensure the total message length is divisible
    /// by the chunk size of 64 bytes.
    int lastReadSize = 0;
    while (offset < length) {
      lastReadSize = randomAccessFile.readIntoSync(rawBuffer);
      offset += _kBlockSize;
      randomAccessFile.setPositionSync(offset);
      // BUG: doesn't handle partial bytes.
      for (int i = 0; i < lastReadSize; i += 64) {
        _processChunk(Uint32List.view(rawBuffer.buffer, i, _kChunkSize), digest);
      }
    }
    randomAccessFile.closeSync();

    // Pad the message to ensure we reach both a minimum message size and
    // divisibility by 64.
    final int paddingOffset = lastReadSize % _kBlockSize;
    final int paddingBufferStart = _align(lastReadSize);
    rawBuffer[paddingOffset] = 128;

    // If we don't have enough room for the one and the length,
    // we need to pad to two chunks.
    if (_kBlockSize - lastReadSize > 56) {
      for (int i = paddingOffset + 1; i < 64; i++) {
        rawBuffer[i] = 0;
      }
      _processChunk(Uint32List.view(rawBuffer.buffer, paddingBufferStart, _kChunkSize), digest);
      for (int i = 0; i < 56; i++) {
        rawBuffer[i] = 0;
      }
      // Pad the message the message length in bits.
      lengthView[7] = length * 8;
      _processChunk(Uint32List.view(rawBuffer.buffer, 0, _kChunkSize), digest);
    } else {
      for (int i = paddingOffset + 1; i < 56; i++) {
        rawBuffer[i] = 0;
      }
      // Pad the message the message length in bits.
      lengthView[7] = length * 8;
      _processChunk(Uint32List.view(rawBuffer.buffer, paddingBufferStart, _kChunkSize), digest);
    }

    final List<int> charCodes = <int>[];
    for (int i = 0; i < hexView.length; i++) {
      final int value = hexView[i];
      final int lower = value & 0xF;
      final int upper = value >> 4;
      charCodes.add(_hexAlphabet[upper]);
      charCodes.add(_hexAlphabet[lower]);
    }
    return String.fromCharCodes(charCodes);
  }

  static final List<int> _hexAlphabet = '0123456789abcdef'.codeUnits;

  static int _align(int lastReadSize) {
    if (lastReadSize == _kBlockSize || lastReadSize == 0) {
      return 0;
    }
    final int overflow = lastReadSize % _kChunkSize;
    return lastReadSize - overflow;
  }

  static void _processChunk(Uint32List buffer, Uint32List digest) {
    int a = digest[0];
    int b = digest[1];
    int c = digest[2];
    int d = digest[3];
    for (int i = 0; i < _kChunkSize; i++) {
      int f = 0;
      int e = 0;
      if (i < 16) {
        e = (b & c) | ((~b &  0xFFFFFFFF) & d);
        f = i;
      } else if (i < 32) {
        e = (d & b) | ((~d &  0xFFFFFFFF) & c);
        f = ((5 * i) + 1) % 16;
      } else if (i < 48) {
        e = b ^ c ^ d;
        f = ((3 * i) + 5) % 16;
      } else {
        e = c ^ (b | (~d &  0xFFFFFFFF));
        f = (7 * i) % 16;
      }
      final int swap = d;
      d = c;
      c = b;
      b = _a32(b, _brl(_a32(_a32(a, e), _a32(_noise[i], buffer[f])), _shift[i]));
      a = swap;
    }
    digest[0] += a;
    digest[1] += b;
    digest[2] += c;
    digest[3] += d;
  }

  // constant noise.
  static const List<int> _noise = <int>[
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
  ];

  // Per round shift amounts.
  static const List<int> _shift = <int>[
    07, 12, 17, 22, 07, 12, 17, 22, 07, 12, 17, 22, 07, 12, 17, 22, 05, 09, 14,
    20, 05, 09, 14, 20, 05, 09, 14, 20, 05, 09, 14, 20, 04, 11, 16, 23, 04, 11,
    16, 23, 04, 11, 16, 23, 04, 11, 16, 23, 06, 10, 15, 21, 06, 10, 15, 21, 06,
    10, 15, 21, 06, 10, 15, 21
  ];

  static int _brl(int val, int shift) {
    final int mod = shift & 31;
    return ((val << mod) &  0xFFFFFFFF) | ((val &  0xFFFFFFFF) >> (32 - mod));
  }

  static int _a32(int x, int y) => (x + y) & 0xFFFFFFFF;

  File get _cacheFile => environment.buildDir.childFile(_kFileCache);
}


class _IsolateMessage {
  _IsolateMessage(this.filePaths, this.resultPort);

  final List<String> filePaths;
  final SendPort resultPort;
}