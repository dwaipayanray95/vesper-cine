# Google Play release guide (owner's checklist)

Package name: `com.theawesomeray.vespercine` (cannot be changed once the app exists in Play Console).

## 1. Signing: how it works

| Key | Where it lives | Used for |
|---|---|---|
| **Dev key** `android/app/vesper-dev.jks` | In the repo (public on purpose) | Test APKs from "Build arm64 APK" so they install over each other. **Never** use it for Play. |
| **Upload key** (new, private) | Only on your computer (backup!) and as 4 GitHub secrets | Signs the `.aab` you upload to Play. |
| **App signing key** | Held by Google (Play App Signing) | Google re-signs what users download. If you ever lose the upload key, Play support can reset it; if Google held no key you'd lose the app. |

## 2. Create the upload key and put it in GitHub (one time)

You need Java's `keytool`. It comes with Android Studio (`<Android Studio>/jbr/bin/keytool`) or any JDK 17.

1. Open a terminal and run (pick two strong passwords; using the same one twice is fine):
   ```
   keytool -genkeypair -v -keystore upload.jks -alias upload -keyalg RSA -keysize 2048 -validity 10000
   ```
   Answer the questions (name etc.; it only appears inside the certificate).
2. **Back up `upload.jks` and both passwords** in your password manager (e.g. as an attachment). Never put the file in the repository.
3. Turn the file into text:
   - Mac / Linux: `base64 -w0 upload.jks > upload.jks.b64` (Mac: `base64 -i upload.jks -o upload.jks.b64`)
   - Windows PowerShell: `[Convert]::ToBase64String([IO.File]::ReadAllBytes("upload.jks")) | Set-Content upload.jks.b64`
4. GitHub → your repo **vesper-cine** → **Settings** → **Secrets and variables** → **Actions** → **New repository secret**. Add these four (names exactly):
   - `VESPER_UPLOAD_KEYSTORE_BASE64` = the whole content of `upload.jks.b64`
   - `VESPER_UPLOAD_STORE_PASSWORD` = the keystore password
   - `VESPER_UPLOAD_KEY_ALIAS` = `upload`
   - `VESPER_UPLOAD_KEY_PASSWORD` = the key password
5. Delete `upload.jks.b64` afterwards (keep `upload.jks` safe).

## 3. Build the Play bundle

