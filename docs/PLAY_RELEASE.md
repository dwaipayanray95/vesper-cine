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

GitHub → **Actions** → **Build arm64 APK** → **Run workflow** → tick **build_aab** → Run.
(`build_mode` and `dev_tools` are ignored for the bundle: always release, no developer tools.)
When it finishes, open the run, scroll to **Artifacts**, download `vesper-cine-v…-build….aab` (it's a zip containing the .aab). The run summary shows the certificate fingerprint of the signing key.
The bundle is a private artifact, not a public GitHub Release.

## 4. Create the app in Play Console (one time)

1. https://play.google.com/console → **Create app**. Name: *Vesper Cine*; default language; **App** (not game); **Free** (cannot change later; Pro is sold as an in-app purchase); accept the declarations.
2. Left menu **Test and release** → **Testing** → **Internal testing** → **Create new release**. Play shows **"Play App Signing"**: choose *Use Google-generated key* (the default), upload the `.aab`, release name = the version, **Save → Review → Start rollout to Internal testing**. Internal testing needs no review, ready in minutes; it's the fastest way to check the bundle installs.
3. The closed-test release (Testing → Closed testing) is the same upload, plus a tester list (see the checklist at the end of this guide, added with the store listing).
