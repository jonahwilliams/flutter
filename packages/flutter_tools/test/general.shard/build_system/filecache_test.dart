// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:flutter_tools/src/base/file_system.dart';
import 'package:flutter_tools/src/build_system/build_system.dart';
import 'package:flutter_tools/src/build_system/file_hash_store.dart';
import 'package:flutter_tools/src/convert.dart';

import '../../src/common.dart';
import '../../src/testbed.dart';

void main() {
  Testbed testbed;
  Environment environment;

  setUp(() {
    testbed = Testbed(setup: () {
      fs.directory('build').createSync();
      environment = Environment(
        outputDir: fs.currentDirectory,
        projectDir: fs.currentDirectory,
      );
      environment.buildDir.createSync(recursive: true);
    });
  });

  test('Initializes file cache', () => testbed.run(() {
    final FileHashStore fileCache = FileHashStore(environment);
    fileCache.initialize();
    fileCache.persist();

    expect(fs.file(fs.path.join(environment.buildDir.path, '.filecache')).existsSync(), true);

    final List<int> buffer = fs.file(fs.path.join(environment.buildDir.path, '.filecache'))
        .readAsBytesSync();
    final FileStorage fileStorage = FileStorage.fromBuffer(buffer);

    expect(fileStorage.files, isEmpty);
    expect(fileStorage.version, 2);
  }));

  test('saves and restores to file cache', () => testbed.run(() async {
    final File file = fs.file('foo.dart')
      ..createSync()
      ..writeAsStringSync('hello');
    final FileHashStore fileCache = FileHashStore(environment);
    fileCache.initialize();
    await fileCache.hashFiles(<File>[file]);
    fileCache.persist();
    final String currentHash =  fileCache.currentHashes[file.path];
    final List<int> buffer = fs.file(fs.path.join(environment.buildDir.path, '.filecache'))
        .readAsBytesSync();
    FileStorage fileStorage = FileStorage.fromBuffer(buffer);

    expect(fileStorage.files.single.hash, currentHash);
    expect(fileStorage.files.single.path, file.path);


    final FileHashStore newFileCache = FileHashStore(environment);
    newFileCache.initialize();
    expect(newFileCache.currentHashes, isEmpty);
    expect(newFileCache.previousHashes['foo.dart'],  currentHash);
    newFileCache.persist();

    // Still persisted correctly.
    fileStorage = FileStorage.fromBuffer(buffer);

    expect(fileStorage.files.single.hash, currentHash);
    expect(fileStorage.files.single.path, file.path);
  }));

  test('handles persisting with a missing build directory', () => testbed.run(() async {
    final File file = fs.file('foo.dart')
      ..createSync()
      ..writeAsStringSync('hello');
    final FileHashStore fileCache = FileHashStore(environment);
    fileCache.initialize();
    environment.buildDir.deleteSync(recursive: true);

    await fileCache.hashFiles(<File>[file]);
    // Does not throw.
    fileCache.persist();
  }));

  // Test cases taken from https://tools.ietf.org/html/rfc1321.
  test('md5 test cases', () => testbed.run(() {
    expect(FileHashStore.computeHash(FakeRandomAccessFile('')),
      'd41d8cd98f00b204e9800998ecf8427e');
    expect(FileHashStore.computeHash(FakeRandomAccessFile('a')),
      '0cc175b9c0f1b6a831c399e269772661');
    expect(FileHashStore.computeHash(FakeRandomAccessFile('abc')),
      '900150983cd24fb0d6963f7d28e17f72');
    expect(FileHashStore.computeHash(FakeRandomAccessFile('message digest')),
      'f96b697d7cb7938d525a2f31aaf161d0');
    expect(FileHashStore.computeHash(FakeRandomAccessFile('abcdefghijklmnopqr'
      'stuvwxyz')), 'c3fcd3d76192e4007dfb496cca67e13b');
    expect(FileHashStore.computeHash(FakeRandomAccessFile('ABCDEFGHIJKLMNOPQRS'
      'TUVWXYZabcdefghijklmnopqrstuvwxyz0123456789')),
      'd174ab98d277d9f5a5611c2c9f419d9f');
    expect(FileHashStore.computeHash(FakeRandomAccessFile('1234567890123456789'
      '0123456789012345678901234567890123456789012345678901234567890')),
      '57edf4a22be3c955ac49da2e2107b67a');
  }));
}

class FakeRandomAccessFile implements RandomAccessFile, File {
  FakeRandomAccessFile(String contents)
    : _contents = utf8.encode(contents);

  int _position = 0;
  final List<int> _contents;

  @override
  Future<int> length() async => _contents.length;

  @override
  Future<RandomAccessFile> setPosition(int newPosition) async {
    _position = newPosition;
    return this;
  }

  @override
  Future<int> readInto(List<int> buffer, [int start = 0, int end]) async {
    int i;
    for (i = 0; i < buffer.length && _position + i < _contents.length; i++) {
      buffer[i] = _contents[_position + i];
    }
    return i;
  }

  @override
  void noSuchMethod(Invocation invocation) => throw UnimplementedError();

  @override
  Future<void> close() async {}
}