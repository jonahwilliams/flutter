import 'dart:collection';
import 'dart:typed_data';
import 'dart:ui' as ui;

class DebugCanvas implements ui.Canvas {
  DebugCanvas(this.delegate, this.recorder);

  final ui.Canvas delegate;
  final PaintRecorder recorder;

  @override
  void clipPath(ui.Path path, {bool doAntiAlias = true}) {
    recorder._record(PaintOp.clipPath);
    delegate.clipPath(path);
  }

  @override
  void clipRRect(ui.RRect rrect, {bool doAntiAlias = true}) {
    recorder._record(PaintOp.clipRRect);
    delegate.clipRRect(rrect, doAntiAlias: doAntiAlias);
  }

  @override
  void clipRect(ui.Rect rect, {ui.ClipOp clipOp = ui.ClipOp.intersect, bool doAntiAlias = true}) {
    recorder._record(PaintOp.clipRect);
    delegate.clipRect(rect, clipOp: clipOp, doAntiAlias: doAntiAlias);
  }

  @override
  void drawArc(ui.Rect rect, double startAngle, double sweepAngle, bool useCenter, ui.Paint paint) {
    recorder._record(PaintOp.drawArc);
    delegate.drawArc(rect, startAngle, sweepAngle, useCenter, paint);
  }

  @override
  void drawAtlas(ui.Image atlas, List<ui.RSTransform> transforms, List<ui.Rect> rects, List<ui.Color>? colors, ui.BlendMode? blendMode, ui.Rect? cullRect, ui.Paint paint) {
    recorder._record(PaintOp.drawAtlas);
    delegate.drawAtlas(atlas, transforms, rects, colors, blendMode, cullRect, paint);
  }

  @override
  void drawCircle(ui.Offset c, double radius, ui.Paint paint) {
    recorder._record(PaintOp.drawCircle);
    delegate.drawCircle(c, radius, paint);
  }

  @override
  void drawColor(ui.Color color, ui.BlendMode blendMode) {
    recorder._record(PaintOp.drawColor);
    delegate.drawColor(color, blendMode);
  }

  @override
  void drawDRRect(ui.RRect outer, ui.RRect inner, ui.Paint paint) {
    recorder._record(PaintOp.drawDRRect);
    delegate.drawDRRect(outer, inner, paint);
  }

  @override
  void drawImage(ui.Image image, ui.Offset offset, ui.Paint paint) {
    recorder._record(PaintOp.drawImage);
    delegate.drawImage(image, offset, paint);
  }

  @override
  void drawImageNine(ui.Image image, ui.Rect center, ui.Rect dst, ui.Paint paint) {
    recorder._record(PaintOp.drawImageNine);
    delegate.drawImageNine(image, center, dst, paint);
  }

  @override
  void drawImageRect(ui.Image image, ui.Rect src, ui.Rect dst, ui.Paint paint) {
    recorder._record(PaintOp.drawImageRect);
    delegate.drawImageRect(image, src, dst, paint);
  }

  @override
  void drawLine(ui.Offset p1, ui.Offset p2, ui.Paint paint) {
    recorder._record(PaintOp.drawLine);
    delegate.drawLine(p1, p2, paint);
  }

  @override
  void drawOval(ui.Rect rect, ui.Paint paint) {
    recorder._record(PaintOp.drawOval);
    delegate.drawOval(rect, paint);
  }

  @override
  void drawPaint(ui.Paint paint) {
    recorder._record(PaintOp.drawPaint);
    delegate.drawPaint(paint);
  }

  @override
  void drawParagraph(ui.Paragraph paragraph, ui.Offset offset) {
    recorder._record(PaintOp.drawParagraph, <Object>[paragraph, offset]);
    delegate.drawParagraph(paragraph, offset);
  }

  @override
  void drawPath(ui.Path path, ui.Paint paint) {
    recorder._record(PaintOp.drawPath);
    delegate.drawPath(path, paint);
  }

  @override
  void drawPicture(ui.Picture picture) {
    recorder._record(PaintOp.drawPicture);
    delegate.drawPicture(picture);
  }

