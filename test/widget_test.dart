import 'package:flutter_test/flutter_test.dart';
import 'package:r_camera/main.dart';

void main() {
  testWidgets('R-Camera app smoke test', (WidgetTester tester) async {
    await tester.pumpWidget(const RawEdgeApp());
    expect(find.text('R-LOG'), findsOneWidget);
  });
}
