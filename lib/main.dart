import 'package:flutter/material.dart';

import 'library_page.dart';
import 'storage.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  final store = await Store.open();
  runApp(PocketGbaApp(store: store));
}

class PocketGbaApp extends StatelessWidget {
  const PocketGbaApp({super.key, required this.store});

  final Store store;

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'PocketGBA',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        useMaterial3: true,
        colorScheme: ColorScheme.fromSeed(seedColor: const Color(0xFF6B4FE0), brightness: Brightness.dark),
      ),
      home: LibraryPage(store: store),
    );
  }
}
