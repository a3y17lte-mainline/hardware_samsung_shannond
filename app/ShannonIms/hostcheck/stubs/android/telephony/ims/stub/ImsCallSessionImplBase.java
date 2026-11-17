package android.telephony.ims.stub; import android.telephony.ims.*; import android.os.Message;
public class ImsCallSessionImplBase implements AutoCloseable {
 public static class State { public static final int IDLE = 0, INITIATED = 1, ESTABLISHED = 4, TERMINATING = 7, TERMINATED = 8; }
 @Deprecated public void setListener(ImsCallSessionListener listener) {} public void close() {}
 public String getCallId() { return null; } public ImsCallProfile getCallProfile() { return null; } public ImsCallProfile getLocalCallProfile() { return null; }
 public ImsCallProfile getRemoteCallProfile() { return null; } public int getState() { return 0; } public boolean isInCall() { return false; }
 public void setMute(boolean muted) {} public void start(String callee, ImsCallProfile profile) {} public void accept(int callType, ImsStreamMediaProfile profile) {}
 public void reject(int reason) {} public void terminate(int reason) {} public void hold(ImsStreamMediaProfile profile) {} public void resume(ImsStreamMediaProfile profile) {}
 public void sendDtmf(char c, Message result) {} public void startDtmf(char c) {} public void stopDtmf() {} }
