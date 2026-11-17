package android.telephony.ims.stub; import android.net.Uri; import android.telephony.ims.ImsReasonInfo;
public class ImsRegistrationImplBase { public static final int REGISTRATION_TECH_LTE = 0;
 public final void onRegistered(int tech) {} public final void onRegistering(int tech) {} public final void onDeregistered(ImsReasonInfo info) {}
 public final void onSubscriberAssociatedUriChanged(Uri[] uris) {} }
