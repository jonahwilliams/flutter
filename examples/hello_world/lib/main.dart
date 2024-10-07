import 'package:flutter/material.dart';

class BlurTestWidget extends StatelessWidget {
  const BlurTestWidget({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: NestedScrollView(
        headerSliverBuilder: (context, innerBoxIsScrolled) => [
          const SliverAppBar(
            title: Text('Blur + Gradient Test'),
          ),
        ],
        body: CustomScrollView(
          physics: const BouncingScrollPhysics(decelerationRate: ScrollDecelerationRate.fast),
          slivers: List.filled(
            5,
            SliverToBoxAdapter(
              child: Padding(
                padding: const EdgeInsets.all(32.0),
                child: AspectRatio(
                  aspectRatio: 2,
                  child: CustomPaint(painter: _BlurTestPainter()),
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class _BlurTestPainter extends CustomPainter {
  @override
  void paint(Canvas canvas, Size size) {
    final rect = Rect.fromLTWH(0, 0, size.width, size.height);

    final shader = const LinearGradient(
      colors: [Colors.red, Colors.blue],
    ).createShader(rect);

    var paint = Paint()
      ..shader = shader
      ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 10.0)
      ..strokeWidth = 20
      ..style = PaintingStyle.stroke;

    canvas.drawOval(rect, paint);
  }

  @override
  bool shouldRepaint(covariant _BlurTestPainter o) => true;
}

