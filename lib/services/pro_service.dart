import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:google_mobile_ads/google_mobile_ads.dart';
import 'package:in_app_purchase/in_app_purchase.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// What the free plan allows (owner's decision, 7 Oct). Pro, or a "Pro pass"
/// earned with a rewarded ad (the next [ProService.proPassClips] clips), lifts
/// every limit. Gyroflow export will join this list when it exists.
class ProLimits {
  static const maxFps = 48.0; // 50 / 60 fps are Pro
  static const maxTemporalNr = 0.5; // up to LOW
  static const maxChromaNr = 0.5; // up to LOW
  static const maxSharpening = 2; // HIGH (3) is Pro
  static const maxRecordQuality = 0; // STANDARD only; HIGH / MAX are Pro
  // HQ oversampling, open-gate (4:3) recording and Native ISO Analysis are Pro.
}

/// The plan: free clips, ad, Pro. A take that is already recording is never
/// touched: everything is checked when the record button is pressed (or a
/// setting is chosen), never during a take.
class ProService extends ChangeNotifier {
  ProService._();
  static final ProService instance = ProService._();

  /// The one-time "Pro" product in Play Console (type: one-time product).
  static const productId = 'vesper_pro';

  /// Clips a free user can record before an ad is needed.
  static const freeClips = 5;

  /// Clips with every Pro feature after watching one ad.
  static const proPassClips = 3;

  // Google's public TEST rewarded ad unit and TEST app id (manifest). Replace
  // both with the AdMob ones: --dart-define=VESPER_REWARDED_AD_UNIT=ca-app-pub-.../...
  static const _adUnit = String.fromEnvironment('VESPER_REWARDED_AD_UNIT',
      defaultValue: 'ca-app-pub-3940256099942544/5224354917');

  static const _kPro = 'proUnlocked';
  static const _kClips = 'freeClipsUsed';
  static const _kPass = 'proPassClipsLeft';

  SharedPreferences? _prefs;
  StreamSubscription<List<PurchaseDetails>>? _sub;
  RewardedAd? _ad;
  bool _adLoading = false;
  bool _consentDone = false;
  bool _debugPro = false;

  bool _isPro = false;
  int _clipsUsed = 0;
  int _passLeft = 0;
  ProductDetails? _product;
  String _message = '';
  bool _busy = false;

  /// Pro is unlocked (bought on this Google account, or the developer override).
  bool get isPro => _isPro || _debugPro;
  int get clipsUsed => _clipsUsed;
  int get clipsLeft => isPro ? 999999 : (freeClips - _clipsUsed).clamp(0, freeClips);
  bool get canRecord => isPro || _passLeft > 0 || _clipsUsed < freeClips;

  /// Clips left of the ad-earned Pro pass.
  int get passClipsLeft => isPro ? 0 : _passLeft;

  /// Pro or a Pro pass: no feature limits.
  bool get hasProFeatures => isPro || _passLeft > 0;

  /// Price text from Google Play in the user's currency, e.g. "$14.99".
  String? get price => _product?.price;
  bool get storeAvailable => _product != null;
  bool get busy => _busy;

  /// Last purchase / ad message for the Pro page.
  String get message => _message;

  /// Developer builds: pretend Pro (sideloaded APKs cannot buy).
  bool get debugPro => _debugPro;
  set debugPro(bool v) {
    _debugPro = v;
    notifyListeners();
  }

  /// Reads the saved plan (fast, local). Awaited at start-up so settings are
  /// never limited by mistake before the plan is known.
  Future<void> loadLocal() async {
    try {
      _prefs = await SharedPreferences.getInstance();
      _isPro = _prefs!.getBool(_kPro) ?? false;
      _clipsUsed = _prefs!.getInt(_kClips) ?? 0;
      _passLeft = _prefs!.getInt(_kPass) ?? 0;
    } catch (e) {
      debugPrint('Pro plan not readable: $e');
    }
  }

  /// Asks Google Play for the price and past purchases (slow, not awaited).
  Future<void> init() async {
    try {
      if (_prefs == null) await loadLocal();
      notifyListeners();
      final iap = InAppPurchase.instance;
      _sub = iap.purchaseStream.listen(_onPurchases, onError: (Object e) => debugPrint('IAP stream: $e'));
      if (await iap.isAvailable()) {
        final r = await iap.queryProductDetails({productId});
        if (r.productDetails.isNotEmpty) _product = r.productDetails.first;
        // Quietly picks up a purchase made earlier on this Google account
        // (new phone, reinstall); the stream then delivers it.
        if (!_isPro) await iap.restorePurchases();
      }
      notifyListeners();
    } catch (e) {
      debugPrint('Pro service unavailable: $e');
    }
  }

  Future<void> _onPurchases(List<PurchaseDetails> list) async {
    for (final p in list) {
      if (p.productID != productId) continue;
      switch (p.status) {
        case PurchaseStatus.purchased:
        case PurchaseStatus.restored:
          await _setPro(true);
          _message = 'Vesper Pro is unlocked. Thank you!';
        case PurchaseStatus.pending:
          _message = 'Purchase pending…';
        case PurchaseStatus.error:
          _message = 'Purchase failed: ${p.error?.message ?? 'unknown error'}';
        case PurchaseStatus.canceled:
          _message = 'Purchase cancelled.';
      }
      if (p.pendingCompletePurchase) await InAppPurchase.instance.completePurchase(p);
      _busy = false;
      notifyListeners();
    }
  }

