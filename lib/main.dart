import 'package:flutter/material.dart';
import 'ui/camera_screen.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const RawEdgeApp());
}

class RawEdgeApp extends StatelessWidget {
  const RawEdgeApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Vesper Cine',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        brightness: Brightness.dark,
        scaffoldBackgroundColor: Colors.black,
        colorScheme: const ColorScheme.dark(
          primary: Colors.redAccent,
          surface: Colors.black,
        ),
        useMaterial3: true,
      ),
      home: const CameraScreen(),
    );
  }
}
