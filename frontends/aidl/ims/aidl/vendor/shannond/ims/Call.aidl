// SPDX-License-Identifier: GPL-2.0-or-later
package vendor.shannond.ims;

@VintfStability
parcelable Call {
    /** The HAL's id of the call, unique while the HAL runs */
    int id;
    vendor.shannond.ims.CallState state = vendor.shannond.ims.CallState.ENDED;
    boolean incoming;
    /** The other side: a number, else its SIP URI; empty if withheld */
    String number;
    /** The other side holds the call */
    boolean remoteHeld;
    /** Once ENDED: the SIP status that ended the call, 0 for a normal end */
    int endStatus;
    String endReason;
    /** Once ENDED: the call could not be set up on IMS and should be placed on CS */
    boolean retryOnCs;
    /** Once ENDED: moved to CS by the network (SRVCC); it continues as a CS call */
    boolean handedOver;
}
