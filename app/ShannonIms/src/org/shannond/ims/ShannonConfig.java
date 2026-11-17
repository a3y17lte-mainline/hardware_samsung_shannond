// SPDX-License-Identifier: GPL-2.0-or-later
package org.shannond.ims;

import android.telephony.ims.stub.ImsConfigImplBase;
import android.util.SparseArray;

/**
 * Provisioning values the framework sets and reads. The HAL's stack takes its
 * settings from its own operator table, so these are only kept.
 */
final class ShannonConfig extends ImsConfigImplBase {
    private final SparseArray<Object> mValues = new SparseArray<>();

    @Override
    public synchronized int setConfig(int item, int value) {
        mValues.put(item, value);
        notifyProvisionedValueChanged(item, value);
        return CONFIG_RESULT_SUCCESS;
    }

    @Override
    public synchronized int setConfig(int item, String value) {
        mValues.put(item, value);
        notifyProvisionedValueChanged(item, value);
        return CONFIG_RESULT_SUCCESS;
    }

    @Override
    public synchronized int getConfigInt(int item) {
        Object v = mValues.get(item);
        return v instanceof Integer ? (Integer) v : CONFIG_RESULT_UNKNOWN;
    }

    @Override
    public synchronized String getConfigString(int item) {
        Object v = mValues.get(item);
        return v instanceof String ? (String) v : null;
    }
}
