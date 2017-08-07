# ParticleElectronAssetTracker
Particle Electron w/ Asset Tracker designed to report positions and other data periodically

This based on libraries and examples provided by Particle.io and members.

Specifically this uses the following libraries from Particle: 
AssetTrackerRK
HttpClient

By default the ParticleElectronAssetTracker will report its position every 20s while moving and powered.  When unpowered it will report its position periodically with an increasing backoff up to 6 hours.

Position reported to a custom PHP script.

Particle's Cloud is not used, but there is a "Stay Awake" mode which can be used to force the device to stay awake despite being unpowered and also connect to the Particle Cloud for OTA FW updates.
