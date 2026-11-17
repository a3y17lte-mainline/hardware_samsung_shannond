package android.telephony.ims.feature; import java.util.List;
public final class CapabilityChangeRequest { public static class CapabilityPair { public int getCapability() { return 0; } public int getRadioTech() { return 0; } }
 public List<CapabilityPair> getCapabilitiesToEnable() { return null; } public List<CapabilityPair> getCapabilitiesToDisable() { return null; } }
