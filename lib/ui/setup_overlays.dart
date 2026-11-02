import 'package:flutter/material.dart';

import '../services/calibration_registry.dart';
import '../services/device_check.dart';

/// Full-screen first-launch screens (0.27.0): the device check and the guided
/// "Calibrate this phone" setup. Both are drawn over the camera screen (which
/// keeps running underneath) and are driven by state in camera_screen.dart.

const _bg = Color(0xFF0E1013);
const _card = Color(0xFF171B21);
const _line = Color(0xFF232831);
const _grey = Color(0xFF9AA3AE);
const _good = Color(0xFF7ED67E);
const _bad = Color(0xFFFF6B6B);

Widget _frame({required Widget child}) => Material(
  color: _bg,
  child: SafeArea(
    child: Center(
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 720),
        child: Padding(padding: const EdgeInsets.fromLTRB(24, 14, 24, 14), child: child),
      ),
    ),
  ),
);

Widget _button(String label, VoidCallback? onTap, {bool primary = true}) => primary
    ? FilledButton(
        style: FilledButton.styleFrom(backgroundColor: Colors.amber, foregroundColor: Colors.black),
        onPressed: onTap,
        child: Text(label, style: const TextStyle(fontWeight: FontWeight.w700, fontSize: 12.5)),
      )
    : OutlinedButton(
        style: OutlinedButton.styleFrom(side: const BorderSide(color: Colors.amber), foregroundColor: Colors.amber),
        onPressed: onTap,
        child: Text(label, style: const TextStyle(fontSize: 12.5)),
      );

// ---------------------------------------------------------------------------
// Device check
// ---------------------------------------------------------------------------

enum DeviceCheckPhase { waitingWarm, running, result }

class DeviceCheckOverlay extends StatelessWidget {
  final DeviceCheckPhase phase;
  final String progress; // "Checking your phone… 30 fps"
  final double fraction; // 0..1
  final DeviceCheckResult? result;
  final VoidCallback onSkipWait;
  final VoidCallback onContinue; // supported phone
  final VoidCallback onTryAnyway; // unsupported phone

  const DeviceCheckOverlay({
    super.key,
    required this.phase,
    this.progress = '',
    this.fraction = 0,
    this.result,
    required this.onSkipWait,
    required this.onContinue,
    required this.onTryAnyway,
  });

