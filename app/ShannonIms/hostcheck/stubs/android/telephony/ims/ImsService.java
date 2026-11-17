package android.telephony.ims;
import android.telephony.ims.feature.MmTelFeature; import android.telephony.ims.stub.*;
public class ImsService extends android.app.Service {
 public ImsFeatureConfiguration querySupportedImsFeatures() { return null; }
 public void readyForFeatureCreation() {}
 public void enableImsForSubscription(int slotId, int subscriptionId) {}
 public void disableImsForSubscription(int slotId, int subscriptionId) {}
 public MmTelFeature createMmTelFeatureForSubscription(int slotId, int subscriptionId) { return null; }
 public ImsConfigImplBase getConfigForSubscription(int slotId, int subscriptionId) { return null; }
 public ImsRegistrationImplBase getRegistrationForSubscription(int slotId, int subscriptionId) { return null; } }
