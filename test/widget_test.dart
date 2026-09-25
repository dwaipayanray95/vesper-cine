import 'package:flutter_test/flutter_test.dart';
import 'package:vesper_cine/main.dart';

void main() {
  testWidgets('Vesper Cine boots to the camera HUD', (WidgetTester tester) async {
    await tester.pumpWidget(const VesperCineApp());
    expect(find.text('APPLE LOG · 2020'), findsOneWidget);
  });
}