  @override
  Widget build(BuildContext context) => switch (phase) {
    DeviceCheckPhase.waitingWarm => _frame(
      child: Column(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          const Icon(Icons.thermostat, color: Colors.amber, size: 40),
          const SizedBox(height: 14),
          const Text('Phone is too warm to get valid results',
              style: TextStyle(color: Colors.white, fontSize: 18, fontWeight: FontWeight.w600)),
          const SizedBox(height: 8),
          const Text(
            'Waiting for it to cool down. Leave the app open; this goes on by itself.',
            textAlign: TextAlign.center,
            style: TextStyle(color: _grey, fontSize: 13, height: 1.4),
          ),
          const SizedBox(height: 18),
          const SizedBox(width: 160, child: LinearProgressIndicator(color: Colors.amber, backgroundColor: _line)),
          const SizedBox(height: 18),
          _button('SKIP WAITING / TEST ANYWAY', onSkipWait, primary: false),
        ],
      ),
    ),
    DeviceCheckPhase.running => _frame(
      child: Column(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          Text(progress.isEmpty ? 'Checking your phone…' : progress,
              style: const TextStyle(color: Colors.white, fontSize: 18, fontWeight: FontWeight.w600)),
          const SizedBox(height: 8),
          const Text(
            'Vesper Cine is trying 1080p at each frame rate with all processing on. Keep the phone still; this takes about half a minute.',
            textAlign: TextAlign.center,
            style: TextStyle(color: _grey, fontSize: 13, height: 1.4),
          ),
          const SizedBox(height: 18),
          SizedBox(
            width: 260,
            child: LinearProgressIndicator(value: fraction <= 0 ? null : fraction, color: Colors.amber, backgroundColor: _line),
          ),
        ],
      ),
    ),
    DeviceCheckPhase.result => _result(result!),
  };

  Widget _result(DeviceCheckResult r) {
    final ok = r.supported;
    return _frame(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Row(
            children: [
              Icon(ok ? Icons.check_circle : Icons.error_outline, color: ok ? _good : _bad, size: 26),
              const SizedBox(width: 10),
              Text(ok ? 'Your phone is supported' : 'This phone is not supported',
                  style: const TextStyle(color: Colors.white, fontSize: 19, fontWeight: FontWeight.w600)),
              if (r.pretend != Pretend.off) ...[
                const SizedBox(width: 10),
                const Text('(TEST RESULT)', style: TextStyle(color: Colors.amber, fontSize: 11, fontFamily: 'monospace')),
              ],
            ],
          ),
          const SizedBox(height: 8),
          Text(summarize(r), style: const TextStyle(color: Colors.white70, fontSize: 13, height: 1.4)),
          const SizedBox(height: 10),
          Expanded(
            child: ListView(
              children: [
                for (final i in r.items) _itemRow(i),
                if (r.fps.isNotEmpty) ...[
                  const SizedBox(height: 8),
                  const Text('FRAME RATES AT 1080P',
                      style: TextStyle(color: Colors.amber, fontSize: 10, fontWeight: FontWeight.bold, letterSpacing: 1.2)),
                  const SizedBox(height: 6),
                  for (final f in r.fps) _fpsRow(f),
                ],
                if (r.measuredWarm)
                  const Padding(
                    padding: EdgeInsets.only(top: 8),
                    child: Text('Measured while the phone was warm, may be pessimistic: re-check later in Settings › Info.',
                        style: TextStyle(color: Colors.amber, fontSize: 12, height: 1.4)),
                  ),
              ],
            ),
          ),
          const SizedBox(height: 10),
          Align(
            alignment: Alignment.centerRight,
            child: ok ? _button('CONTINUE', onContinue) : _button('TRY ANYWAY', onTryAnyway),
          ),
        ],
      ),
    );
  }

  Widget _itemRow(CheckItem i) {
    final (icon, color) = switch (i.level) {
      CheckLevel.ok => (Icons.check, _good),
      CheckLevel.info => (Icons.info_outline, _grey),
      CheckLevel.warn => (Icons.warning_amber_rounded, Colors.amber),
      CheckLevel.fail => (Icons.close, _bad),
    };
    return Container(
      margin: const EdgeInsets.only(bottom: 6),
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
      decoration: BoxDecoration(color: _card, border: Border.all(color: _line), borderRadius: BorderRadius.circular(8)),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Icon(icon, color: color, size: 16),
          const SizedBox(width: 10),
          Expanded(
            child: Text.rich(TextSpan(children: [
              TextSpan(text: '${i.title}: ', style: const TextStyle(color: Colors.white, fontWeight: FontWeight.w600)),
              TextSpan(text: i.text, style: const TextStyle(color: Colors.white70)),
            ]), style: const TextStyle(fontSize: 12, height: 1.35)),
          ),
        ],
      ),
    );
  }

  Widget _fpsRow(FpsResult f) {
    final (label, color) = switch (f.cls) {
      FpsClass.full => ('Full quality', _good),
      FpsClass.reduced => ('Reduced processing (RQ)', Colors.amber),
      FpsClass.unsupported => ('Too much for this phone: hidden', _bad),
    };
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 3),
      child: Row(
        children: [
          SizedBox(width: 64, child: Text('${f.fps} fps', style: const TextStyle(color: Colors.white, fontSize: 12.5, fontFamily: 'monospace'))),
          Expanded(child: Text(label, style: TextStyle(color: color, fontSize: 12.5))),
          Text('GPU ${f.gpuMs.toStringAsFixed(1)} / ${f.budgetMs.toStringAsFixed(1)} ms',
              style: const TextStyle(color: _grey, fontSize: 11, fontFamily: 'monospace')),
        ],
      ),
    );
  }
}

// ---------------------------------------------------------------------------
// Guided calibration ("Calibrate this phone")
// ---------------------------------------------------------------------------

enum CalibPhase { list, cover, running, done }

/// Everything the calibration overlay shows; the camera screen mutates it.
class CalibFlowState {
  CalibPhase phase = CalibPhase.list;
  final Set<String> selected = {};
  String? currentId;
  final Map<String, String> outcome = {}; // step id -> "done" / "failed" / "cancelled" / short note
  bool firstRun = false;
}

