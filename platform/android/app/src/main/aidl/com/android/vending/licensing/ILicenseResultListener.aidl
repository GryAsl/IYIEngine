// Google Play licensing result callback (vendored from google/play-licensing, Apache 2.0).
package com.android.vending.licensing;

oneway interface ILicenseResultListener {
  void verifyLicense(int responseCode, String signedData, String signature);
}
