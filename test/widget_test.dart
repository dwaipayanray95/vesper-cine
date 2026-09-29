import 'package:flutter_test/flutter_test.dart';
import 'package:flutter/material.dart';
import 'package:vesper_cine/main.dart';
import 'package:vesper_cine/ui/value_picker.dart';

void main() {
  testWidgets('Vesper Cine boots to the camera HUD', (WidgetTester tester) async {
    await tester.pumpWidget(const VesperCineApp());
    expect(find.text('APPLE LOG · 2020'), findsOneWidget);
    // Verify top bar and left controls
    expect(find.text('REC.709 LUT'), findsOneWidget);
    expect(find.text('FC'), findsOneWidget);
    expect(find.text('PEAK'), findsOneWidget);
    expect(find.text('ZEBRA'), findsOneWidget);
    expect(find.text('SHUTTER'), findsOneWidget);
    expect(find.text('ISO'), findsOneWidget);
    expect(find.text('WB'), findsOneWidget);
    expect(find.text('FOCUS'), findsOneWidget);
  });

  testWidgets('Ronin 4D dial opens, switches, and closes properly', (WidgetTester tester) async {
    await tester.pumpWidget(const VesperCineApp());
    // Tap on SHUTTER control opens Shutter dial
    await tester.tap(find.text('SHUTTER'));
    await tester.pump(const Duration(milliseconds: 300));
    expect(find.text('SHUTTER ANGLE'), findsOneWidget);

    // Tapping ISO closes Shutter dial and opens ISO dial (only one visible)
    await tester.tap(find.text('ISO'));
    await tester.pump(const Duration(milliseconds: 300));
    expect(find.text('SHUTTER ANGLE'), findsNothing);
    expect(find.text('ISO GAIN'), findsOneWidget);

    // Tapping ISO again closes the dial
    await tester.tap(find.text('ISO'));
    await tester.pump(const Duration(milliseconds: 300));
    expect(find.text('ISO GAIN'), findsNothing);

    // Tapping WB opens WB panel with Google AWB switch & Kelvin
    await tester.tap(find.text('WB'));
    await tester.pump(const Duration(milliseconds: 300));
    expect(find.text('GOOGLE AWB'), findsOneWidget);
    expect(find.text('MANUAL'), findsOneWidget);

    // Tapping outside on the center closes the dial
    await tester.tapAt(const Offset(400, 300));
    await tester.pump(const Duration(milliseconds: 300));
    expect(find.text('GOOGLE AWB'), findsNothing);
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