class CalibrateOverlay extends StatelessWidget {
  final CalibFlowState flow;
  final CalibrationRecord record;
  final Set<String> coveredByProfile;
  final double progress; // 0..1 of the running step, <0 unknown
  final String stage; // text under the progress bar
  final void Function(String id, bool run) onToggle;
  final VoidCallback onStart; // list -> cover instruction (or straight to running)
  final VoidCallback onReady; // cover instruction -> running
  final VoidCallback onCancel; // stop the running step
  final VoidCallback onClose;

  const CalibrateOverlay({
    super.key,
    required this.flow,
    required this.record,
    this.coveredByProfile = const {},
    this.progress = -1,
    this.stage = '',
    required this.onToggle,
    required this.onStart,
    required this.onReady,
    required this.onCancel,
    required this.onClose,
  });

  @override
  Widget build(BuildContext context) => switch (flow.phase) {
    CalibPhase.list => _list(),
    CalibPhase.cover => _cover(),
    CalibPhase.running => _running(),
    CalibPhase.done => _done(),
  };

  static String statusText(StepStatus s) => switch (s) {
    StepStatus.done => 'Done',
    StepStatus.providedForPhone => 'Provided for your phone',
    StepStatus.notDone => 'Not done',
    StepStatus.updateAvailable => 'Update available',
  };

