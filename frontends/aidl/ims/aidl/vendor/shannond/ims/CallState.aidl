// SPDX-License-Identifier: GPL-2.0-or-later
package vendor.shannond.ims;

@VintfStability
@Backing(type="int")
enum CallState {
    /** Outgoing, the far end does not ring yet */
    DIALING,
    /** Outgoing, the far end rings */
    ALERTING,
    /** Incoming, ringing here */
    INCOMING,
    ACTIVE,
    /** Held by us */
    HELD,
    ENDED,
}
