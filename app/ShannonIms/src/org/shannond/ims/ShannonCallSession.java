// SPDX-License-Identifier: GPL-2.0-or-later
package org.shannond.ims;

import android.os.Message;
import android.os.RemoteException;
import android.os.ServiceSpecificException;
import android.telephony.ims.ImsCallProfile;
import android.telephony.ims.ImsCallSessionListener;
import android.telephony.ims.ImsReasonInfo;
import android.telephony.ims.ImsStreamMediaProfile;
import android.telephony.ims.stub.ImsCallSessionImplBase;
import android.util.Log;

import vendor.shannond.ims.Call;
import vendor.shannond.ims.CallState;
import vendor.shannond.ims.IShannonIms;

/** One call of the HAL's SIP stack, as Android's IMS call session */
final class ShannonCallSession extends ImsCallSessionImplBase {
    private static final String TAG = "ShannonIms";

    private final ShannonMmTelFeature mFeature;
    private final Hal mHal;
    private final ImsCallProfile mProfile;
    private ImsCallSessionListener mListener;
    private int mId = -1;
    private int mState = State.IDLE;
    private int mCallState = -1;
    private boolean mEstablished;
    private boolean mRemoteHeld;
    private boolean mLocalEnd;
    private boolean mEarlyMedia;
    // A hold or resume the HAL took and has not answered: 1 hold, 2 resume
    private int mPendingHold;

    /** call: the HAL's incoming call, null for an outgoing one */
    ShannonCallSession(ShannonMmTelFeature feature, Hal hal, ImsCallProfile profile, Call call) {
        mFeature = feature;
        mHal = hal;
        mProfile = profile;
        if (call != null) {
            mId = call.id;
            mCallState = call.state;
            mState = State.INITIATED;
            setCaller(call);
        }
    }

    private void setCaller(Call call) {
        // The number, else presentation restricted (withheld)
        if (call.number != null && !call.number.isEmpty()) {
            mProfile.setCallExtra(ImsCallProfile.EXTRA_OI, call.number);
            mProfile.setCallExtraInt(ImsCallProfile.EXTRA_OIR,
                    ImsCallProfile.OIR_PRESENTATION_NOT_RESTRICTED);
        } else {
            mProfile.setCallExtraInt(ImsCallProfile.EXTRA_OIR,
                    ImsCallProfile.OIR_PRESENTATION_RESTRICTED);
        }
    }

    @Override
    public void setListener(ImsCallSessionListener listener) {
        mListener = listener;
    }

    @Override
    public String getCallId() {
        return mId >= 0 ? Integer.toString(mId) : null;
    }

    @Override
    public ImsCallProfile getCallProfile() {
        return mProfile;
    }

    @Override
    public ImsCallProfile getLocalCallProfile() {
        return mProfile;
    }

    @Override
    public ImsCallProfile getRemoteCallProfile() {
        return mProfile;
    }

    @Override
    public int getState() {
        return mState;
    }

    @Override
    public boolean isInCall() {
        return mState == State.ESTABLISHED;
    }

    @Override
    public void start(String callee, ImsCallProfile profile) {
        IShannonIms hal = mHal.hal();
        try {
            if (hal == null) throw new IllegalStateException("no radio HAL");
            // Number withheld for this call (CLIR): the dial prefix the HAL reads
            ImsCallProfile p = profile != null ? profile : mProfile;
            boolean hide = p.getCallExtraInt(ImsCallProfile.EXTRA_OIR,
                    ImsCallProfile.OIR_PRESENTATION_NOT_RESTRICTED)
                    == ImsCallProfile.OIR_PRESENTATION_RESTRICTED;
            mId = hal.dial(hide ? "#31#" + callee : callee);
            mState = State.INITIATED;
            mFeature.track(mId, this);
            if (mListener != null) mListener.callSessionInitiating(mProfile);
        } catch (RemoteException | RuntimeException e) {
            // IMS cannot take it: the framework places it on CS
            Log.w(TAG, "dial failed, retry on CS", e);
            mState = State.TERMINATED;
            if (mListener != null)
                mListener.callSessionInitiatingFailed(
                        new ImsReasonInfo(ImsReasonInfo.CODE_LOCAL_CALL_CS_RETRY_REQUIRED, 0));
        }
    }

