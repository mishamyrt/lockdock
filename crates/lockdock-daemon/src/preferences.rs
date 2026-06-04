use prefs::{Key, Preferences};

use crate::display_state::{display_identity_is_valid, DisplayIdentity};
use crate::Result;

const BUNDLE_ID: &str = "co.myrt.lockdock";

const PREFERRED_UUID: Key<String> = Key::new("preferredDisplayUUID");
const PREFERRED_BUILTIN: Key<bool> = Key::new("preferredDisplayBuiltin");
const PREFERRED_VENDOR: Key<i64> = Key::new("preferredDisplayVendor");
const PREFERRED_MODEL: Key<i64> = Key::new("preferredDisplayModel");
const PREFERRED_SERIAL: Key<i64> = Key::new("preferredDisplaySerial");

pub(crate) struct DisplayPreferences {
    preferences: Preferences,
}

impl DisplayPreferences {
    pub(crate) fn new() -> Result<Self> {
        Ok(Self {
            preferences: Preferences::new(BUNDLE_ID)?,
        })
    }

    pub(crate) fn save(&self, identity: &DisplayIdentity) -> Result<()> {
        self.preferences.set(PREFERRED_UUID, &identity.uuid)?;
        self.preferences
            .set(PREFERRED_BUILTIN, &identity.is_builtin)?;
        self.preferences
            .set(PREFERRED_VENDOR, &i64::from(identity.vendor_number))?;
        self.preferences
            .set(PREFERRED_MODEL, &i64::from(identity.model_number))?;
        self.preferences
            .set(PREFERRED_SERIAL, &i64::from(identity.serial_number))?;
        Ok(())
    }

    pub(crate) fn load(&self) -> Result<Option<DisplayIdentity>> {
        let uuid = self.preferences.get(PREFERRED_UUID)?.unwrap_or_default();
        let is_builtin = self.preferences.get(PREFERRED_BUILTIN)?.unwrap_or(false);
        let vendor_number = self.preferences.get(PREFERRED_VENDOR)?.unwrap_or(0);
        let model_number = self.preferences.get(PREFERRED_MODEL)?.unwrap_or(0);
        let serial_number = self.preferences.get(PREFERRED_SERIAL)?.unwrap_or(0);

        let identity = DisplayIdentity {
            is_builtin,
            vendor_number: u32::try_from(vendor_number).unwrap_or(0),
            model_number: u32::try_from(model_number).unwrap_or(0),
            serial_number: u32::try_from(serial_number).unwrap_or(0),
            uuid,
        };

        if display_identity_is_valid(&identity) {
            Ok(Some(identity))
        } else {
            Ok(None)
        }
    }

    pub(crate) fn clear(&self) -> Result<()> {
        self.preferences.remove(PREFERRED_UUID)?;
        self.preferences.remove(PREFERRED_BUILTIN)?;
        self.preferences.remove(PREFERRED_VENDOR)?;
        self.preferences.remove(PREFERRED_MODEL)?;
        self.preferences.remove(PREFERRED_SERIAL)?;
        Ok(())
    }
}
