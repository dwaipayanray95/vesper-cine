import 'package:flutter_test/flutter_test.dart';
import 'package:flutter/material.dart';
import 'package:vesper_cine/main.dart';
import 'package:vesper_cine/ui/value_picker.dart';

void main() {
  testWidgets('Vesper Cine boots to the camera HUD', (WidgetTester tester) async {
    await tester.pumpWidget(const VesperCineApp());
    expect(find.text('APPLE LOG · 2020'), findsOneWidget);
  });

  // Picker sheets must fit a landscape phone (Pixel 10 is ~915x411 logical px).
  testWidgets('Tall picker sheet scrolls instead of overflowing in landscape', (tester) async {
    tester.view.physicalSize = const Size(2424, 1080);
    tester.view.devicePixelRatio = 2.625;
    addTearDown(tester.view.reset);
    await tester.pumpWidget(
      MaterialApp(
        home: Builder(
          builder: (ctx) => TextButton(
            onPressed: () => showModalBottomSheet(
              context: ctx,
              isScrollControlled: true,
              builder: (_) => SheetBody(
                child: Column(children: [for (var i = 0; i < 12; i++) const SizedBox(height: 48, child: Text('row'))]),
              ),
            ),
            child: const Text('open'),
          ),
        ),
      ),
    );
    await tester.tap(find.text('open'));
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
    expect(find.text('row'), findsWidgets);
  });
}
