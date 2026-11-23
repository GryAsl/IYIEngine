// Google Play licensing service, on-device side (vendored from google/play-licensing, Apache 2.0).
package com.android.vending.licensing;

import com.android.vending.licensing.ILicenseResultListener;

oneway interface ILicensingService {
  void checkLicense(long nonce, String packageName, in ILicenseResultListener listener);
}
