// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:typed_data';
import 'package:flutter/material.dart';
import 'dart:developer';


final SimdMatrix4 _identityMatrix = SimdMatrix4(
  1, 0, 0, 0,
  0, 1, 0, 0,
  0, 0, 1, 0,
  0, 0, 0, 1
);

/// A matrix 4 implementation backed by Float32x$ SIMD data types.
class SimdMatrix4 {
  /// Create a new Matrix4.
  factory SimdMatrix4(
    double a0,
    double a1,
    double a2,
    double a3,
    double b0,
    double b1,
    double b2,
    double b3,
    double c0,
    double c1,
    double c2,
    double c3,
    double d0,
    double d1,
    double d2,
    double d3,
  ) {
    return SimdMatrix4._(
      Float32x4(a0, b0, c0, d0),
      Float32x4(a1, b1, c1, d1),
      Float32x4(a2, b2, c2, d2),
      Float32x4(a3, b3, c3, d3),
    );
  }

  // factory SimdMatrix4.fromVectorMath(Matrix4 matrix4) {
  //   Float32x4List data = Float32x4List(4);
  //   final Float64List storage = matrix4.storage;
  //   data[0] = Float32x4(storage[0],  storage[1],  storage[2],  storage[3]);
  //   data[1] = Float32x4(storage[4],  storage[5],  storage[6],  storage[7]);
  //   data[2] = Float32x4(storage[8],  storage[9],  storage[10], storage[11]);
  //   data[3] = Float32x4(storage[12], storage[13], storage[14], storage[15]);
  //   return SimdMatrix4._(data);
  // }

  /// Return the identity matrix.
  factory SimdMatrix4.identity() => _identityMatrix;

  SimdMatrix4._(this._column0, this._column1, this._column2, this._column3);

  final Float32x4 _column0;
  final Float32x4 _column1;
  final Float32x4 _column2;
  final Float32x4 _column3;

  /// Multiply this matrix by [other], producing a new Matrix.
  ///
  /// Taken from an archived dart.dev website post about SIMD from
  /// 2012.
  SimdMatrix4 operator *(SimdMatrix4 other) {
    final Float32x4 a0 = _column0;
    final Float32x4 a1 = _column1;
    final Float32x4 a2 = _column2;
    final Float32x4 a3 = _column3;

    final Float32x4 b0 = other._column0;
    final Float32x4 result0 = b0.shuffle(Float32x4.xxxx) * a0 +
        b0.shuffle(Float32x4.yyyy) * a1 +
        b0.shuffle(Float32x4.zzzz) * a2 +
        b0.shuffle(Float32x4.wwww) * a3;
    final Float32x4 b1 = other._column1;
    final Float32x4 result1 = b1.shuffle(Float32x4.xxxx) * a0 +
        b1.shuffle(Float32x4.yyyy) * a1 +
        b1.shuffle(Float32x4.zzzz) * a2 +
        b1.shuffle(Float32x4.wwww) * a3;
    final Float32x4 b2 = other._column2;
    final Float32x4 result2 = b2.shuffle(Float32x4.xxxx) * a0 +
        b2.shuffle(Float32x4.yyyy) * a1 +
        b2.shuffle(Float32x4.zzzz) * a2 +
        b2.shuffle(Float32x4.wwww) * a3;
    final Float32x4 b3 = other._column3;
    final Float32x4 result3 = b3.shuffle(Float32x4.xxxx) * a0 +
        b3.shuffle(Float32x4.yyyy) * a1 +
        b3.shuffle(Float32x4.zzzz) * a2 +
        b3.shuffle(Float32x4.wwww) * a3;
    return SimdMatrix4._(result0, result1, result2, result3);
  }

