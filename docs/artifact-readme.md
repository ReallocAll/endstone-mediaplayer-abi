# Endstone MediaPlayer ABI evidence

This overlay was generated only after fresh Windows and Linux Endstone/BDS processes loaded the matching probe, every current MediaPlayer ABI requirement resolved with allowed provenance, isolated MediaPlayer builds and CTest runs passed, and those rebuilt plugins passed the generated-header consumer runtime smoke on both platforms.

Copy the extracted root over a clean MediaPlayer checkout. The `include/abi` files replace the generated platform ABI headers. Files under `abi-evidence` document the exact MediaPlayer and Endstone identities, runtime reports, coverage, provenance, hashes, build/CTest gates, console-command message dispatch, logical-screen lifecycle, and clean shutdown evidence.