GitHub → **Actions** → **Build arm64 APK** → **Run workflow**. The APK (dev key, so test APKs still install over each other) is always built. **Tick `build_aab`** to also build the Play bundle `.aab`, signed with the private upload key. `build_mode` and `dev_tools` only affect the APK; the bundle is always release, no developer tools. If you tick it before the four upload-key secrets exist, the bundle job fails with a message saying which one is missing (the APK is unaffected).
When it finishes, open the run, scroll to **Artifacts**, download `vesper-cine-v…-build….aab` (it's a zip containing the .aab). The run summary shows the certificate fingerprint of the signing key. The APK is published as a GitHub Release as before; the bundle stays a private artifact.

## 4. Create the app in Play Console (one time)

1. https://play.google.com/console → **Create app**. Name: *Vesper Cine*; default language; **App** (not game); **Free** (cannot change later; Pro is sold as an in-app purchase); accept the declarations.
2. Left menu **Test and release** → **Testing** → **Internal testing** → **Create new release**. Play shows **"Play App Signing"**: choose *Use Google-generated key* (the default), upload the `.aab`, release name = the version, **Save → Review → Start rollout to Internal testing**. Internal testing needs no review, ready in minutes; it's the fastest way to check the bundle installs.
3. The closed-test release (Testing → Closed testing) is the same upload, plus a tester list (see the checklist at the end of this guide, added with the store listing).

---

## 5. Services you set up once (Firebase, AdMob, Play product)

### 5a. Firebase Crashlytics (crash reports)
1. https://console.firebase.google.com → **Create a project** (name e.g. *Vesper Cine*; Google Analytics can stay off).
2. In the project: **Add app → Android**. Package name exactly `com.theawesomeray.vespercine`. Nickname *Vesper Cine*. Skip the SHA field. **Register app**, then **Download google-services.json**. Ignore the "add SDK" steps (already done).
3. In GitHub: open the repo → folder `android/app` → **Add file → Upload files** → upload your `google-services.json` (it replaces the placeholder; it is not a secret) → Commit.
   *Alternative:* Settings → Secrets → new secret `GOOGLE_SERVICES_JSON_BASE64` with the file base64-encoded (same method as the keystore).
4. Firebase console → **Release & Monitor → Crashlytics → Enable**. Crashes of builds from Play appear there a few minutes after the app restarts. Native (C++) crashes are included; symbols are uploaded automatically once the real file is in place.

### 5b. AdMob (rewarded ads)
1. https://admob.google.com → sign in with the same Google account → accept terms → payments profile (needed to get paid).
2. **Apps → Add app → Android**. Say it is *not yet published* (link it to the store listing later). Name *Vesper Cine*. Copy the **App ID** (`ca-app-pub-1234~5678`).
3. In the app: **Ad units → Add ad unit → Rewarded**. Name *free_clips_reward*. Reward: 5 *clips* (the number is only a label). Copy the **Ad unit ID** (`ca-app-pub-1234/5678`).
4. GitHub → Settings → Secrets and variables → Actions → tab **Variables** → add `VESPER_ADMOB_APP_ID` and `VESPER_REWARDED_AD_UNIT` with those two values.
5. AdMob → **Privacy & messaging → GDPR**: create the consent message for EEA/UK and publish it (the app already calls it). Add **US states** messaging if you like.
Until step 4 the bundle shows Google's **test ads** (safe, no revenue). The workflow prints a warning when the IDs are missing. **Never go to production with test ads.**

### 5c. The Pro product in Play Console
1. Needs the app created and **one bundle uploaded**, and a payments profile (Play Console → *Setup → Payments profile*).
2. **Monetize with Play → Products → One-time products → Create**: Product ID **`vesper_pro`** (exact), name *Vesper Pro*, description *Unlimited clips, no ads. One-time purchase.* Set the **price** (default country, then *Edit prices* for other countries; Play converts automatically and you can override per country). Activate it.
3. **Testing purchases:** Play Console → *Settings → License testing* → add your testers' Gmail addresses. They can then buy Pro on the closed-test build without being charged. Purchases only work for builds installed **from Google Play** (internal/closed test link), not for sideloaded APKs.

## 6. Privacy policy on GitHub Pages
1. Edit `docs/privacy-policy.md`: fill in `[YOUR NAME / STUDIO]` and `[YOUR CONTACT EMAIL]` (a real, monitored address; Play checks).
2. GitHub → repo **Settings → Pages** → Source: *Deploy from a branch*, Branch `main`, folder **/docs** → Save.
3. After a minute the policy is at `https://dwaipayanray95.github.io/vesper-cine/privacy-policy` (use exactly the address Pages shows). That URL goes into Play Console *App content → Privacy policy*.
Note: the Pages site is public, and so is the repo's `docs/` folder.

## 7. Play Console "Data safety" answers (copy these)

Play Console → *App content → Data safety*. Answer as follows (re-check if you change SDKs):

- **Does your app collect or share any of the required user data types?** Yes.
- **Is all of the user data collected by your app encrypted in transit?** Yes.
- **Do you provide a way for users to request that their data be deleted?** Yes (email in the privacy policy; crash data is also removed by uninstalling + request).
- **Data types collected** (tick, then for each: *collected*, *processing is not optional?* see notes):

| Data type | Collected | Shared | Purpose | Optional? |
|---|---|---|---|---|
| App info and performance → **Crash logs** | Yes (Firebase Crashlytics) | No (processor acting for you) | App functionality / Analytics | Yes: user can switch it off |
| App info and performance → **Diagnostics** | Yes (Crashlytics) | No | Analytics | Yes |
| Device or other IDs → **Device or other IDs** (Crashlytics installation ID; advertising ID for AdMob) | Yes | **Yes** (advertising ID goes to Google AdMob) | Advertising; App functionality | No for ads on the free plan |
| Location → **Approximate location** (AdMob, from IP) | Yes | **Yes** (AdMob) | Advertising | No for ads on the free plan |
| App activity → **App interactions** (ad interactions) | Yes (AdMob) | **Yes** (AdMob) | Advertising | No |
| Financial info → *Purchase history* | **Not collected by you** (Google Play handles it). Leave unticked. | | | |

- **Not collected:** photos/videos, audio, contacts, precise location, personal info, messages. Camera/microphone content stays on the phone and is never sent off the device ("collected" means leaving the device).
- **Ads:** *App content → Ads* → **Yes, my app contains ads**.
- **Target audience:** 13+ / 18+ (not designed for children; avoids the Families policy).
- **Advertising ID declaration:** Yes, used for advertising.

## 8. Store listing text (paste)

**App name (max 30):** `Vesper Cine: RAW Log Camera`

**Short description (max 80):** `RAW sensor video in Apple Log-compatible 10-bit HEVC/AV1. Made for Pixel.`

**Full description (max 4000):**
```
Vesper Cine is a cinema camera for Google Pixel phones. It reads the sensor's RAW data directly and develops it on the GPU with its own colour science, so you get flat, grade-ready footage instead of the phone's processed video.

WHAT YOU GET
• RAW10 sensor capture developed to an Apple Log-compatible, Rec.2020 master
• 10-bit HEVC or AV1 recording with audio, up to 240 Mb/s (HEVC)
• 23.976 to 60 fps, 16:9 or 4:3 sensor crop, manual ISO, shutter (angle or speed) and white balance
• HQ oversampling: full-sensor detail anti-aliased to 1080p
• Temporal and chroma noise reduction with motion alignment, tuned to your phone's measured sensor noise
• Per-phone hot-pixel calibration (cover the lens for a minute)
• Peaking, zebra, false colour, histogram and waveform, 3x magnifier, REC.709 preview LUT
• PDAF tap focus (track or lock), face detection
• Smart heat protection that steps processing down gently so long takes keep going
• Recording power saver: dim screen or pause the viewfinder on long takes

FREE AND PRO
Every camera feature is free. The free plan records 5 clips at a time, then a short ad gives you 5 more. Vesper Pro is a one-time purchase: unlimited clips and no ads.

IN POST
Set the input colour space to Apple Log and the gamut to Rec.2020 in your editing software. LUTs and a DCTL are available on the project page.

DEVICES
Built and tested on Pixel 10. It needs a phone with RAW10 camera output; other models may not work yet.

PRIVACY
Your footage never leaves your phone. Optional anonymous crash reports (can be turned off). See the privacy policy.

Apple Log is a trademark of Apple Inc. Vesper Cine is not affiliated with or endorsed by Apple.
```

**Category:** Photography (or Video Players & Editors). **Tags:** camera, video. **Contact email:** a monitored address. **Graphics you must make:** app icon 512×512, feature graphic 1024×500, 2–8 phone screenshots (landscape is fine), take them from the Pixel 10.

## 9. Your checklist in Play Console (what only you can do)

**Account (once)**
- [ ] Register at https://play.google.com/console/signup: **Personal** account, US$25 one-time fee, verify identity (government ID) and a phone number and the Android device. Use a developer name you're happy to show publicly. Personal accounts also show your address.
- [ ] Payments profile for selling Pro (Setup → Payments profile) and tax details.

**Create the app and fill in the required forms** (Dashboard → "Set up your app")
- [ ] App access: *All functionality is available without special access*.
- [ ] Ads: *Yes* · Content rating questionnaire (camera app, no violence/user content: expect "Everyone"; answer honestly) · Target audience: 13+ or 18+ · Data safety (section 7) · Privacy policy URL (section 6) · Government apps: No · Financial features: None · Health: None · News: No.
- [ ] Store listing (section 8): text, icon, feature graphic, screenshots.
- [ ] **Countries/regions** (Release → Production → Countries, also for tests) and **pricing** of `vesper_pro` per country (5c).

**Restrict devices to Pixels you can vouch for** (only Pixel 10 is tested)
- [ ] Play Console → **Release → Setup → Device catalog** (older UI: *Release → Device catalog*; Test and release → Advanced settings → *Device catalog*). The app already needs a RAW10-capable camera (the manifest says so, so Play hides it from phones without one). Add a **device exclusion rule**: *Exclude: manufacturer is not Google* or, stricter, exclude every model except *Pixel 10*, *Pixel 10 Pro*, *Pixel 10 Pro XL* (only Pixel 10 is tested; add others after a tester confirms they work).
- [ ] Also state "Tested on Pixel 10" at the top of the full description (already in the text above).

**Closed test (personal account rule)**
- [ ] Personal accounts created after Nov 2023 must run a closed test with **at least 12 testers opted in for 14 continuous days** before they can apply for production.
- [ ] Testing → **Closed testing → Create track** (default *Alpha*). Upload the `.aab` (section 3), release notes, countries.
- [ ] **Testers** tab: create an email list (12+ Gmail addresses; invite 15–20 so you stay above 12 if someone leaves) or use a Google Group. Copy the **opt-in link** and send it to them. Each tester must open the link, accept, and install from Play, and **stay opted in 14 days**. Ask them to open the app now and then (not required by Play, but it makes the production application look good).
- [ ] Note: only Pixels will see it; testers with other phones can accept the link but can't install. Make sure all 12 own a supported Pixel (or loosen the device rule for the test period).
- [ ] For testing purchases, add their addresses under *Settings → License testing* (5c).
- [ ] When the 14 days are over: **Dashboard → Apply for production** (questions about the test, feedback you received, readiness). Google reviews in about 7 days or less.

**Before production**
- [ ] AdMob IDs set (5b) so real ads, not test ads, are in the bundle.
- [ ] Real `google-services.json` committed (5a).
- [ ] Pro price set; `vesper_pro` active.
- [ ] Privacy policy live, with your name and email filled in.
- [ ] Production → Create release → upload latest `.aab` (bump `version` first: every upload needs a higher build number; the build number increases with every change I make) → Review → Roll out (start at 20% if you like).

**Versions note:** the Play bundle is signed by the upload key; testers who installed the old sideloaded APK (`com.vesper.cine`, signed with the dev key) have a different app and must uninstall it, and the Play version can't be installed over a sideloaded copy of the *same* package signed with another key (use only one or the other on a phone).