  /// An implementation of matrix inversion.
  ///
  /// This is based on https://github.com/tc39/ecmascript_simd/blob/master/src/benchmarks/inverse4x4.js
  SimdMatrix4 invert() {
    final Float32x4 src0 = _column0;
    final Float32x4 src1 = _column1;
    final Float32x4 src2 = _column2;
    final Float32x4 src3 = _column3;

    // Incorrect transposition?
    Float32x4 tmp1 = src0.shuffleMix(src1, Float32x4.xyxy); // 0, 1, 4, 5
    Float32x4 row1 = src2.shuffleMix(src3, Float32x4.xyxy); // 0, 1, 4, 5
    Float32x4 row0 = tmp1.shuffleMix(row1, Float32x4.xzxz); // 0, 2, 4, 6
    row1 = row1.shuffleMix(tmp1, Float32x4.ywyw); // 1, 3, 5, 7
    //

    //
    tmp1 = src0.shuffleMix(src1, Float32x4.zwzw); // 2, 3, 6, 7
    Float32x4 row3 = src2.shuffleMix(src3, Float32x4.zwzw); // 2, 3, 6, 7
    Float32x4 row2 = tmp1.shuffleMix(row3, Float32x4.xzxz); // 0, 2, 4, 6
    row3 = row3.shuffleMix(tmp1, Float32x4.ywyw); // 1, 3, 5, 7
    //
    // Float32x4 tmp1 = src0.shuffleMix(src1, Float32x4.xyxy);
    // Float32x4 tmp2 = src2.shuffleMix(src3, Float32x4.xyxy);
    // Float32x4 row0  = tmp1.shuffleMix(tmp2, Float32x4.xzxz);
    // Float32x4 row1  = tmp1.shuffleMix(tmp2, Float32x4.ywyw);

    // tmp1 = src0.shuffleMix(src1, Float32x4.zwzw);
    // tmp2 = src2.shuffleMix(src3, Float32x4.zwzw);
    // Float32x4 row2  = tmp1.shuffleMix(tmp2, Float32x4.xzxz);
    // Float32x4 row3  = tmp1.shuffleMix(tmp2, Float32x4.ywyw);

    //
    tmp1 = row2 * row3;
    tmp1 = tmp1.shuffle(Float32x4.yxwz); // 1, 0, 3, 2
    Float32x4 minor0 = row1 * tmp1;
    Float32x4 minor1 = row0 * tmp1;
    tmp1 = tmp1.shuffle(Float32x4.zwxy); // 2, 3, 0, 1
    minor0 = (row1 * tmp1) - minor0;
    minor1 = (row0 * tmp1) - minor1;
    minor1 = minor1.shuffle(Float32x4.zwxy); // 2, 3, 0, 1
    //

    //
    tmp1 = row1 * row2;
    tmp1 = tmp1.shuffle(Float32x4.yxwz); // 1, 0, 3, 2
    minor0 = (row3 * tmp1) + minor0;
    Float32x4 minor3 = row0 * tmp1;
    tmp1 = tmp1.shuffle(Float32x4.zwxy); // 2, 3, 0, 1
    minor0 = minor0 - (row3 * tmp1);
    minor3 = (row0 * tmp1) - minor3;
    minor3 = minor3.shuffle(Float32x4.zwxy); // 2, 3, 0, 1
    //

    //
    tmp1 = row1.shuffle(Float32x4.zwxy) * row3; // 2, 3, 0, 1
    tmp1 = tmp1.shuffle(Float32x4.yxwz); // 1, 0, 3, 2
    row2 = row2.shuffle(Float32x4.zwxy); // 2, 3, 0, 1
    minor0 = (row2 * tmp1) + minor0;
    Float32x4 minor2 = row0 * tmp1;
    tmp1 = tmp1.shuffle(Float32x4.zwxy); // 2, 3, 0, 1
    minor0 = minor0 - (row2 * tmp1);
    minor2 = (row0 * tmp1) - minor2;
    minor2 = minor2.shuffle(Float32x4.zwxy); // 2, 3, 0, 1
    //

    //
    tmp1 = row0 * row1;
    tmp1 = tmp1.shuffle(Float32x4.yxwz); //  1, 0, 3, 2
    minor2 = (row3 * tmp1) + minor2;
    minor3 = (row2 * tmp1) - minor3;
    tmp1 = tmp1.shuffle(Float32x4.zwxy); // 2, 3, 0, 1
    minor2 = (row3 * tmp1) - minor2;
    minor3 = minor3 - (row2 * tmp1);
    //

    //
    tmp1 = row0 * row3;
    tmp1 = tmp1.shuffle(Float32x4.yxwz);
    minor1 = minor1 - (row2 * tmp1);
    minor2 = (row1 * tmp1) + minor2;
    tmp1 = tmp1.shuffle(Float32x4.zwxy);
    minor1 = (row2 * tmp1) + minor1;
    minor2 = minor2 - (row1 * tmp1);
    //

    // ----
    tmp1 = row0 * row2;
    tmp1 = tmp1.shuffle(Float32x4.yxwz);
    minor1 = (row3 * tmp1) + minor1;
    minor3 = minor3 - (row1 * tmp1);
    tmp1 = tmp1.shuffle(Float32x4.zwxy);
    minor1 = minor1 - (row3 * tmp1);
    minor3 = (row1 * tmp1) + minor3;

    // Compute determinant
    Float32x4 det = row0 * minor0;
    det = det.shuffle(Float32x4.zwxy) + det;
    det = det.shuffle(Float32x4.yxwz) + det;
    tmp1 = det.reciprocal();
    det = (tmp1 + tmp1) - (det * (tmp1 * tmp1));
    det = det.shuffle(Float32x4.xxxx);

    // minor0 = minor0.shuffle(Float32x4.zyxw);
    // minor1 = minor1.shuffle(Float32x4.zyxw);
    // minor2 = minor2.shuffle(Float32x4.zyxw);
    // minor3 = minor3.shuffle(Float32x4.zyxw);

    // Compute final values by multiplying with 1/det
    minor0 = det * minor0;
    minor1 = det * minor1;
    minor2 = det * minor2;
    minor3 = det * minor3;

    return SimdMatrix4._(minor0, minor1, minor2, minor3);
  }
}

void main() {
  runApp(MaterialApp(
    home: Scaffold(
      body: Center(
        child: MaterialButton(onPressed: bench, child: Text('bench'),),
      )
    )
  ));
}

void bench() {
  Timeline.startSync('bench');
  for (int j = 0; j < 20; j++) {
    var sw = Stopwatch()..start();
    SimdMatrix4 result;
    var simd4Identity = SimdMatrix4.identity();
    for (var i = 0; i < 10000; i++) {
      result = simd4Identity.invert();
    }
    sw.stop();
    print(result);
    print(
        'SIMD: ${sw.elapsedMilliseconds}ms or ${sw.elapsedMicroseconds / 10000} microseconds per invert');

    sw.reset();
    sw.start();
    double det = 0;
    Matrix4 identity = Matrix4.identity();
    for (var i = 0; i < 10000; i++) {
      det = identity.copyInverse(Matrix4.identity());
    }
    sw.stop();
    print(det);
    print(
        'VM: ${sw.elapsedMilliseconds}ms or ${sw.elapsedMicroseconds / 10000} microseconds per invert');
  }
   Timeline.finishSync();
}
