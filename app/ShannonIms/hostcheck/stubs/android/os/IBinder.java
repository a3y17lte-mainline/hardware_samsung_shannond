package android.os; public interface IBinder { interface DeathRecipient { void binderDied(); } void linkToDeath(DeathRecipient r, int flags) throws RemoteException; }
