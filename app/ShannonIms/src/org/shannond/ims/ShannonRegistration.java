// SPDX-License-Identifier: GPL-2.0-or-later
package org.shannond.ims;

import android.net.Uri;
import android.telephony.ims.ImsReasonInfo;
import android.telephony.ims.stub.ImsRegistrationImplBase;

import vendor.shannond.ims.Call;
import vendor.shannond.ims.Registration;
import vendor.shannond.ims.RegistrationState;

/** The HAL's IMS registration, as Android's registration callbacks */
final class ShannonRegistration extends ImsRegistrationImplBase implements Hal.Listener {
    private int mState = -1;

    ShannonRegistration(Hal hal) {
        hal.addListener(this);
    }

    boolean registered() {
        return mState == RegistrationState.REGISTERED;
    }

    @Override
    public void onConnected() {
        Registration r = Hal.get().registration();
        if (r != null) onRegistrationChanged(r);
    }

    @Override
    public void onRegistrationChanged(Registration r) {
        if (r.state == mState) return;
        mState = r.state;
        switch (r.state) {
            case RegistrationState.REGISTERING:
                onRegistering(REGISTRATION_TECH_LTE);
                break;
            case RegistrationState.REGISTERED:
                onRegistered(REGISTRATION_TECH_LTE);
                if (r.associatedUris != null) {
                    Uri[] uris = new Uri[r.associatedUris.length];
                    for (int i = 0; i < uris.length; i++) uris[i] = Uri.parse(r.associatedUris[i]);
                    onSubscriberAssociatedUriChanged(uris);
                }
                break;
            default:
                onDeregistered(new ImsReasonInfo(ImsReasonInfo.CODE_LOCAL_NOT_REGISTERED, 0,
                        r.reason));
                break;
        }
    }

    @Override
    public void onDisconnected() {
        if (mState == -1 || mState == RegistrationState.IDLE) return;
        mState = RegistrationState.IDLE;
        onDeregistered(new ImsReasonInfo(ImsReasonInfo.CODE_LOCAL_NOT_REGISTERED, 0, "radio HAL gone"));
    }
    @Override
    public void onCallChanged(Call call) {
    }
}
