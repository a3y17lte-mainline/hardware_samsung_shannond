package android.telephony.ims.stub;
public class ImsConfigImplBase { public static final int CONFIG_RESULT_UNKNOWN = -1; public static final int CONFIG_RESULT_SUCCESS = 0;
 public final void notifyProvisionedValueChanged(int item, int value) {} public final void notifyProvisionedValueChanged(int item, String value) {}
 public int setConfig(int item, int value) { return 0; } public int setConfig(int item, String value) { return 0; }
 public int getConfigInt(int item) { return 0; } public String getConfigString(int item) { return null; } }
