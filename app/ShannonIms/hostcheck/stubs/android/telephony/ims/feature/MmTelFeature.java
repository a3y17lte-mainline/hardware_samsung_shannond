package android.telephony.ims.feature;
import android.os.Bundle; import android.telephony.ims.*; import android.telephony.ims.stub.ImsCallSessionImplBase;
public class MmTelFeature extends ImsFeature {
 public static final int PROCESS_CALL_IMS = 0; public static final int PROCESS_CALL_CSFB = 1;
 public static class MmTelCapabilities extends Capabilities { public static final int CAPABILITY_TYPE_VOICE = 1;
  public MmTelCapabilities() {} public final void addCapabilities(int c) {} }
 public boolean queryCapabilityConfiguration(int capability, int radioTech) { return false; }
 public void changeEnabledCapabilities(CapabilityChangeRequest request, CapabilityCallbackProxy c) {}
 public ImsCallProfile createCallProfile(int callSessionType, int callType) { return null; }
 public ImsCallSessionImplBase createCallSession(ImsCallProfile profile) { return null; }
 public int shouldProcessCall(String[] numbers) { return 0; }
 public final void notifyCapabilitiesStatusChanged(MmTelCapabilities c) {}
 public final ImsCallSessionListener notifyIncomingCall(ImsCallSessionImplBase c, String callId, Bundle extras) { return null; }
 public void onFeatureRemoved() {} public void onFeatureReady() {} }
