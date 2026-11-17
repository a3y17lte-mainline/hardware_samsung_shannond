// SPDX-License-Identifier: GPL-2.0-or-later
package org.shannond.ims;

import android.os.Bundle;
import android.os.RemoteException;
import android.telephony.TelephonyManager;
import android.telephony.ims.ImsCallProfile;
import android.telephony.ims.ImsCallSessionListener;
import android.telephony.ims.feature.CapabilityChangeRequest;
import android.telephony.ims.feature.MmTelFeature;
import android.telephony.ims.stub.ImsCallSessionImplBase;
import android.telephony.ims.stub.ImsRegistrationImplBase;
import android.util.Log;
import android.util.SparseArray;

import vendor.shannond.ims.Call;
import vendor.shannond.ims.CallState;
import vendor.shannond.ims.IShannonIms;
import vendor.shannond.ims.Registration;

/**
 * Voice over LTE. Calls are sessions of the HAL's SIP stack; this feature
 * creates them for the framework's outgoing calls, announces the HAL's
 * incoming ones and keeps the voice capability in step with the registration.
 */
final class ShannonMmTelFeature extends MmTelFeature implements Hal.Listener {
    private static final String TAG = "ShannonIms";

    private final ShannonImsService mService;
    private final Hal mHal;
    private final ShannonRegistration mRegistration;
    // The HAL's calls by id, while Android has a session for them
    private final SparseArray<ShannonCallSession> mSessions = new SparseArray<>();
    private boolean mVoiceEnabled;

    ShannonMmTelFeature(ShannonImsService service, Hal hal, ShannonRegistration registration) {
        mService = service;
        mHal = hal;
        mRegistration = registration;
        hal.addListener(this);
    }

    @Override
    public void onFeatureReady() {
        if (mHal.hal() != null) setFeatureState(STATE_READY);
    }

    @Override
    public void onFeatureRemoved() {
    }

    @Override
    public void onConnected() {
        setFeatureState(STATE_READY);
        Registration r = mHal.registration();
        if (r != null) onRegistrationChanged(r);
        // Calls the HAL has and we do not (this app restarted): a ringing
        // one is announced; any other has no call in Telecom to belong to
        IShannonIms hal = mHal.hal();
        for (Call call : mHal.calls()) {
            if (mSessions.get(call.id) != null) continue;
            if (call.state == CallState.INCOMING && call.incoming) {
                onCallChanged(call);
            } else if (hal != null) {
                Log.w(TAG, "call " + call.id + " has no session, ending it");
                try {
                    hal.hangup(call.id);
                } catch (RemoteException | RuntimeException e) {
                    Log.w(TAG, "hangup of call " + call.id + " failed", e);
                }
            }
        }
    }
    @Override
    public void onDisconnected() {
        // The HAL's calls died with it
        for (int i = mSessions.size() - 1; i >= 0; i--) mSessions.valueAt(i).lost();
        mSessions.clear();
        updateCapabilities();
    }

    @Override
    public boolean queryCapabilityConfiguration(int capability, int radioTech) {
        return capability == MmTelCapabilities.CAPABILITY_TYPE_VOICE
                && radioTech == ImsRegistrationImplBase.REGISTRATION_TECH_LTE && mVoiceEnabled;
    }

    @Override
    public void changeEnabledCapabilities(CapabilityChangeRequest request,
            CapabilityCallbackProxy c) {
        for (CapabilityChangeRequest.CapabilityPair p : request.getCapabilitiesToEnable())
            if (isVoiceOnLte(p)) mVoiceEnabled = true;
        for (CapabilityChangeRequest.CapabilityPair p : request.getCapabilitiesToDisable())
            if (isVoiceOnLte(p)) mVoiceEnabled = false;
        Log.i(TAG, "voice over LTE " + (mVoiceEnabled ? "enabled" : "disabled"));
        mService.setVoiceEnabled(mVoiceEnabled);
        updateCapabilities();
    }

    private static boolean isVoiceOnLte(CapabilityChangeRequest.CapabilityPair p) {
        return p.getCapability() == MmTelCapabilities.CAPABILITY_TYPE_VOICE
                && p.getRadioTech() == ImsRegistrationImplBase.REGISTRATION_TECH_LTE;
    }

    private void updateCapabilities() {
        MmTelCapabilities caps = new MmTelCapabilities();
        if (mVoiceEnabled && mRegistration.registered())
            caps.addCapabilities(MmTelCapabilities.CAPABILITY_TYPE_VOICE);
        notifyCapabilitiesStatusChanged(caps);
    }

    @Override
    public ImsCallProfile createCallProfile(int callSessionType, int callType) {
        return new ImsCallProfile(callSessionType, callType);
    }

    @Override
    public ImsCallSessionImplBase createCallSession(ImsCallProfile profile) {
        return new ShannonCallSession(this, mHal, profile, null);
    }

    @Override
    public int shouldProcessCall(String[] numbers) {
        // Emergency calls on CS, as the stock O2 profile has them
        TelephonyManager tm = mService.getSystemService(TelephonyManager.class);
        for (String n : numbers) {
            try {
                if (tm != null && tm.isEmergencyNumber(n)) return PROCESS_CALL_CSFB;
            } catch (RuntimeException e) {
                Log.w(TAG, "emergency number check failed", e);
            }
        }
        return PROCESS_CALL_IMS;
    }

    void track(int id, ShannonCallSession session) {
        mSessions.put(id, session);
    }

    void untrack(int id) {
        mSessions.remove(id);
    }

    @Override
    public void onRegistrationChanged(Registration registration) {
        // The registration object has seen this first (it listens before us)
        updateCapabilities();
    }

    @Override
    public void onCallChanged(Call call) {
        ShannonCallSession s = mSessions.get(call.id);
        if (s != null) {
            s.onCallChanged(call);
            return;
        }
        if (call.state != CallState.INCOMING || !call.incoming) return;
        // A new incoming call: a session for it, announced to the framework
        ImsCallProfile profile = new ImsCallProfile(ImsCallProfile.SERVICE_TYPE_NORMAL,
                ImsCallProfile.CALL_TYPE_VOICE);
        s = new ShannonCallSession(this, mHal, profile, call);
        mSessions.put(call.id, s);
        try {
            ImsCallSessionListener listener =
                    notifyIncomingCall(s, Integer.toString(call.id), new Bundle());
            if (listener != null) s.setListener(listener);
        } catch (RuntimeException e) {
            Log.e(TAG, "cannot announce call " + call.id + ", rejecting it", e);
            mSessions.remove(call.id);
            s.reject(0);
        }
    }
}