    private interface Request {
        void run(IShannonIms hal) throws RemoteException;
    }

    private boolean request(String what, Request r) {
        IShannonIms hal = mHal.hal();
        if (hal == null || mId < 0) return false;
        try {
            r.run(hal);
            return true;
        } catch (RemoteException | ServiceSpecificException e) {
            Log.w(TAG, what + " of call " + mId + " failed: " + e);
            return false;
        }
    }

    @Override
    public void accept(int callType, ImsStreamMediaProfile profile) {
        request("answer", hal -> hal.answer(mId));
    }

    @Override
    public void reject(int reason) {
        mLocalEnd = true;
        request("reject", hal -> hal.hangup(mId));
    }

    @Override
    public void terminate(int reason) {
        mLocalEnd = true;
        mState = State.TERMINATING;
        if (!request("hangup", hal -> hal.hangup(mId)) && mListener != null) {
            mState = State.TERMINATED;
            mListener.callSessionTerminated(new ImsReasonInfo(ImsReasonInfo.CODE_USER_TERMINATED, 0));
        }
    }

    @Override
    public void hold(ImsStreamMediaProfile profile) {
        if (request("hold", hal -> hal.hold(mId)))
            mPendingHold = 1;
        else if (mListener != null)
            mListener.callSessionHoldFailed(new ImsReasonInfo(ImsReasonInfo.CODE_UNSPECIFIED, 0));
    }

    @Override
    public void resume(ImsStreamMediaProfile profile) {
        if (request("resume", hal -> hal.resume(mId)))
            mPendingHold = 2;
        else if (mListener != null)
            mListener.callSessionResumeFailed(new ImsReasonInfo(ImsReasonInfo.CODE_UNSPECIFIED, 0));
    }

    @Override
    public void sendDtmf(char c, Message result) {
        request("DTMF", hal -> hal.sendDtmf(mId, c));
        if (result != null) result.sendToTarget();
    }

    @Override
    public void startDtmf(char c) {
        // One event per key press: the HAL sends it with its standard length
        request("DTMF", hal -> hal.sendDtmf(mId, c));
    }

    @Override
    public void stopDtmf() {
    }

    @Override
    public void setMute(boolean muted) {
        // Nothing here, as the stock IMS service: Telecom mutes through the
        // audio HAL, which mutes the modem
    }

