package android.telephony.ims;
public final class ImsCallProfile { public static final int SERVICE_TYPE_NORMAL = 1, CALL_TYPE_VOICE = 2, OIR_PRESENTATION_RESTRICTED = 1, OIR_PRESENTATION_NOT_RESTRICTED = 2;
 public static final String EXTRA_OI = "oi", EXTRA_OIR = "oir"; public ImsCallProfile(int serviceType, int callType) {}
 public void setCallExtra(String name, String value) {} public void setCallExtraInt(String name, int value) {} }
