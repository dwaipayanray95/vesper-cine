import 'package:flutter/material.dart';

import '../services/pro_service.dart';

/// "This is a Pro feature": unlock Pro, or watch one ad for every Pro feature
/// on the next [ProService.proPassClips] clips. Returns true when the pass was
/// earned (the caller may go ahead); false when cancelled, or when the Play
/// purchase sheet was opened (it finishes there).
Future<bool> showProUpsell(BuildContext context, String feature) async {
  final pro = ProService.instance;
  if (pro.hasProFeatures) return true;
  final n = ProService.proPassClips;
  final choice = await showDialog<String>(
    context: context,
    builder: (ctx) => AlertDialog(
      backgroundColor: const Color(0xFF14171D),
      title: const Text('Pro feature', style: TextStyle(color: Colors.white, fontSize: 18)),
      content: Text(
        '$feature is part of Vesper Pro. Unlock Pro once${pro.price == null ? '' : ' (${pro.price})'} for every feature, '
        'unlimited clips and no ads, or watch one short ad to use all Pro features for your next $n clips.',
        style: const TextStyle(color: Color(0xFFB5BDC7), fontSize: 13.5, height: 1.4),
      ),
      actions: [
        TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('NOT NOW')),
        TextButton(onPressed: () => Navigator.pop(ctx, 'pro'), child: const Text('GET PRO')),
        FilledButton(onPressed: () => Navigator.pop(ctx, 'ad'), child: Text('WATCH AD · PRO FOR $n CLIPS')),
      ],
    ),
  );
  if (!context.mounted || choice == null) return false;
  if (choice == 'pro') {
    await pro.buy();
    return false;
  }
  final ok = await pro.watchAdForPass();
  if (!ok && context.mounted && pro.message.isNotEmpty) {
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(pro.message)));
  }
  return ok;
}