    /** The HAL died with the call */
    void lost() {
        if (mState == State.TERMINATED) return;
        mState = State.TERMINATED;
        if (mListener != null)
            mListener.callSessionTerminated(new ImsReasonInfo(ImsReasonInfo.CODE_UNSPECIFIED, 0,
                    "radio HAL gone"));
    }
    /** A change of the HAL's call (main thread) */
    void onCallChanged(Call call) {
        final int previous = mCallState;
        mCallState = call.state;
        ImsCallSessionListener l = mListener;
        // The HAL reports a failed hold or resume as the call unchanged
        if (mPendingHold != 0 && call.state == previous
                && (call.state == CallState.ACTIVE || call.state == CallState.HELD)
                && call.remoteHeld == mRemoteHeld) {
            final boolean hold = mPendingHold == 1;
            mPendingHold = 0;
            if (hold == (call.state == CallState.ACTIVE) && l != null) {
                ImsReasonInfo why = new ImsReasonInfo(ImsReasonInfo.CODE_UNSPECIFIED, 0);
                if (hold) l.callSessionHoldFailed(why);
                else l.callSessionResumeFailed(why);
            }
        } else if (call.state != previous) {
            mPendingHold = 0;
        }
        switch (call.state) {
            case CallState.DIALING:
            case CallState.ALERTING: {
                // Progress: the phone plays its own ringback unless the network
                // plays one (early media: audio direction send-receive)
                final boolean early = call.earlyMedia != mEarlyMedia;
                mEarlyMedia = call.earlyMedia;
                final boolean alerting = call.state == CallState.ALERTING && previous != CallState.ALERTING;
                if (l != null && (alerting || (early && (call.state == CallState.ALERTING || mEarlyMedia)))) {
                    l.callSessionProgressing(new ImsStreamMediaProfile(
                            ImsStreamMediaProfile.AUDIO_QUALITY_AMR_WB,
                            mEarlyMedia ? ImsStreamMediaProfile.DIRECTION_SEND_RECEIVE
                                        : ImsStreamMediaProfile.DIRECTION_INACTIVE,
                            ImsStreamMediaProfile.VIDEO_QUALITY_NONE,
                            ImsStreamMediaProfile.DIRECTION_INVALID,
                            ImsStreamMediaProfile.RTT_MODE_DISABLED));
                }
                break;
            }
            case CallState.ACTIVE:
                mState = State.ESTABLISHED;
                if (!mEstablished) {
                    mEstablished = true;
                    if (l != null) l.callSessionInitiated(mProfile);
                } else if (previous == CallState.HELD && l != null) {
                    l.callSessionResumed(mProfile);
                }
                break;
            case CallState.HELD:
                if (previous != CallState.HELD && l != null) l.callSessionHeld(mProfile);
                break;
            case CallState.ENDED:
                ended(call);
                return;
            default:
                break;
        }
        if (call.remoteHeld != mRemoteHeld) {
            mRemoteHeld = call.remoteHeld;
            if (l != null) {
                if (mRemoteHeld)
                    l.callSessionHoldReceived(mProfile);
                else
                    l.callSessionResumeReceived(mProfile);
            }
        }
    }

    private void ended(Call call) {
        mState = State.TERMINATED;
        mFeature.untrack(mId);
        ImsCallSessionListener l = mListener;
        // SRVCC: the call goes on as a CS call, which telephony took over
        // when the radio HAL reported the handover
        if (call.handedOver || l == null) return;
        if (!mEstablished && call.retryOnCs) {
            l.callSessionInitiatedFailed(
                    new ImsReasonInfo(ImsReasonInfo.CODE_LOCAL_CALL_CS_RETRY_REQUIRED, 0));
            return;
        }
        ImsReasonInfo reason = new ImsReasonInfo(reasonCode(call), call.endStatus, call.endReason);
        if (!mEstablished && !call.incoming && !mLocalEnd)
            l.callSessionInitiatedFailed(reason);
        else
            l.callSessionTerminated(reason);
    }

    private int reasonCode(Call call) {
        if (call.endStatus == 0)
            return mLocalEnd ? ImsReasonInfo.CODE_USER_TERMINATED
                             : ImsReasonInfo.CODE_USER_TERMINATED_BY_REMOTE;
        switch (call.endStatus) {
            case 403: return ImsReasonInfo.CODE_SIP_FORBIDDEN;
            case 404: return ImsReasonInfo.CODE_SIP_NOT_FOUND;
            case 408: return ImsReasonInfo.CODE_SIP_REQUEST_TIMEOUT;
            case 480: return ImsReasonInfo.CODE_SIP_TEMPRARILY_UNAVAILABLE;
            case 486:
            case 600: return ImsReasonInfo.CODE_SIP_BUSY;
            case 487: return ImsReasonInfo.CODE_SIP_REQUEST_CANCELLED;
            case 488:
            case 606: return ImsReasonInfo.CODE_SIP_NOT_ACCEPTABLE;
            case 503: return ImsReasonInfo.CODE_SIP_SERVICE_UNAVAILABLE;
            case 603: return ImsReasonInfo.CODE_SIP_USER_REJECTED;
            default:
                if (call.endStatus >= 600) return ImsReasonInfo.CODE_SIP_GLOBAL_ERROR;
                if (call.endStatus >= 500) return ImsReasonInfo.CODE_SIP_SERVER_ERROR;
                if (call.endStatus >= 400) return ImsReasonInfo.CODE_SIP_CLIENT_ERROR;
                return ImsReasonInfo.CODE_UNSPECIFIED;
        }
    }
}
