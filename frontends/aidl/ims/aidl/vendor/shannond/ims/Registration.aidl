// SPDX-License-Identifier: GPL-2.0-or-later
package vendor.shannond.ims;

@VintfStability
parcelable Registration {
    vendor.shannond.ims.RegistrationState state = vendor.shannond.ims.RegistrationState.IDLE;
    /** The VoLTE switch as the HAL has it (setEnabled) */
    boolean enabled;
    /** What IMS waits for or is doing, for logs only */
    String reason;
    /** The public identities the network associated (sip: and tel: URIs), while registered */
    String[] associatedUris;
}