  @override
  void drawPoints(ui.PointMode pointMode, List<ui.Offset> points, ui.Paint paint) {
    recorder._record(PaintOp.drawPoints);
    delegate.drawPoints(pointMode, points, paint);
  }

  @override
  void drawRRect(ui.RRect rrect, ui.Paint paint) {
    recorder._record(PaintOp.drawRRect);
    delegate.drawRRect(rrect, paint);
  }

  @override
  void drawRawAtlas(ui.Image atlas, Float32List rstTransforms, Float32List rects, Int32List? colors, ui.BlendMode? blendMode, ui.Rect? cullRect, ui.Paint paint) {
    recorder._record(PaintOp.drawRawAtlas);
    delegate.drawRawAtlas(atlas, rstTransforms, rects, colors, blendMode, cullRect, paint);
  }

  @override
  void drawRawPoints(ui.PointMode pointMode, Float32List points, ui.Paint paint) {
    recorder._record(PaintOp.drawRawPoints);
    delegate.drawRawPoints(pointMode, points, paint);
  }

  @override
  void drawRect(ui.Rect rect, ui.Paint paint) {
    recorder._record(PaintOp.drawRect);
    delegate.drawRect(rect, paint);
  }

  @override
  void drawShadow(ui.Path path, ui.Color color, double elevation, bool transparentOccluder) {
    recorder._record(PaintOp.drawShadow);
    delegate.drawShadow(path, color, elevation, transparentOccluder);
  }

  @override
  void drawVertices(ui.Vertices vertices, ui.BlendMode blendMode, ui.Paint paint) {
    recorder._record(PaintOp.drawVertices);
    delegate.drawVertices(vertices, blendMode, paint);
  }

  @override
  int getSaveCount() {
    return delegate.getSaveCount();
  }

  @override
  void restore() {
    recorder._record(PaintOp.restore);
    delegate.restore();
  }

  @override
  void rotate(double radians) {
    recorder._record(PaintOp.rotate);
    delegate.rotate(radians);
  }

  @override
  void save() {
    recorder._record(PaintOp.save);
    delegate.save();
  }

  @override
  void saveLayer(ui.Rect? bounds, ui.Paint paint) {
    recorder._record(PaintOp.saveLayer);
    delegate.saveLayer(bounds, paint);
  }

  @override
  void scale(double sx, [double? sy]) {
    recorder._record(PaintOp.scale);
    delegate.scale(sx, sy);
  }

  @override
  void skew(double sx, double sy) {
    recorder._record(PaintOp.skew);
    delegate.skew(sx, sy);
  }

  @override
  void transform(Float64List matrix4) {
    recorder._record(PaintOp.transform);
    delegate.transform(matrix4);
  }

  @override
  void translate(double dx, double dy) {
    recorder._record(PaintOp.translate);
    delegate.translate(dx, dy);
  }
}

enum PaintOp {
  translate,
  transform,
  skew,
  scale,
  saveLayer,
  save,
  rotate,
  restore,
  drawVertices,
  drawShadow,
  drawRect,
  drawRawPoints,
  drawRawAtlas,
  clipPath,
  clipRRect,
  clipRect,
  drawArc,
  drawAtlas,
  drawDRRect,
  drawCircle,
  drawColor,
  drawImageNine,
  drawImage,
  drawLine,
  drawOval,
  drawParagraph,
  drawPath,
  drawPicture,
  drawPaint,
  drawRRect,
  drawPoints,
  drawImageRect,
}

class PaintRecording {
  PaintRecording(this.op, this.args);

  final PaintOp op;
  final List<Object> args;

  String toString() => '{$op, ${args.join(',')}';
}

class PaintRecorder {
  final Map<Object, List<Object>> _recordings = HashMap<Object, List<Object>>();
  List<Object> _active = <Object>[];
  bool _hasPaint = false;

  set active(Object config) {
    _active = _recordings[config] ?? <Object>[];
    _recordings[config] = _active;
  }

  void _record(PaintOp op, [List<Object> args = const []]) {
    _hasPaint = true;
    _active.add(PaintRecording(op, args));
  }

  List<Object>? opsFor(Object config) {
    return _recordings[config];
  }

  bool get hasPaint => _hasPaint;
}