  Future<void> _setPro(bool v) async {
    _isPro = v;
    await _prefs?.setBool(_kPro, v);
  }

  /// Opens Google Play's purchase sheet.
  Future<void> buy() async {
    final p = _product;
    if (p == null) {
      _message = 'The Play Store is not reachable, or Pro is not available in this build (install from Google Play).';
      notifyListeners();
      return;
    }
    _busy = true;
    _message = '';
    notifyListeners();
    try {
      await InAppPurchase.instance.buyNonConsumable(purchaseParam: PurchaseParam(productDetails: p));
    } catch (e) {
      _busy = false;
      _message = 'Could not start the purchase: $e';
      notifyListeners();
    }
  }

  /// "Restore purchase": asks Play for purchases of this Google account.
  Future<void> restore() async {
    _busy = true;
    _message = 'Checking Google Play…';
    notifyListeners();
    try {
      await InAppPurchase.instance.restorePurchases();
      // Nothing arrives on the stream when there is no purchase: settle after a moment.
      await Future<void>.delayed(const Duration(seconds: 4));
      if (!_isPro && _message == 'Checking Google Play…') _message = 'No Pro purchase found on this Google account.';
    } catch (e) {
      _message = 'Could not reach Google Play: $e';
    }
    _busy = false;
    notifyListeners();
  }

  /// Count a clip that really started recording.
  Future<void> noteClipStarted() async {
    if (isPro) return;
    if (_passLeft > 0) {
      _passLeft--;
      await _prefs?.setInt(_kPass, _passLeft);
    } else {
      _clipsUsed++;
      await _prefs?.setInt(_kClips, _clipsUsed);
    }
    notifyListeners();
    if (clipsLeft <= 1) unawaited(preloadAd());
  }

  // --- rewarded ad ----------------------------------------------------------

  /// EU/UK users are asked for consent first (Google's User Messaging Platform).
  Future<bool> _ensureConsent() async {
    if (_consentDone) return true;
    final done = Completer<void>();
    ConsentInformation.instance.requestConsentInfoUpdate(
      ConsentRequestParameters(),
      () async {
        await ConsentForm.loadAndShowConsentFormIfRequired((_) {});
        if (!done.isCompleted) done.complete();
      },
      (_) {
        if (!done.isCompleted) done.complete();
      },
    );
    await done.future.timeout(const Duration(seconds: 60), onTimeout: () {});
    final ok = await ConsentInformation.instance.canRequestAds();
    if (ok) {
      _consentDone = true;
      await MobileAds.instance.initialize();
    }
    return ok;
  }

  /// Whether the user can change their ad choices (shown in the Pro page in the EEA/UK).
  Future<bool> privacyOptionsRequired() async =>
      await ConsentInformation.instance.getPrivacyOptionsRequirementStatus() ==
      PrivacyOptionsRequirementStatus.required;

  Future<void> showPrivacyOptions() => ConsentForm.showPrivacyOptionsForm((_) {});

  Future<void> preloadAd() async {
    if (_ad != null || _adLoading || isPro) return;
    _adLoading = true;
    try {
      if (!await _ensureConsent()) return;
      await RewardedAd.load(
        adUnitId: _adUnit,
        request: const AdRequest(),
        rewardedAdLoadCallback: RewardedAdLoadCallback(
          onAdLoaded: (ad) {
            _ad = ad;
            _adLoading = false;
          },
          onAdFailedToLoad: (e) {
            debugPrint('Rewarded ad failed: ${e.message}');
            _adLoading = false;
          },
        ),
      );
      // The callbacks above clear the flag; wait for them briefly.
      for (var i = 0; i < 100 && _adLoading; i++) {
        await Future<void>.delayed(const Duration(milliseconds: 100));
      }
    } catch (e) {
      debugPrint('Ad preload failed: $e');
    } finally {
      _adLoading = false;
    }
  }

  /// Shows a rewarded ad. True when the user earned the reward.
  Future<bool> _showRewardedAd() async {
    if (_ad == null) await preloadAd();
    final ad = _ad;
    if (ad == null) {
      _message = 'No ad available right now. Check your connection and try again, or unlock Pro.';
      notifyListeners();
      return false;
    }
    _ad = null;
    final result = Completer<bool>();
    var earned = false;
    ad.fullScreenContentCallback = FullScreenContentCallback<RewardedAd>(
      onAdDismissedFullScreenContent: (a) {
        a.dispose();
        if (!result.isCompleted) result.complete(earned);
      },
      onAdFailedToShowFullScreenContent: (a, e) {
        a.dispose();
        if (!result.isCompleted) result.complete(false);
      },
    );
    await ad.show(onUserEarnedReward: (_, _) => earned = true);
    return result.future;
  }

  /// Ad -> the free-clip counter is reset (free features only).
  Future<bool> watchAdForClips() async {
    final ok = await _showRewardedAd();
    if (ok) {
      _clipsUsed = 0;
      await _prefs?.setInt(_kClips, 0);
      _message = '';
    }
    notifyListeners();
    return ok;
  }

  /// Ad -> every Pro feature for the next [proPassClips] clips.
  Future<bool> watchAdForPass() async {
    final ok = await _showRewardedAd();
    if (ok) {
      _passLeft = proPassClips;
      await _prefs?.setInt(_kPass, _passLeft);
      _message = '';
    }
    notifyListeners();
    return ok;
  }

  @override
  void dispose() {
    _sub?.cancel();
    super.dispose();
  }
}
