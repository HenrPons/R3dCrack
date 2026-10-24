# Security policy

## Reporting

For a sensitive issue, use GitHub's private vulnerability reporting feature if enabled. Otherwise contact the repository owner privately through an available GitHub contact channel. Do not put private keys, WIFs, credentials or personal checkpoint files in public issues.

## Trust boundaries and remaining risks

- Results contain plaintext private keys/WIFs; the app does not encrypt them or clear clipboard history.
- Checkpoints/settings are local, user-controlled files. Atomic replacement and exclusive worker locks reduce accidental corruption; they are not a defense against malicious same-user processes.
- The GUI starts its sibling worker with an explicit executable path and no command shell. A replaced worker or DLL in an untrusted app directory can execute code as the user.
- IPC names are unpredictable and collision-checked, but this is not a sandbox or protection against a compromised Windows account.
- GPU vendor drivers and externally supplied build SDKs are trusted dependencies. They are external dependencies and are not bundled in the source.
- No network client, telemetry or result-upload functionality was added to the search application. Build/repository operations may use network tools outside it.
- Physical multi-device operation, NVIDIA hardware and high-DPI layouts require further validation; see README.
