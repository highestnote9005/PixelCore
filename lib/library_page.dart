// Biblioteca: lista de jogos importados e botão para abrir uma ROM.
import 'dart:io';

import 'package:flutter/material.dart';

import 'emulator_page.dart';
import 'platform.dart';
import 'storage.dart';

class LibraryPage extends StatefulWidget {
  const LibraryPage({super.key, required this.store});

  final Store store;

  @override
  State<LibraryPage> createState() => _LibraryPageState();
}

class _LibraryPageState extends State<LibraryPage> {
  bool _busy = false;

  void _msg(String text) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(text)));
  }

  Future<void> _import() async {
    setState(() => _busy = true);
    try {
      final picked = await HostPlatform.pickFile();
      if (picked == null) return;
      final Game g;
      try {
        g = await widget.store.importRom(picked.path, name: picked.name);
      } finally {
        try {
          await File(picked.path).delete();
        } catch (_) {}
      }
      if (!mounted) return;
      setState(() => _busy = false);
      await _play(g);
    } on FormatException catch (e) {
      _msg(e.message);
    } catch (e) {
      _msg('Erro ao abrir a ROM: $e');
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _play(Game g) async {
    await widget.store.touch(g);
    if (!mounted) return;
    await Navigator.of(context).push(
      MaterialPageRoute<void>(builder: (_) => EmulatorPage(store: widget.store, game: g)),
    );
    if (mounted) setState(() {});
  }

  Future<void> _confirmRemove(Game g) async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Remover jogo?'),
        content: Text('"${g.title}" será removido da biblioteca, junto com o save e os estados salvos.'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('Cancelar')),
          FilledButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('Remover')),
        ],
      ),
    );
    if (ok == true) {
      await widget.store.remove(g);
      if (mounted) setState(() {});
    }
  }

  String _when(int ms) {
    if (ms <= 0) return 'nunca jogado';
    final d = DateTime.fromMillisecondsSinceEpoch(ms);
    String two(int v) => v.toString().padLeft(2, '0');
    return '${two(d.day)}/${two(d.month)}/${d.year} ${two(d.hour)}:${two(d.minute)}';
  }

  @override
  Widget build(BuildContext context) {
    final games = widget.store.sorted;
    return Scaffold(
      appBar: AppBar(title: const Text('PocketGBA')),
      floatingActionButton: FloatingActionButton.extended(
        onPressed: _busy ? null : _import,
        icon: const Icon(Icons.folder_open),
        label: const Text('Abrir ROM'),
      ),
      body: Stack(children: [
        if (games.isEmpty)
          Center(
            child: Padding(
              padding: const EdgeInsets.all(32),
              child: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  const Icon(Icons.sports_esports, size: 72),
                  const SizedBox(height: 16),
                  Text('Nenhum jogo ainda', style: Theme.of(context).textTheme.titleLarge),
                  const SizedBox(height: 8),
                  const Text(
                    'Toque em "Abrir ROM" e escolha um arquivo .gba (ou um .zip com ele). '
                    'Use apenas ROMs de jogos que você possui.',
                    textAlign: TextAlign.center,
                  ),
                ],
              ),
            ),
          )
        else
          ListView.separated(
            padding: const EdgeInsets.only(bottom: 96),
            itemCount: games.length,
            separatorBuilder: (_, __) => const Divider(height: 1),
            itemBuilder: (ctx, i) {
              final g = games[i];
              final mb = (g.size / (1024 * 1024)).toStringAsFixed(1);
              return ListTile(
                leading: const CircleAvatar(child: Icon(Icons.videogame_asset)),
                title: Text(g.title),
                subtitle: Text('${g.code.isEmpty ? '' : '${g.code} · '}$mb MB · ${_when(g.lastPlayed)}'),
                onTap: () => _play(g),
                trailing: PopupMenuButton<String>(
                  onSelected: (v) {
                    if (v == 'remove') _confirmRemove(g);
                  },
                  itemBuilder: (_) => const [
                    PopupMenuItem<String>(value: 'remove', child: Text('Remover')),
                  ],
                ),
              );
            },
          ),
        if (_busy) const Positioned.fill(child: ColoredBox(color: Color(0x66000000), child: Center(child: CircularProgressIndicator()))),
      ]),
    );
  }
}
