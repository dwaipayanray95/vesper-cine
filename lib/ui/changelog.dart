/// User-facing release notes shown in Settings > Info. Newest first.
/// Add an entry with every version bump (see CLAUDE.md).
class ChangelogEntry {
  final String version;
  final List<String> notes;
  const ChangelogEntry(this.version, this.notes);
}

const changelog = <ChangelogEntry>[
  ChangelogEntry('0.24.2', [
    'Behind the scenes: developer notes for the Play release (no change in the app)',
  ]),
  ChangelogEntry('0.24.1', [
    'Behind the scenes: privacy policy and Play Store release documents, ad settings now come from the build configuration (no change in the app)',
  ]),
  ChangelogEntry('0.24.0', [
    'Vesper Pro (Settings › Vesper Pro): every camera feature stays free. The free plan records 5 clips, then a short ad gives you 5 more; Pro is a one-time purchase with unlimited clips and no ads, restorable on any phone with your Google account',
    'A clip that is already recording is never stopped or interrupted by any of this',
  ]),
  ChangelogEntry('0.23.0', [
    'Crash reports (Settings › Info › Send crash reports, on by default, release builds): if the app crashes, anonymous technical details are sent so it can be fixed without anyone copying logs. Never footage, location or personal data',
  ]),
  ChangelogEntry('0.22.1', [
    'Behind the scenes: Play Store release bundle with a private signing key (no change in the app)',
  ]),
  ChangelogEntry('0.22.0', [
    'New app identity for the Play Store (com.theawesomeray.vespercine): this installs as a separate app next to the old one. Uninstall the old Vesper Cine after copying any clips you want; run Hot Pixel Calibration again in the new app',
  ]),
  ChangelogEntry('0.21.0', [
    'Image & NR › Hot Pixel Calibration: cover the lens for about a minute and this phone\'s own hot pixels are always removed (was developer-only)',
    'Image & NR › NR Character: TEXTURE (default, most fine detail) or SMOOTH (cleaner low-light footage, slightly softer texture). Was the developer switch "Calibrated Noise Model"',
  ]),
  ChangelogEntry('0.20.0', [
    'Calibrated Noise Model (Developer) is OFF by default and now only affects midtones and highlights: in a still daylight test it removed no visible grain but softened fine texture slightly. Shadows and high ISO always use the measured sensor noise',
    'The heat badge only appears when the phone is hot enough for the app to step processing down (HOT / VERY HOT); the old WARM stage changed nothing and is no longer shown',
  ]),
  ChangelogEntry('0.19.0', [
    'Recording Quality is HIGH by default: measured on your clips, the frames between keyframes now keep about 90% of the fine detail instead of 75%, so sharpness no longer "breathes" once a second',
    'For MAX quality use HEVC: on the Pixel 10 the HEVC encoder goes up to 240 Mb/s, the AV1 encoder is rated to 60 Mb/s',
  ]),
  ChangelogEntry('0.18.0', [
    'Recording Quality (Settings › Recording): STANDARD, HIGH (2x bitrate) or MAX (3x). At standard bitrate the encoder kept 35-45% less fine detail in the frames between its once-a-second keyframes (sharpness "breathing" in your 6 Oct clips)',
  ]),
  ChangelogEntry('0.17.0', [
    'Noise reduction, motion alignment and sharpening now use the Pixel 10\'s measured noise in bright areas too (the camera\'s own figure was about a third too low at every ISO): smoother midtones and skies, less sharpened grain',
    'Developer: Calibrated Noise Model ON/OFF switch to compare footage with the camera\'s own noise figure',
  ]),
  ChangelogEntry('0.16.0', [
    'Each phone calibrates itself: Sensor Calibration · Dark (Settings › Developer) now saves this phone\'s own hot-pixel map and noise table and uses them straight away (every phone has different hot pixels). Running it again adds any new hot pixels',
    'Without a Dark run the Pixel 10 still gets the measured noise correction for its sensor model; the hot-pixel map from 0.15.1 (one specific phone) is no longer shipped',
  ]),
  ChangelogEntry('0.15.1', [
    'Hot pixels: the ~650 known hot pixels of the Pixel 10 main camera (measured with the new dark calibration) are repaired before any processing, and the automatic repair catches far more of the rest. At high ISO about 95-100% are now removed (was ~12%)',
    'Noise reduction and motion alignment use the measured sensor noise: from ISO ~1000 up the camera\'s own figure was 40% too low in the shadows, so NR was too weak there',
  ]),
  ChangelogEntry('0.15.0', [
    'Sensor calibration without a colour chart (developer builds, Settings › Developer): DARK (lens covered) and WHITE (paper over the lens, daylight) sweeps measure noise, black level, lens shading, clip point, linearity and hot pixels; results go to Download/Vesper Calibration',
    'Calibration Frame also saves a DNG with the full camera metadata (Google AWB, noise profile, colour matrices, lens-shading map), and works with Settings open',
  ]),
  ChangelogEntry('0.14.1', [
    'Faster HQ oversampling (about 1.4 ms less GPU time per frame, same image)',
    'Heat safeguard keeps quality much longer: it steps down at most once a minute and only while the phone keeps heating up; noise reduction stays on until "severe"',
  ]),
  ChangelogEntry('0.14.0', [
    'Fixed: "swimming" grain in dark areas with motion alignment on (worst towards the frame edges). The motion search no longer mistakes noise for movement',
    'Heat safeguard: when Android forecasts the phone getting too hot, processing steps down gently (one stage every ~15 s) instead of the take being cut; at "severe" the take continues with minimal processing and stops only at "critical"',
    'Heat badge shows WARM / HOT / VERY HOT',
    'Viewfinder-off power saver: the REC text moves every minute so it can\'t burn into the OLED screen',
    'GPU A/B Test (developer builds): wide HQ tiles experiment',
  ]),
  ChangelogEntry('0.13.4', [
    'Safer recording: if the encoder or storage fails mid-take, the clip is finished and saved with everything recorded so far (instead of the app freezing)',
    'If the microphone delivers nothing, the take records without audio instead of filling the phone\'s memory',
    'A take cut off by a crash or the app being closed is no longer hidden: it shows up in the gallery at the next launch (…_INCOMPLETE.mp4 if it needs repair)',
  ]),
  ChangelogEntry('0.13.3', ['HQ detail is more accurate towards the frame corners: lens-shading correction now matches the rest of the image per pixel (no extra GPU cost)']),
  ChangelogEntry('0.13.2', ['GPU A/B Test (developer builds): each speed-up experiment against the normal path in alternating blocks, so the phone heating up cancels out']),
  ChangelogEntry('0.13.1', [
    'While recording, paused processing (e.g. HQ on a hot phone) comes back only with clear headroom and at most every 30 s: no more sharpness pumping in a clip',
    'Slightly faster viewfinder/recording step on the GPU',
    'GPU Benchmark corrects for the phone warming up during the run',
  ]),
  ChangelogEntry('0.13.0', [
    'The screen no longer dims or locks while Vesper is open',
    'Power Saver While Recording (Settings › Recording): dim the screen, or also pause the viewfinder, 10 s into a take; tap to wake. The recording is not affected',
  ]),
  ChangelogEntry('0.12.3', ['GPU Benchmark (developer builds) stops with a message if the camera restarts or the GPU guard steps in, instead of showing wrong numbers']),
  ChangelogEntry('0.12.2', ['GPU Benchmark (developer builds) tries exact vignetting in HQ, bigger HQ tiles and other GPU work-group sizes']),
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
