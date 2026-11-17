package android.telephony.ims.feature;
public abstract class ImsFeature {
 public static final int FEATURE_MMTEL = 1; public static final int STATE_UNAVAILABLE = 0; public static final int STATE_READY = 2;
 public static class Capabilities {}
 protected static class CapabilityCallbackProxy {}
 public final void setFeatureState(int state) {}
 public abstract boolean queryCapabilityConfiguration(int capability, int radioTech);
 public abstract void changeEnabledCapabilities(CapabilityChangeRequest request, CapabilityCallbackProxy c);
 public abstract void onFeatureRemoved(); public abstract void onFeatureReady(); }