  Widget _list() {
    final chosen = [for (final s in calibrationSteps) if (flow.selected.contains(s.id)) s];
    final minutes = chosen.fold<int>(0, (a, s) => a + s.minutes);
    return _frame(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Text(flow.firstRun ? 'Set up your phone' : 'Calibrate this phone',
              style: const TextStyle(color: Colors.white, fontSize: 19, fontWeight: FontWeight.w600)),
          const SizedBox(height: 6),
          const Text(
            'Every phone is a little different. These short measurements make the picture cleaner on yours. '
            'You can do them now or later in Settings › Image & NR › Calibrate this phone.',
            style: TextStyle(color: _grey, fontSize: 12.5, height: 1.4),
          ),
          const SizedBox(height: 10),
          Expanded(
            child: ListView(
              children: [
                for (final s in calibrationSteps) _stepCard(s),
                const SizedBox(height: 4),
                if (chosen.any((s) => s.needsLensCovered))
                  const Text('These steps need the lens covered; you will be asked once, and they run one after the other.',
                      style: TextStyle(color: _grey, fontSize: 11.5)),
              ],
            ),
          ),
          const SizedBox(height: 10),
          Row(
            mainAxisAlignment: MainAxisAlignment.end,
            children: [
              _button(flow.firstRun ? 'SKIP FOR NOW' : 'CLOSE', onClose, primary: false),
              const SizedBox(width: 10),
              _button(chosen.isEmpty ? 'NOTHING SELECTED' : 'RUN ${chosen.length} (~$minutes MIN)', chosen.isEmpty ? null : onStart),
            ],
          ),
        ],
      ),
    );
  }

  Widget _stepCard(CalibrationStep s) {
    final st = statusOf(s, record, coveredByProfile: coveredByProfile);
    final run = flow.selected.contains(s.id);
    final color = switch (st) {
      StepStatus.done => _good,
      StepStatus.providedForPhone => _good,
      StepStatus.notDone => Colors.amber,
      StepStatus.updateAvailable => Colors.amber,
    };
    return Container(
      margin: const EdgeInsets.only(bottom: 8),
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
      decoration: BoxDecoration(color: _card, border: Border.all(color: _line), borderRadius: BorderRadius.circular(10)),
      child: Row(
        children: [
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(children: [
                  Flexible(child: Text(s.title, style: const TextStyle(color: Colors.white, fontSize: 14, fontWeight: FontWeight.w600))),
                  const SizedBox(width: 8),
                  Text(statusText(st).toUpperCase(), style: TextStyle(color: color, fontSize: 10, fontWeight: FontWeight.bold, letterSpacing: 0.8)),
                ]),
                const SizedBox(height: 3),
                Text('${s.improves}  About ${s.minutes} min${s.needsLensCovered ? ', lens covered' : ''}.',
                    style: const TextStyle(color: _grey, fontSize: 11.5, height: 1.35)),
              ],
            ),
          ),
          const SizedBox(width: 10),
          SegmentedButton<bool>(
            showSelectedIcon: false,
            style: SegmentedButton.styleFrom(
              visualDensity: VisualDensity.compact,
              selectedBackgroundColor: Colors.amber,
              selectedForegroundColor: Colors.black,
              foregroundColor: Colors.white70,
              textStyle: const TextStyle(fontSize: 11.5),
            ),
            segments: [
              const ButtonSegment(value: false, label: Text('SKIP')),
              ButtonSegment(value: true, label: Text(st == StepStatus.done || st == StepStatus.providedForPhone ? 'REDO' : 'RUN')),
            ],
            selected: {run},
            onSelectionChanged: (v) => onToggle(s.id, v.first),
          ),
        ],
      ),
    );
  }

  Widget _cover() => _frame(
    child: Column(
      mainAxisAlignment: MainAxisAlignment.center,
      children: [
        const Icon(Icons.dark_mode, color: Colors.amber, size: 40),
        const SizedBox(height: 14),
        const Text('Cover the lens', style: TextStyle(color: Colors.white, fontSize: 19, fontWeight: FontWeight.w600)),
        const SizedBox(height: 8),
        const Text(
          'No light may reach the lens: lay the phone face down on a dark cloth, or use a lens cap with a cloth over it. '
          'Don\'t move it until it finishes. Your exposure settings are restored afterwards.',
          textAlign: TextAlign.center,
          style: TextStyle(color: _grey, fontSize: 13, height: 1.4),
        ),
        const SizedBox(height: 18),
        Row(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            _button('BACK', onClose, primary: false),
            const SizedBox(width: 10),
            _button('LENS COVERED: START', onReady),
          ],
        ),
      ],
    ),
  );

  Widget _running() {
    final step = calibrationSteps.where((s) => s.id == flow.currentId).firstOrNull;
    return _frame(
      child: Column(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          Text(step?.title ?? 'Calibrating…', style: const TextStyle(color: Colors.white, fontSize: 19, fontWeight: FontWeight.w600)),
          const SizedBox(height: 8),
          const Text('Keep the lens covered and the phone still.', style: TextStyle(color: _grey, fontSize: 13)),
          const SizedBox(height: 18),
          SizedBox(
            width: 280,
            child: LinearProgressIndicator(value: progress < 0 ? null : progress.clamp(0.0, 1.0), color: Colors.amber, backgroundColor: _line),
          ),
          const SizedBox(height: 8),
          Text(stage, style: const TextStyle(color: _grey, fontSize: 11.5, fontFamily: 'monospace')),
          const SizedBox(height: 18),
          _button('CANCEL', onCancel, primary: false),
        ],
      ),
    );
  }

  Widget _done() => _frame(
    child: Column(
      mainAxisAlignment: MainAxisAlignment.center,
      children: [
        const Icon(Icons.check_circle, color: _good, size: 40),
        const SizedBox(height: 12),
        const Text('Calibration finished', style: TextStyle(color: Colors.white, fontSize: 19, fontWeight: FontWeight.w600)),
        const SizedBox(height: 10),
        for (final s in calibrationSteps)
          if (flow.outcome.containsKey(s.id))
            Padding(
              padding: const EdgeInsets.symmetric(vertical: 2),
              child: Text('${s.title}: ${flow.outcome[s.id]}',
                  style: TextStyle(color: flow.outcome[s.id]!.startsWith('done') ? _good : Colors.amber, fontSize: 13)),
            ),
        const SizedBox(height: 18),
        _button('DONE', onClose),
      ],
    ),
  );
}

/// The "New calibration available" card shown once after an update.
Future<bool> showNewCalibrationCard(BuildContext context, List<CalibrationStep> pending) async {
  final run = await showDialog<bool>(
    context: context,
    barrierDismissible: false,
    builder: (ctx) => AlertDialog(
      backgroundColor: const Color(0xFF14171D),
      title: const Text('New calibration available', style: TextStyle(color: Colors.white, fontSize: 15)),
      content: Text(
        '${[for (final s in pending) '${s.title}: ${s.improves}'].join('\n\n')}'
        '\n\nAbout ${pending.fold<int>(0, (a, s) => a + s.minutes)} min.',
        style: const TextStyle(color: Colors.white70, fontSize: 12),
      ),
      actions: [
        TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('LATER')),
        TextButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('RUN NOW')),
      ],
    ),
  );
  return run == true;
}
