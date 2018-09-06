import 'dart:io';

import 'package:flutter/rendering.dart';
import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  testWidgets('InvertColors',  (WidgetTester tester) async {
    await tester.pumpWidget(const RepaintBoundary(
      child: SizedBox(
        width: 200.0,
        height: 200.0,
        child: InvertColorTestWidget(
          color: Color.fromRGBO(255, 0, 0, 1.0),
          invertColors: true,
        ),
      ),
    ));

    await expectLater(
      find.byType(RepaintBoundary),
      matchesGoldenFile('invert_colors_test.0.png'),
      skip: !Platform.isLinux, // explanation
    );
  }, skip: true);

  testWidgets('InvertColors and ColorFilter',  (WidgetTester tester) async {
    await tester.pumpWidget(const RepaintBoundary(
      child: SizedBox(
        width: 200.0,
        height: 200.0,
        child: InvertColorTestWidget(
          color: Color.fromRGBO(255, 0, 0, 1.0),
          filter: ColorFilter.mode(Color.fromRGBO(0, 255, 0, 0.5), BlendMode.darken),
          invertColors: true,
        ),
      ),
    ));

    await expectLater(
      find.byType(RepaintBoundary),
      matchesGoldenFile('invert_colors_test.1.png'),
      skip: !Platform.isLinux, // explanation
    );
  }, skip: true);
}

// Draws a rectangle sized by the parent widget with [color], [colorFilter],
// and [invertColors] applied for testing the invert colors.
class InvertColorTestWidget extends LeafRenderObjectWidget {
  const InvertColorTestWidget({
    this.color,
    this.filter,
    this.invertColors,
    Key key
  }) : super(key: key);

  final Color color;
  final ColorFilter filter;
  final bool invertColors;

  @override
  RenderInvertColorTest createRenderObject(BuildContext context) {
    return new RenderInvertColorTest(color, filter, invertColors);
  }
  @override
  void updateRenderObject(BuildContext context, covariant RenderInvertColorTest renderObject) {
    renderObject
      ..color = color
      ..filter = filter
      ..invertColors = invertColors;
  }

}

class RenderInvertColorTest extends RenderProxyBox {
  RenderInvertColorTest(this._color, this._filter, this._invertColors);

  @override
  bool get sizedByParent => true;

  bool get invertColors => _invertColors;
  bool _invertColors;
  set invertColors(bool value) {
    if (invertColors == value)
      return;
    _invertColors = value;
    markNeedsPaint();
  }

  Color get color => _color;
  Color _color;
  set color(Color value) {
    if (color == value)
      return;
    _color = value;
    markNeedsPaint();
  }


  ColorFilter get filter => _filter;
  ColorFilter _filter;
  set filter(ColorFilter value) {
    if (filter == value)
      return;
    _filter = value;
    markNeedsPaint();
  }

  @override
  void paint(PaintingContext context, Offset offset) {
    final Paint paint = new Paint()
      ..style = PaintingStyle.fill
      ..color = color
      ..colorFilter = filter;
    // TODO: pass invertColors.
    context.canvas.drawRect(offset & size, paint);
  }
}