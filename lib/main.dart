import 'package:flutter/material.dart';
import 'services/crash_reporting.dart';
import 'services/pro_service.dart';
import 'ui/camera_screen.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await CrashReporting.init();
  ProService.instance.init(); // not awaited: the store may be slow
  runApp(const VesperCineApp());
}

class VesperCineApp extends StatelessWidget {
  const VesperCineApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Vesper Cine',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        brightness: Brightness.dark,
        scaffoldBackgroundColor: Colors.black,
        colorScheme: const ColorScheme.dark(primary: Colors.redAccent, surface: Colors.black),
        useMaterial3: true,
      ),
      home: const CameraScreen(),
    );
  }
}
