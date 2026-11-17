// SPDX-License-Identifier: GPL-2.0-or-later
package vendor.shannond.ims;

/** From the HAL to the ImsService app. Nothing here waits for the app. */
@VintfStability
oneway interface IShannonImsCallback {
    void onRegistrationChanged(in vendor.shannond.ims.Registration registration);
    /** A call changed; a call first seen in state INCOMING is a new incoming call */
    void onCallChanged(in vendor.shannond.ims.Call call);
}
