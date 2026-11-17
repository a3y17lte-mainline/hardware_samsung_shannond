// SPDX-License-Identifier: GPL-2.0-or-later
package org.shannond.ims;

import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;

import vendor.shannond.ims.Call;
import vendor.shannond.ims.IShannonIms;
import vendor.shannond.ims.IShannonImsCallback;
import vendor.shannond.ims.Registration;

/**
 * The radio HAL's IMS interface. Connects when the HAL is up, again after it
 * restarted (and then switches IMS back on as it was), and hands the HAL's
 * callbacks to the listeners on the main thread.
 */
final class Hal {
    private static final String TAG = "ShannonIms";
    private static final String SERVICE = IShannonIms.DESCRIPTOR + "/default";

    interface Listener {
        void onConnected();
        /** The HAL died: its registration and calls are gone */
        default void onDisconnected() {}
        void onRegistrationChanged(Registration registration);
        void onCallChanged(Call call);
    }

    private static Hal sInstance;

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final List<Listener> mListeners = new ArrayList<>();
    private IShannonIms mHal;
    private boolean mEnabled;

    static synchronized Hal get() {
        if (sInstance == null) {
            sInstance = new Hal();
            sInstance.connect();
        }
        return sInstance;
    }

    private final IShannonImsCallback mCallback = new IShannonImsCallback.Stub() {
        @Override
        public void onRegistrationChanged(Registration registration) {
            mHandler.post(() -> {
                for (Listener l : new ArrayList<>(mListeners)) l.onRegistrationChanged(registration);
            });
        }

        @Override
        public void onCallChanged(Call call) {
            mHandler.post(() -> {
                for (Listener l : new ArrayList<>(mListeners)) l.onCallChanged(call);
            });
        }
    };

    private final IBinder.DeathRecipient mDeath = () -> mHandler.post(() -> {
        Log.w(TAG, "radio HAL gone, waiting for it");
        mHal = null;
        for (Listener l : new ArrayList<>(mListeners)) l.onDisconnected();
        connect();
    });

    void addListener(Listener listener) {
        mListeners.add(listener);
    }

    void removeListener(Listener listener) {
        mListeners.remove(listener);
    }

    /** The HAL, or null while it is not there */
    IShannonIms hal() {
        return mHal;
    }

    private void connect() {
        // The HAL is a VINTF service: this waits until it is up
        new Thread(() -> {
            IBinder binder = ServiceManager.waitForDeclaredService(SERVICE);
            if (binder == null) {
                Log.e(TAG, SERVICE + " is not declared");
                return;
            }
            mHandler.post(() -> attach(binder));
        }, "ShannonImsConnect").start();
    }

    private void attach(IBinder binder) {
        try {
            binder.linkToDeath(mDeath, 0);
            IShannonIms hal = IShannonIms.Stub.asInterface(binder);
            hal.setCallback(mCallback);
            mHal = hal;
            Log.i(TAG, "connected to " + SERVICE);
        } catch (RemoteException e) {
            Log.e(TAG, "cannot attach to the radio HAL", e);
            connect();
            return;
        }
        // The HAL starts with IMS off: the switch is ours (the HAL keeps it
        // while its modem is not up yet)
        setEnabled(mEnabled);
        for (Listener l : new ArrayList<>(mListeners)) l.onConnected();
    }

    /** The VoLTE switch, kept here for a HAL that restarts */
    void setEnabled(boolean enabled) {
        mEnabled = enabled;
        if (mHal == null) return;
        try {
            mHal.setEnabled(enabled);
        } catch (RemoteException | RuntimeException e) {
            Log.w(TAG, "setEnabled(" + enabled + ") failed", e);
        }
    }

    /** The HAL's calls that have not ended; empty while it is not there */
    Call[] calls() {
        if (mHal == null) return new Call[0];
        try {
            Call[] calls = mHal.getCalls();
            return calls != null ? calls : new Call[0];
        } catch (RemoteException | RuntimeException e) {
            return new Call[0];
        }
    }
    Registration registration() {
        if (mHal == null) return null;
        try {
            return mHal.getRegistration();
        } catch (RemoteException | RuntimeException e) {
            return null;
        }
    }
}
