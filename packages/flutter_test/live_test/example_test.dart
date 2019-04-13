import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  testLiveWidgets('can wait until network request finished', (LiveWidgetTester tester) async {
    bool didLoad = false;
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: Center(
          child: MaterialButton(
            child: const Text('button'),
            onPressed: () async {
              try {
                await HttpClient().getUrl(Uri.parse('http://www.google.com'));
              } finally {
                didLoad = true;
              }
            },
          )
        )
      )
    ));
    expect(didLoad, false);
    await tester.tap(find.text('button'));
    await tester.waitUntilNetworkIdle();
    expect(didLoad, true);
  });
}