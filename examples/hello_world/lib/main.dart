// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';

//void main() => runApp(const Center(child: const Text('Hello, world!', textDirection: TextDirection.ltr)));


void main() {
  runApp(new Center(
    child: new Transform(
      transform: new Matrix4.identity()
        ..setEntry(3, 2, 0.001)
        ..rotateX(0.50)
        , // perspective transform
      child: new Ancestor( // new child layer
        child: new Container( // something to draw
          color: Colors.red,
            width: 200.0,
            height: 200.0,
          ),
       ),
   )));
}

class Ancestor extends SingleChildRenderObjectWidget {
  const Ancestor({Widget child, Key key}): super(child: child, key: key);
  
  @override
  RenderObject createRenderObject(BuildContext context) {
    return new RenderAncestor();
  }
}
class RenderAncestor extends RenderProxyBox {
  @override
  bool get alwaysNeedsCompositing => true; 

  @override
  void paint(PaintingContext context, Offset offset) {
    context.pushLayer(new OffsetLayer(), super.paint, offset);
  }
}