// SPDX-License-Identifier: GPL-2.0-or-later
package vendor.shannond.ims;

/** The IMS registration, as the HAL's SIP stack has it */
@VintfStability
@Backing(type="int")
enum RegistrationState {
    /** Not registered and not trying (off, or waiting: see Registration.reason) */
    IDLE,
    REGISTERING,
    REGISTERED,
    DEREGISTERING,
}
