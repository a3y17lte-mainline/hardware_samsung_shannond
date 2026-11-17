// SPDX-License-Identifier: GPL-2.0-or-later
package org.shannond.ims;

import android.telephony.ims.ImsService;
import android.telephony.ims.feature.ImsFeature;
import android.telephony.ims.feature.MmTelFeature;
import android.telephony.ims.stub.ImsConfigImplBase;
import android.telephony.ims.stub.ImsFeatureConfiguration;
import android.telephony.ims.stub.ImsRegistrationImplBase;
import android.util.Log;

/**
 * The device's ImsService: MMTEL (voice over LTE) on SIM slot 0. The SIP stack
 * runs in the radio HAL; everything here maps Android's IMS API to it.
 */
public class ShannonImsService extends ImsService {
    private static final String TAG = "ShannonIms";

    private ShannonMmTelFeature mMmTel;
    private ShannonRegistration mRegistration;
    private ShannonConfig mConfig;
    // IMS as the framework wants it (enableIms), and the voice capability
    // it enabled for LTE (MmTelFeature.changeEnabledCapabilities)
    private boolean mImsEnabled;
    private boolean mVoiceEnabled;

    @Override
    public void onCreate() {
        super.onCreate();
        Hal hal = Hal.get();
        mRegistration = new ShannonRegistration(hal);
        mConfig = new ShannonConfig();
        mMmTel = new ShannonMmTelFeature(this, hal, mRegistration);
    }

    @Override
    public ImsFeatureConfiguration querySupportedImsFeatures() {
        return new ImsFeatureConfiguration.Builder()
                .addFeature(0, ImsFeature.FEATURE_MMTEL)
                .build();
    }

    @Override
    public void readyForFeatureCreation() {
        Log.i(TAG, "ready for feature creation");
    }

    @Override
    public MmTelFeature createMmTelFeatureForSubscription(int slotId, int subscriptionId) {
        return slotId == 0 ? mMmTel : null;
    }

    @Override
    public ImsRegistrationImplBase getRegistrationForSubscription(int slotId, int subscriptionId) {
        return mRegistration;
    }

    @Override
    public ImsConfigImplBase getConfigForSubscription(int slotId, int subscriptionId) {
        return mConfig;
    }

    @Override
    public void enableImsForSubscription(int slotId, int subscriptionId) {
        Log.i(TAG, "enableIms slot " + slotId);
        mImsEnabled = true;
        update();
    }

    @Override
    public void disableImsForSubscription(int slotId, int subscriptionId) {
        Log.i(TAG, "disableIms slot " + slotId);
        mImsEnabled = false;
        update();
    }

    void setVoiceEnabled(boolean enabled) {
        mVoiceEnabled = enabled;
        update();
    }

    private void update() {
        Hal.get().setEnabled(mImsEnabled && mVoiceEnabled);
    }
}
