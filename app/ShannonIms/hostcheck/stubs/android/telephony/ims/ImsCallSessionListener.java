package android.telephony.ims;
public class ImsCallSessionListener { public void callSessionInitiating(ImsCallProfile p) {} public void callSessionInitiatingFailed(ImsReasonInfo r) {}
 public void callSessionProgressing(ImsStreamMediaProfile p) {} public void callSessionInitiated(ImsCallProfile p) {} public void callSessionInitiatedFailed(ImsReasonInfo r) {}
 public void callSessionTerminated(ImsReasonInfo r) {} public void callSessionHeld(ImsCallProfile p) {} public void callSessionHoldFailed(ImsReasonInfo r) {}
 public void callSessionHoldReceived(ImsCallProfile p) {} public void callSessionResumed(ImsCallProfile p) {} public void callSessionResumeFailed(ImsReasonInfo r) {}
 public void callSessionResumeReceived(ImsCallProfile p) {} }
