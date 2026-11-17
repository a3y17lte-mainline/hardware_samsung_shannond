// SPDX-License-Identifier: GPL-2.0-or-later
package vendor.shannond.ims;

/**
 * VoLTE in the radio HAL (shannond): the SIP stack, the IMS PDN and the media
 * setup live in the HAL. The ImsService app switches IMS on and off, follows
 * the registration and drives the calls. Instance "default".
 */
@VintfStability
interface IShannonIms {
    /** Service specific errors */
    const int ERROR_NOT_READY = 1;
    const int ERROR_NO_SUCH_CALL = 2;
    const int ERROR_REFUSED = 3;

    /** One callback at a time; the current state is not replayed (ask for it) */
    void setCallback(in vendor.shannond.ims.IShannonImsCallback callback);

    /** The user's VoLTE switch. Off after every start of the HAL. */
    void setEnabled(in boolean enabled);

    vendor.shannond.ims.Registration getRegistration();

    /** The calls that are not ENDED */
    vendor.shannond.ims.Call[] getCalls();

    /** Places a call; its progress comes through onCallChanged. Returns the call's id. */
    int dial(in String number);

    void answer(in int id);

    /** Ends the call, cancels an outgoing one or rejects an incoming one */
    void hangup(in int id);

    void hold(in int id);

    void resume(in int id);

    /** One DTMF digit: 0-9, * or # */
    void sendDtmf(in int id, in char digit);
}
