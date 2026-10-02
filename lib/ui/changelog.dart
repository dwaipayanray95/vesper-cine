/// User-facing release notes shown in Settings > Info. Newest first.
/// Add an entry with every version bump (see CLAUDE.md).
class ChangelogEntry {
  final String version;
  final List<String> notes;
  const ChangelogEntry(this.version, this.notes);
}

const changelog = <ChangelogEntry>[
  ChangelogEntry('0.12.1', ['No dropped frame when recording starts (the viewfinder no longer changes size)']),
  ChangelogEntry('0.12.0', [
    'At 30 fps temporal NR keeps its motion alignment: when the GPU is tight it runs every 4th frame (ALIGN REDUCED) instead of switching off',
    'Faster motion alignment (about 1–1.5 ms less GPU time per frame)',
  ]),
  ChangelogEntry('0.11.6', [
    'Raw-copy experiment from 0.11.5 removed (it was slower)',
    'GPU Benchmark (developer builds) breaks HQ and alignment down further and tries three speed-ups',
  ]),
  ChangelogEntry('0.11.5', [
    'Noise-reduction change from 0.11.4 undone (it measured slower on the phone)',
    'GPU Benchmark (developer builds) measures the cost of each GPU pass and scrolls when long',
  ]),
  ChangelogEntry('0.11.4', [
    'Faster temporal and chroma noise reduction on the GPU (identical image)',
    'GPU Benchmark compares against the 0.11.3 and 0.11.2 shaders; app log starts with the app version',
  ]),
  ChangelogEntry('0.11.3', [
    'Faster HQ oversampling and sharpening on the GPU (identical image)',
    'GPU Benchmark (developer builds) ends with an A/B: new vs previous shaders',
  ]),
  ChangelogEntry('0.11.2', ['Settings: each category opens scrolled to the top']),
  ChangelogEntry('0.11.0', [
    'New Settings layout: categories on the left, their settings on the right',
    'Info tab: version, changelog, how-to and FAQ',
    'GPU guard is always on in release builds (developer option otherwise)',
  ]),
  ChangelogEntry('0.10.5', ['Cleaner image corners in dark scenes: noise reduction follows the lens-shading gain']),
  ChangelogEntry('0.10.4', ['Changing the frame rate turns all processing back on, then pauses only what that rate can\'t carry']),
  ChangelogEntry('0.10.1', ['Viewfinder drawn at its on-screen size: no swirly moiré in noise']),
  ChangelogEntry('0.10.0', ['In-app log for testing (developer builds)']),
  ChangelogEntry('0.9.0', [
    'GPU guard brings paused processing back when it fits again',
    'The guard is always active while recording; warning if a frame rate can\'t be sustained',
  ]),
  ChangelogEntry('0.8.3', ['Faster temporal noise reduction and HQ oversampling (16-bit maths)']),
  ChangelogEntry('0.8.1', ['Less viewfinder lag: the newest frame is always shown']),
  ChangelogEntry('0.8.0', ['Smooth viewfinder: drawn straight to the display']),
  ChangelogEntry('0.7.0', ['Faster HQ oversampling, processing pauses while Settings is open, microphone required']),
  ChangelogEntry('0.6.0', ['Both landscape orientations, clean auto-exposure (native ISO first), updates install over each other']),
  ChangelogEntry('0.5.0', ['HQ oversampling: full-sensor detail, anti-aliased to 1080p']),
  ChangelogEntry('0.4.0', ['Sharper image (bicubic + noise-aware sharpening), focus magnifier (MAG)']),
];
